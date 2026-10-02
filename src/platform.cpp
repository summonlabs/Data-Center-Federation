// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/platform.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "dcf/hash.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace dcf::platform {
namespace {

namespace fs = std::filesystem;

// Every platform call resolves its path first. A relative path cannot carry the
// Windows long-path prefix, so a long relative path would fail for a reason that
// has nothing to do with the operation being attempted.
[[nodiscard]] std::string resolved(const std::string& path) {
  std::error_code code;
  fs::path candidate(path);
  if (candidate.is_relative()) {
    const fs::path absolute = fs::absolute(candidate, code);
    if (!code) {
      candidate = absolute;
    }
  }
  // A path carrying the long-path prefix must use the native separator and must
  // be free of "." and ".." components, because the prefix switches off the
  // normalisation that would otherwise deal with them.
  candidate = candidate.lexically_normal();
  candidate.make_preferred();
  return candidate.string();
}

[[nodiscard]] std::string describe_error(int code) {
  return std::error_code(code, std::generic_category()).message();
}

[[nodiscard]] Error io_error(const char* what, const std::string& path, int code) {
  std::string detail = what;
  detail.append(" failed for '");
  detail.append(sanitize_for_terminal(path));
  detail.append("': ");
  detail.append(describe_error(code));
  return make_error(ErrorCode::Io, std::move(detail));
}

#ifndef _WIN32

[[nodiscard]] Result<int> open_descriptor(const std::string& path, int flags, int mode) {
  const int descriptor = ::open(path.c_str(), flags, mode);
  if (descriptor < 0) {
    return io_error("open", path, errno);
  }
  return descriptor;
}

#endif

#ifdef _WIN32

// Windows rejects a path longer than MAX_PATH unless it carries the long-path
// prefix, which also switches off the normalisation that would rewrite '/'.
[[nodiscard]] std::wstring to_wide(const std::string& path) {
  std::string native = path;
  for (char& character : native) {
    if (character == '/') {
      character = '\\';
    }
  }
  // The \\?\ prefix removes the legacy path length limit and skips the
  // normalisation that would otherwise reject a long name outright.
  std::string prefixed;
  if (native.size() >= 2 && native[1] == ':' && native[0] != '\\') {
    prefixed = "\\\\?\\" + native;
  } else if (native.rfind("\\\\", 0) == 0 && native.rfind("\\\\?\\", 0) != 0) {
    prefixed = "\\\\?\\UNC\\" + native.substr(2);
  } else {
    prefixed = native;
  }
  return std::wstring(prefixed.begin(), prefixed.end());
}

// std::filesystem on Windows only applies the long-path prefix to paths it can
// normalise itself, and it does not do so for every operation. The store creates
// its own directories, so it creates them component by component with the prefix
// in place, which is the only way a path beyond MAX_PATH is guaranteed to work.
[[nodiscard]] Result<Ack> create_directories_native(const std::string& path) {
  const std::wstring wide = to_wide(path);
  if (wide.size() < 4 || wide.compare(0, 4, L"\\\\?\\") != 0) {
    return make_error(ErrorCode::Io,
                      "an internal path was not converted for long-path use: " +
                          sanitize_for_terminal(path));
  }
  // The prefix is "\\?\" followed by either "C:" or "UNC\server\share".
  std::size_t start = 6;
  if (wide.compare(4, 4, L"UNC\\") == 0) {
    std::size_t separators = 0;
    std::size_t index = 8;
    for (; index < wide.size() && separators < 2; ++index) {
      if (wide[index] == L'\\') {
        ++separators;
      }
    }
    start = index;
  }
  for (std::size_t index = start + 1; index <= wide.size(); ++index) {
    if (index != wide.size() && wide[index] != L'\\') {
      continue;
    }
    const std::wstring component = wide.substr(0, index);
    if (CreateDirectoryW(component.c_str(), nullptr) == FALSE) {
      const DWORD error = GetLastError();
      if (error != ERROR_ALREADY_EXISTS) {
        std::string detail = "could not create directory '";
        detail.append(sanitize_for_terminal(path));
        detail.append("': Windows error ");
        detail.append(std::to_string(error));
        return make_error(ErrorCode::Io, std::move(detail));
      }
    }
  }
  return Ack{};
}
// The same long-path reasoning applies to every other file operation: the
// standard library is not guaranteed to apply the prefix, so the store asks the
// platform directly.
[[nodiscard]] Result<bool> exists_native(const std::string& path) {
  const std::wstring wide = to_wide(path);
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return false;
    }
    std::string detail = "could not inspect '";
    detail.append(sanitize_for_terminal(path));
    detail.append("': Windows error ");
    detail.append(std::to_string(error));
    return make_error(ErrorCode::Io, std::move(detail));
  }
  return true;
}

