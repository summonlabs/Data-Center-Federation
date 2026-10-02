// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dcf::test {

// A real operating-system process. The federation boundary claims to federate
// independently governed sites, so at least one suite has to prove it with
// processes that share nothing but a socket, rather than with threads that share
// an address space.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ~ChildProcess();

  [[nodiscard]] static ChildProcess spawn(const std::string& executable,
                                          const std::vector<std::string>& arguments,
                                          const std::string& output_path);
  // True while the process has not exited.
  [[nodiscard]] bool running() const;
  // Ends the process the way a crash would: no shutdown, no flush, no goodbye.
  void kill();
  // Waits for exit and returns the status. Returns -1 if it was already reaped.
  int wait();
  [[nodiscard]] std::int64_t id() const noexcept { return id_; }
  [[nodiscard]] bool valid() const noexcept { return id_ != 0; }

 private:
  void reset() noexcept;

  std::int64_t id_{0};
  bool reaped_{false};
  int status_{-1};
  void* handle_{nullptr};
};

// Why the most recent spawn failed, in the words of the operating system.
[[nodiscard]] const std::string& last_spawn_error();

// Reads a whole file, or an empty string when it cannot be read.
[[nodiscard]] std::string read_text_file(const std::string& path);

// Waits until the file contains the marker, or the attempt budget is exhausted.
// The wait is bounded so that a daemon that never starts is reported as a failed
// expectation rather than hanging the suite: this is a readiness check against
// another process, not a timeout that turns a hang into a pass.
[[nodiscard]] bool wait_for_marker(const std::string& path, const std::string& marker,
                                   int attempts = 200, int delay_ms = 25);

}  // namespace dcf::test
