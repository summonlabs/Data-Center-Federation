// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dcf/types.hpp"

namespace dcf::platform {

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
// Every durability claim in this runtime is made through this interface, so
// there is exactly one place where "durable" is defined. On POSIX the boundary
// is fsync(2) on the file descriptor, followed by fsync on the containing
// directory after a rename. On Windows it is FlushFileBuffers on the handle;
// Windows has no directory-flush equivalent and the atomic replace itself is the
// boundary, which is stated plainly rather than papered over.

[[nodiscard]] Result<Ack> create_directories(const std::string& path);
[[nodiscard]] Result<bool> exists(const std::string& path);
[[nodiscard]] Result<std::uint64_t> file_size(const std::string& path);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path,
                                                          std::uint64_t max_bytes);
// Writes through a temporary file in the same directory, flushes it, and
// replaces the destination atomically. A reader never observes a partial file.
[[nodiscard]] Result<Ack> write_file_atomically(const std::string& path,
                                                std::span<const std::uint8_t> data);
[[nodiscard]] Result<Ack> remove_file(const std::string& path) noexcept;
[[nodiscard]] Result<std::vector<std::string>> list_directory(const std::string& path);
// Flushes the directory entry itself where the platform supports it.
[[nodiscard]] Result<Ack> sync_directory(const std::string& path);

// An append-only file with an explicit durability boundary. append() writes;
// only flush() makes the bytes durable, and callers are expected to say which
// one they mean.
class AppendFile {
 public:
  AppendFile() = default;
  AppendFile(const AppendFile&) = delete;
  AppendFile& operator=(const AppendFile&) = delete;
  AppendFile(AppendFile&& other) noexcept;
  AppendFile& operator=(AppendFile&& other) noexcept;
  ~AppendFile();

  [[nodiscard]] static Result<AppendFile> open(const std::string& path);

  [[nodiscard]] Result<Ack> append(std::span<const std::uint8_t> bytes);
  // The durability boundary.
  [[nodiscard]] Result<Ack> flush();
  [[nodiscard]] Result<Ack> truncate_to(std::uint64_t size);
  [[nodiscard]] Result<Ack> close();

  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  void reset() noexcept;

  std::FILE* file_{nullptr};
  std::uint64_t size_{0};
  std::string path_{};
};

// An exclusive advisory lock on a file. The lock is released when the process
// exits, even if it exits abnormally, so a crashed process cannot leave a lock
// that has to be removed by hand.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  ~FileLock();

  [[nodiscard]] static Result<FileLock> acquire(const std::string& path);

  [[nodiscard]] bool held() const noexcept { return handle_ != nullptr; }
  void release() noexcept;

 private:
  void* handle_{nullptr};
  std::string path_{};
};

// True when the path names a directory that can be written to.
[[nodiscard]] bool is_writable_directory(const std::string& path) noexcept;

}  // namespace dcf::platform