[[nodiscard]] Result<std::uint64_t> size_native(const std::string& path) {
  const std::wstring wide = to_wide(path);
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data) == FALSE) {
    std::string detail = "could not size '";
    detail.append(sanitize_for_terminal(path));
    detail.append("': Windows error ");
    detail.append(std::to_string(GetLastError()));
    return make_error(ErrorCode::Io, std::move(detail));
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  return size;
}

[[nodiscard]] Result<std::vector<std::string>> list_native(const std::string& path) {
  std::wstring pattern = to_wide(path);
  if (!pattern.empty() && pattern.back() != L'\\') {
    pattern.push_back(L'\\');
  }
  pattern.push_back(L'*');
  WIN32_FIND_DATAW found{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &found);
  if (handle == INVALID_HANDLE_VALUE) {
    std::string detail = "could not list '";
    detail.append(sanitize_for_terminal(path));
    detail.append("': Windows error ");
    detail.append(std::to_string(GetLastError()));
    return make_error(ErrorCode::Io, std::move(detail));
  }
  std::vector<std::string> entries;
  for (;;) {
    const std::wstring name(found.cFileName);
    if (name != L"." && name != L"..") {
      const int length = WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                                             static_cast<int>(name.size()), nullptr, 0,
                                             nullptr, nullptr);
      std::string utf8(static_cast<std::size_t>(length), '\0');
      WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()),
                          utf8.data(), length, nullptr, nullptr);
      entries.push_back(std::move(utf8));
    }
    if (FindNextFileW(handle, &found) == FALSE) {
      break;
    }
  }
  FindClose(handle);
  std::sort(entries.begin(), entries.end());
  return entries;
}

// _wfopen is the only way to open a path that the \\?\ prefix is needed for,
// and the safe variant is used so that no deprecation is suppressed.
[[nodiscard]] FILE* open_file_utf8(const std::string& path, const char* mode) {
  FILE* file = nullptr;
  const std::wstring wide = to_wide(path);
  const std::wstring wide_mode(mode, mode + std::strlen(mode));
  if (::_wfopen_s(&file, wide.c_str(), wide_mode.c_str()) != 0) {
    return nullptr;
  }
  return file;
}

#else

[[nodiscard]] FILE* open_file_utf8(const std::string& path, const char* mode) {
  return std::fopen(path.c_str(), mode);
}

#endif

}  // namespace

Result<Ack> create_directories(const std::string& path) {
  const std::string full = resolved(path);
#ifdef _WIN32
  return create_directories_native(full);
#else
  std::error_code code;
  if (fs::exists(full, code)) {
    if (!fs::is_directory(full, code)) {
      return make_error(ErrorCode::Io, "path '" + sanitize_for_terminal(path) +
                                           "' exists and is not a directory");
    }
    return Ack{};
  }
  fs::create_directories(full, code);
  if (code) {
    return make_error(ErrorCode::Io, "could not create directory '" +
                                         sanitize_for_terminal(path) + "': " + code.message());
  }
  return Ack{};
#endif
}

Result<bool> exists(const std::string& path) {
#ifdef _WIN32
  return exists_native(resolved(path));
#else
  std::error_code code;
  const bool present = fs::exists(resolved(path), code);
  if (code) {
    return make_error(ErrorCode::Io,
                      "could not stat '" + sanitize_for_terminal(path) + "': " + code.message());
  }
  return present;
#endif
}

Result<std::uint64_t> file_size(const std::string& path) {
#ifdef _WIN32
  return size_native(resolved(path));
#else
  std::error_code code;
  const auto size = fs::file_size(resolved(path), code);
  if (code) {
    return make_error(ErrorCode::Io,
                      "could not size '" + sanitize_for_terminal(path) + "': " + code.message());
  }
  return static_cast<std::uint64_t>(size);
#endif
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::uint64_t max_bytes) {
  const auto size = file_size(path);
  if (!size) {
    return size.error();
  }
  if (size.value() > max_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      "file '" + sanitize_for_terminal(path) + "' is " +
                          std::to_string(size.value()) + " bytes which exceeds the limit of " +
                          std::to_string(max_bytes));
  }
  FILE* file = open_file_utf8(resolved(path), "rb");
  if (file == nullptr) {
    return io_error("open", path, errno);
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size.value()));
  std::size_t read = 0;
  if (!data.empty()) {
    read = std::fread(data.data(), 1, data.size(), file);
    if (read != data.size()) {
      std::fclose(file);
      return make_error(ErrorCode::Io, "short read from '" + sanitize_for_terminal(path) +
                                           "': expected " + std::to_string(data.size()) +
                                           " bytes and read " + std::to_string(read));
    }
  }
  std::fclose(file);
  return data;
}

