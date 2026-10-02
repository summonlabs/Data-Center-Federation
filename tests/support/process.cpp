// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "process.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <spawn.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace dcf::test {
namespace {

std::string& spawn_error_slot() {
  static std::string message;
  return message;
}

void sleep_ms(int millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

#ifdef _WIN32
// CreateProcess does not resolve an application path written with forward
// slashes, so the executable path is normalised before it is handed over.
[[nodiscard]] std::string native_path(const std::string& path) {
  std::string out = path;
  for (char& character : out) {
    if (character == '/') {
      character = '\\';
    }
  }
  return out;
}

[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring{};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

[[nodiscard]] std::string quote(const std::string& argument) {
  std::string out = "\"";
  for (const char character : argument) {
    if (character == '"') {
      out.push_back('\\');
    }
    out.push_back(character);
  }
  out.push_back('"');
  return out;
}
#endif

}  // namespace

const std::string& last_spawn_error() { return spawn_error_slot(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : id_(other.id_), reaped_(other.reaped_), status_(other.status_), handle_(other.handle_) {
  other.id_ = 0;
  other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    reset();
    id_ = other.id_;
    reaped_ = other.reaped_;
    status_ = other.status_;
    handle_ = other.handle_;
    other.id_ = 0;
    other.handle_ = nullptr;
  }
  return *this;
}

ChildProcess::~ChildProcess() { reset(); }

void ChildProcess::reset() noexcept {
  if (id_ == 0) {
    return;
  }
  if (!reaped_) {
    kill();
    static_cast<void>(wait());
  }
#ifdef _WIN32
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#endif
  id_ = 0;
}

ChildProcess ChildProcess::spawn(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::string& output_path) {
  ChildProcess child;
  spawn_error_slot().clear();
#ifdef _WIN32
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  const std::wstring wide_output = widen(output_path);
  HANDLE output = CreateFileW(wide_output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    spawn_error_slot() = "could not create the output file (error " +
                         std::to_string(GetLastError()) + ")";
    return child;
  }
  const std::wstring nul = widen("NUL");
  HANDLE input = CreateFileW(nul.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::string command = quote(native_path(executable));
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote(argument));
  }
  std::wstring wide_command = widen(command);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = input;

  PROCESS_INFORMATION information{};
  const BOOL created =
      CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                     nullptr, nullptr, &startup, &information);
  CloseHandle(output);
  if (input != INVALID_HANDLE_VALUE) {
    CloseHandle(input);
  }
  if (created == FALSE) {
    spawn_error_slot() = "CreateProcess failed with error " + std::to_string(GetLastError()) +
                         " for '" + command + "'";
    return child;
  }
  CloseHandle(information.hThread);
  child.id_ = static_cast<std::int64_t>(information.dwProcessId);
  child.handle_ = information.hProcess;
  return child;
#else
  std::vector<std::string> owned;
  owned.push_back(executable);
  for (const std::string& argument : arguments) {
    owned.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  for (std::string& item : owned) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, output_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);

  pid_t pid = 0;
  const int result =
      posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result != 0) {
    spawn_error_slot() = "posix_spawn failed with error " + std::to_string(result);
    return child;
  }
  child.id_ = static_cast<std::int64_t>(pid);
  return child;
#endif
}

bool ChildProcess::running() const {
  if (id_ == 0 || reaped_) {
    return false;
  }
#ifdef _WIN32
  return WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(id_), &status, WNOHANG);
  return result == 0;
#endif
}

void ChildProcess::kill() {
  if (id_ == 0 || reaped_) {
    return;
  }
#ifdef _WIN32
  TerminateProcess(static_cast<HANDLE>(handle_), 1);
#else
  ::kill(static_cast<pid_t>(id_), SIGKILL);
#endif
}

int ChildProcess::wait() {
  if (id_ == 0) {
    return -1;
  }
  if (reaped_) {
    return status_;
  }
#ifdef _WIN32
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
  status_ = static_cast<int>(code);
#else
  int status = 0;
  static_cast<void>(waitpid(static_cast<pid_t>(id_), &status, 0));
  status_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  reaped_ = true;
  return status_;
}

std::string read_text_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

bool wait_for_marker(const std::string& path, const std::string& marker, int attempts,
                     int delay_ms) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const std::string text = read_text_file(path);
    if (text.find(marker) != std::string::npos) {
      return true;
    }
    sleep_ms(delay_ms);
  }
  return false;
}

}  // namespace dcf::test