Result<Ack> write_file_atomically(const std::string& path, std::span<const std::uint8_t> data) {
  const std::string temporary = path + ".tmp";
  {
    FILE* file = open_file_utf8(resolved(temporary), "wb");
    if (file == nullptr) {
      return io_error("create", temporary, errno);
    }
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file) != data.size()) {
      const int failure = errno;
      std::fclose(file);
      return io_error("write", temporary, failure);
    }
    // The temporary must reach storage before the rename, otherwise a crash can
    // leave the destination pointing at a name whose contents were never
    // written.
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
      const int failure = errno;
      std::fclose(file);
      return io_error("flush", temporary, failure);
    }
#else
    if (::fflush(file) != 0 || ::fsync(::fileno(file)) != 0) {
      const int failure = errno;
      std::fclose(file);
      return io_error("flush", temporary, failure);
    }
#endif
    if (std::fclose(file) != 0) {
      return io_error("close", temporary, errno);
    }
  }

#ifdef _WIN32
  const std::wstring from = to_wide(resolved(temporary));
  const std::wstring to = to_wide(resolved(path));
  if (MoveFileExW(from.c_str(), to.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    const DWORD error = GetLastError();
    static_cast<void>(DeleteFileW(from.c_str()));
    std::string detail = "could not replace '";
    detail.append(sanitize_for_terminal(path));
    detail.append("': Windows error ");
    detail.append(std::to_string(error));
    return make_error(ErrorCode::Io, std::move(detail));
  }
#else
  std::error_code code;
  fs::rename(resolved(temporary), resolved(path), code);
  if (code) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return make_error(ErrorCode::Io, "could not replace '" + sanitize_for_terminal(path) +
                                         "': " + code.message());
  }
#endif

  const auto directory = fs::path(resolved(path)).parent_path();
  if (!directory.empty()) {
    return sync_directory(directory.string());
  }
  return Ack{};
}

Result<Ack> remove_file(const std::string& path) noexcept {
#ifdef _WIN32
  const std::wstring wide = to_wide(resolved(path));
  if (DeleteFileW(wide.c_str()) == FALSE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Ack{};
    }
    std::string detail = "could not remove '";
    detail.append(sanitize_for_terminal(path));
    detail.append("': Windows error ");
    detail.append(std::to_string(error));
    return make_error(ErrorCode::Io, std::move(detail));
  }
  return Ack{};
#else
  std::error_code code;
  fs::remove(resolved(path), code);
  if (code) {
    return make_error(ErrorCode::Io,
                      "could not remove '" + sanitize_for_terminal(path) + "': " + code.message());
  }
  return Ack{};
#endif
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
#ifdef _WIN32
  return list_native(resolved(path));
#else
  std::vector<std::string> entries;
  std::error_code code;
  fs::directory_iterator iterator(resolved(path), code);
  if (code) {
    return make_error(ErrorCode::Io, "could not list '" + sanitize_for_terminal(path) +
                                         "': " + code.message());
  }
  for (const fs::directory_entry& entry : iterator) {
    entries.push_back(entry.path().filename().string());
  }
  std::sort(entries.begin(), entries.end());
  return entries;
#endif
}

Result<Ack> sync_directory(const std::string& path) {
#ifndef _WIN32
  const auto descriptor = open_descriptor(resolved(path), O_RDONLY, 0);
  if (!descriptor) {
    return descriptor.error();
  }
  const int result = ::fsync(descriptor.value());
  const int failure = errno;
  ::close(descriptor.value());
  if (result != 0) {
    return io_error("fsync on directory", path, failure);
  }
#else
  // Windows has no operation that flushes a directory entry. The atomic replace
  // performed by MoveFileEx is the durability boundary for the name, and the
  // file contents were already flushed before the replace.
  (void)path;
#endif
  return Ack{};
}

AppendFile::AppendFile(AppendFile&& other) noexcept
    : file_(other.file_), size_(other.size_), path_(std::move(other.path_)) {
  other.file_ = nullptr;
  other.size_ = 0;
}

AppendFile& AppendFile::operator=(AppendFile&& other) noexcept {
  if (this != &other) {
    reset();
    file_ = other.file_;
    size_ = other.size_;
    path_ = std::move(other.path_);
    other.file_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

AppendFile::~AppendFile() { reset(); }

void AppendFile::reset() noexcept {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  size_ = 0;
}

Result<AppendFile> AppendFile::open(const std::string& path) {
  FILE* file = open_file_utf8(resolved(path), "ab+");
  if (file == nullptr) {
    return io_error("open", path, errno);
  }
  AppendFile result;
  result.file_ = file;
  result.path_ = path;
  // The public sizing helper is used rather than the standard library directly,
  // so that a long path is handled the same way here as everywhere else.
  const auto size = file_size(path);
  if (!size) {
    std::fclose(file);
    result.file_ = nullptr;
    return size.error();
  }
  result.size_ = size.value();
  return result;
}

Result<Ack> AppendFile::append(std::span<const std::uint8_t> bytes) {
  if (file_ == nullptr) {
    return make_error(ErrorCode::Closed, "the append file is not open");
  }
  if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
    return io_error("write", path_, errno);
  }
  size_ += static_cast<std::uint64_t>(bytes.size());
  return Ack{};
}

Result<Ack> AppendFile::flush() {
  if (file_ == nullptr) {
    return make_error(ErrorCode::Closed, "the append file is not open");
  }
  if (std::fflush(file_) != 0) {
    return io_error("fflush", path_, errno);
  }
#ifdef _WIN32
  if (_commit(_fileno(file_)) != 0) {
    return io_error("commit", path_, errno);
  }
#else
  if (::fsync(::fileno(file_)) != 0) {
    return io_error("fsync", path_, errno);
  }
#endif
  return Ack{};
}

Result<Ack> AppendFile::truncate_to(std::uint64_t size) {
  if (file_ == nullptr) {
    return make_error(ErrorCode::Closed, "the append file is not open");
  }
  if (size > size_) {
    return make_error(ErrorCode::InvalidArgument,
                      "refusing to extend '" + sanitize_for_terminal(path_) + "' by truncation");
  }
#ifdef _WIN32
  if (_chsize_s(_fileno(file_), static_cast<long long>(size)) != 0) {
    return io_error("truncate", path_, errno);
  }
#else
  if (::ftruncate(::fileno(file_), static_cast<off_t>(size)) != 0) {
    return io_error("truncate", path_, errno);
  }
#endif
  size_ = size;
  return flush();
}

Result<Ack> AppendFile::close() {
  if (file_ == nullptr) {
    return Ack{};
  }
  const std::string path = path_;
  const int result = std::fclose(file_);
  file_ = nullptr;
  if (result != 0) {
    return io_error("close", path, errno);
  }
  return Ack{};
}

FileLock::FileLock(FileLock&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
  other.handle_ = nullptr;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    other.handle_ = nullptr;
  }
  return *this;
}

FileLock::~FileLock() { release(); }

void FileLock::release() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#ifdef _WIN32
  auto* handle = static_cast<HANDLE>(handle_);
  OVERLAPPED overlapped{};
  UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
  CloseHandle(handle);
#else
  const int descriptor = *static_cast<int*>(handle_);
  ::flock(descriptor, LOCK_UN);
  ::close(descriptor);
  delete static_cast<int*>(handle_);
#endif
  handle_ = nullptr;
}

Result<FileLock> FileLock::acquire(const std::string& path) {
  FileLock lock;
  lock.path_ = path;
#ifdef _WIN32
  const std::wstring wide = to_wide(resolved(path));
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return make_error(ErrorCode::Locked,
                      "another process holds the store lock at '" +
                          sanitize_for_terminal(path) + "' (error " +
                          std::to_string(GetLastError()) + ")");
  }
  OVERLAPPED overlapped{};
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                 MAXDWORD, &overlapped) == 0) {
    CloseHandle(handle);
    return make_error(ErrorCode::Locked,
                      "another process holds the store lock at '" +
                          sanitize_for_terminal(path) + "'");
  }
  lock.handle_ = handle;
#else
  const auto descriptor = open_descriptor(resolved(path), O_RDWR | O_CREAT, 0644);
  if (!descriptor) {
    return descriptor.error();
  }
  if (::flock(descriptor.value(), LOCK_EX | LOCK_NB) != 0) {
    ::close(descriptor.value());
    return make_error(ErrorCode::Locked,
                      "another process holds the store lock at '" +
                          sanitize_for_terminal(path) + "'");
  }
  lock.handle_ = new int(descriptor.value());
#endif
  return lock;
}

bool is_writable_directory(const std::string& path) noexcept {
  const std::string full = resolved(path);
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(to_wide(full).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return false;
  }
#else
  std::error_code code;
  if (!fs::is_directory(full, code)) {
    return false;
  }
#endif
  const fs::path probe = fs::path(full) / ".dcf-write-probe";
  {
    FILE* file = open_file_utf8(probe.string(), "wb");
    if (file == nullptr) {
      return false;
    }
    std::fclose(file);
  }
  static_cast<void>(remove_file(probe.string()));
  return true;
}

}  // namespace dcf::platform
