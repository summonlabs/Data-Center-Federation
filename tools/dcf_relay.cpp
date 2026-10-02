// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// dcf-relay forwards bytes between sites and a federation and can cut a chosen
// site off. It exists so that a communication partition is a real event on a
// real network path - the site's connection is closed and its bytes stop
// arriving - rather than a flag inside the process under test. The relay never
// parses the federation protocol, so it cannot become part of the semantics it
// is being used to test.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dcf/cli.hpp"
#include "dcf/hash.hpp"
#include "dcf/wire.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
#endif

namespace {

[[nodiscard]] NativeSocket as_native(std::uintptr_t handle) {
  return static_cast<NativeSocket>(handle);
}

void close_native(NativeSocket socket) {
  if (socket == kNoSocket) {
    return;
  }
#ifdef _WIN32
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

// The relay moves bytes itself rather than through the framed protocol, so it
// needs these two primitives. POSIX returns ssize_t and Winsock returns int;
// both are normalised to std::ptrdiff_t, and a count becomes a size only after
// it is known to be positive.
constexpr std::size_t kMaxSocketChunk = 1U << 20;

[[nodiscard]] std::ptrdiff_t receive_some(NativeSocket socket, char* data, std::size_t size) {
  const std::size_t wanted = std::min(size, kMaxSocketChunk);
#ifdef _WIN32
  return static_cast<std::ptrdiff_t>(::recv(socket, data, static_cast<int>(wanted), 0));
#else
  return static_cast<std::ptrdiff_t>(::recv(socket, data, wanted, 0));
#endif
}

[[nodiscard]] std::ptrdiff_t send_some(NativeSocket socket, const char* data, std::size_t size) {
  const std::size_t wanted = std::min(size, kMaxSocketChunk);
#ifdef _WIN32
  return static_cast<std::ptrdiff_t>(::send(socket, data, static_cast<int>(wanted), 0));
#else
  return static_cast<std::ptrdiff_t>(::send(socket, data, wanted, MSG_NOSIGNAL));
#endif
}

void shutdown_native(NativeSocket socket) {
  if (socket == kNoSocket) {
    return;
  }
#ifdef _WIN32
  ::shutdown(socket, SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

// One forwarded link. The descriptors live here and are closed exactly once, by
// the thread that owns them, after both directions have stopped. A partition
// shuts the link down, which wakes every blocked reader without freeing the
// descriptor: closing it from another thread while a reader is blocked on it is
// not safe, and the number can be handed to an unrelated connection.
struct Link {
  NativeSocket client{kNoSocket};
  NativeSocket upstream{kNoSocket};
  std::string token{};
  std::atomic<bool> stopped{false};
};

class Relay {
 public:
  Relay(dcf::wire::Address listen, dcf::wire::Address upstream, dcf::wire::Address control)
      : listen_(std::move(listen)), upstream_(std::move(upstream)), control_(std::move(control)) {}

  [[nodiscard]] bool start() {
    auto listener = dcf::wire::Listener::bind(listen_);
    if (!listener) {
      std::cerr << "error: " << listener.error().detail << "\n";
      return false;
    }
    data_ = std::make_unique<dcf::wire::Listener>(std::move(listener).value());
    auto control = dcf::wire::Listener::bind(control_);
    if (!control) {
      std::cerr << "error: " << control.error().detail << "\n";
      return false;
    }
    control_listener_ = std::move(control).value();

    const auto data_port = data_->port();
    const auto control_port = control_listener_.port();
    if (!data_port || !control_port) {
      std::cerr << "error: could not determine the relay ports\n";
      return false;
    }
    std::cout << "relay-data " << listen_.host << ":" << data_port.value() << "\n";
    std::cout << "relay-control " << control_.host << ":" << control_port.value() << "\n";
    std::cout << "upstream " << upstream_.to_string() << "\n";
    std::cout << "READY" << std::endl;

    control_thread_ = std::thread([this] { run_control(); });
    run_data();
    return true;
  }

  void stop() {
    quitting_.store(true);
    if (data_) {
      data_->close();
    }
    control_listener_.close();
    {
      std::lock_guard<std::mutex> guard(mutex_);
      for (auto& link : links_) {
        link->stopped.store(true);
        shutdown_native(link->client);
        shutdown_native(link->upstream);
      }
    }
    if (control_thread_.joinable()) {
      control_thread_.join();
    }
  }

 private:
  static void pump(NativeSocket from, NativeSocket to, const std::atomic<bool>& link_stopped,
                   std::atomic<bool>& stop) {
    std::vector<char> buffer(16384);
    while (!stop.load() && !link_stopped.load()) {
      const std::ptrdiff_t received = receive_some(from, buffer.data(), buffer.size());
      if (received <= 0) {
        break;
      }
      const std::size_t total = static_cast<std::size_t>(received);
      std::size_t sent = 0;
      while (sent < total) {
        const std::ptrdiff_t chunk = send_some(to, buffer.data() + sent, total - sent);
        if (chunk <= 0) {
          stop.store(true);
          shutdown_native(to);
          shutdown_native(from);
          return;
        }
        sent += static_cast<std::size_t>(chunk);
      }
    }
    stop.store(true);
    // Shutting both ends down makes the opposite pump return immediately, so a
    // partition really does stop the flow in both directions.
    shutdown_native(to);
    shutdown_native(from);
  }

  void run_data() {
    for (;;) {
      auto accepted = data_->accept();
      if (!accepted) {
        if (quitting_.load()) {
          return;
        }
        if (accepted.error().code == dcf::ErrorCode::Busy) {
          continue;
        }
        return;
      }
      std::thread([this, connection = std::move(accepted).value()]() mutable {
        handle_client(std::move(connection));
      }).detach();
    }
  }

  void handle_client(dcf::wire::Connection client) {
    const NativeSocket client_socket = as_native(client.handle());
    // The preamble identifies the connection to the relay and never reaches the
    // federation. A relay that cannot tell one site from another cannot
    // partition one site.
    std::string line;
    while (line.size() < 256) {
      char character = 0;
      const std::ptrdiff_t received = receive_some(client_socket, &character, 1);
      if (received <= 0) {
        client.close();
        return;
      }
      if (character == '\n') {
        break;
      }
      line.push_back(character);
    }
    if (line.rfind(dcf::wire::kRelayMagic, 0) != 0) {
      client.close();
      return;
    }
    const std::string token = line.substr(std::strlen(dcf::wire::kRelayMagic));
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (blocked_.count(token) > 0) {
        // A blocked site's connection is refused outright, so a reconnect during
        // a partition really does fail rather than being silently ignored.
        client.close();
        return;
      }
    }

    auto upstream = dcf::wire::connect(upstream_);
    if (!upstream) {
      client.close();
      return;
    }
    const NativeSocket upstream_socket = as_native(upstream.value().release_handle());
    static_cast<void>(client.release_handle());

    std::cout << "link " << token << " established" << std::endl;
    auto link = std::make_shared<Link>();
    link->client = client_socket;
    link->upstream = upstream_socket;
    link->token = token;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      links_.push_back(link);
    }

    std::atomic<bool> stop{false};
    std::thread back([link, &stop] {
      pump(link->upstream, link->client, link->stopped, stop);
    });
    pump(link->client, link->upstream, link->stopped, stop);
    back.join();

    // Both directions have stopped. The link leaves the registry before its
    // descriptors are closed, so no other thread can shut down a number that has
    // already been released, and then the descriptors are closed exactly once.
    {
      std::lock_guard<std::mutex> guard(mutex_);
      for (auto iterator = links_.begin(); iterator != links_.end(); ++iterator) {
        if (iterator->get() == link.get()) {
          links_.erase(iterator);
          break;
        }
      }
    }
    close_native(link->client);
    close_native(link->upstream);
    link->client = kNoSocket;
    link->upstream = kNoSocket;
  }

  void run_control() {
    for (;;) {
      auto accepted = control_listener_.accept();
      if (!accepted) {
        if (quitting_.load()) {
          return;
        }
        if (accepted.error().code == dcf::ErrorCode::Busy) {
          continue;
        }
        return;
      }
      handle_control(std::move(accepted).value());
    }
  }

  void handle_control(dcf::wire::Connection connection) {
    const NativeSocket socket = as_native(connection.handle());
    std::string pending;
    char buffer[512];
    for (;;) {
      const std::ptrdiff_t received = receive_some(socket, buffer, sizeof(buffer));
      if (received <= 0) {
        return;
      }
      pending.append(buffer, static_cast<std::size_t>(received));
      std::size_t newline = pending.find('\n');
      while (newline != std::string::npos) {
        std::string command = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        while (!command.empty() && command.back() == '\r') {
          command.pop_back();
        }
        const std::string reply = execute(command);
        if (reply == "BYE") {
          quitting_.store(true);
          if (data_) {
            data_->close();
          }
          control_listener_.close();
          return;
        }
        if (!reply.empty()) {
          std::string out = reply;
          out.push_back('\n');
          std::size_t sent = 0;
          while (sent < out.size()) {
            const std::ptrdiff_t chunk = send_some(socket, out.data() + sent, out.size() - sent);
            if (chunk <= 0) {
              return;
            }
            sent += static_cast<std::size_t>(chunk);
          }
        }
        newline = pending.find('\n');
      }
    }
  }

  [[nodiscard]] std::size_t link_count() {
    std::lock_guard<std::mutex> guard(mutex_);
    return links_.size();
  }

  void block(const std::string& token) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      blocked_.insert(token);
      for (auto& link : links_) {
        if (link->token == token) {
          // A partition is the flow stopping in both directions. The descriptors
          // stay owned by the thread reading them until it has stopped.
          link->stopped.store(true);
          shutdown_native(link->client);
          shutdown_native(link->upstream);
        }
      }
    }
    std::cout << "partition " << token << " (links " << link_count() << ")" << std::endl;
  }

  void allow(const std::string& token) {
    std::lock_guard<std::mutex> guard(mutex_);
    blocked_.erase(token);
    std::cout << "healed " << token << std::endl;
  }

  [[nodiscard]] std::string execute(const std::string& command) {
    std::size_t offset = 0;
    while (offset < command.size() && command[offset] == ' ') {
      ++offset;
    }
    const std::size_t space = command.find(' ', offset);
    const std::string verb =
        space == std::string::npos ? command.substr(offset) : command.substr(offset, space - offset);
    const std::string argument =
        space == std::string::npos ? std::string{} : command.substr(space + 1);

    if (verb == "BLOCK") {
      if (argument.empty()) {
        return "ERR BLOCK requires a token";
      }
      block(argument);
      return "OK blocked " + argument;
    }
    if (verb == "ALLOW") {
      if (argument.empty()) {
        return "ERR ALLOW requires a token";
      }
      allow(argument);
      return "OK allowed " + argument;
    }
    if (verb == "STATUS") {
      std::lock_guard<std::mutex> guard(mutex_);
      std::string text = "OK blocked=" + std::to_string(blocked_.size()) + " links=" +
                         std::to_string(links_.size());
      for (const std::string& token : blocked_) {
        text.append(" ").append(token);
      }
      return text;
    }
    if (verb == "QUIT") {
      return "BYE";
    }
    return "ERR unknown relay command";
  }

  dcf::wire::Address listen_{};
  dcf::wire::Address upstream_{};
  dcf::wire::Address control_{};
  std::unique_ptr<dcf::wire::Listener> data_{};
  dcf::wire::Listener control_listener_{};
  std::thread control_thread_{};
  std::mutex mutex_{};
  std::vector<std::shared_ptr<Link>> links_{};
  std::set<std::string> blocked_{};
  std::atomic<bool> quitting_{false};
};

}  // namespace

int main(int argc, char** argv) {
  const dcf::cli::OptionSet parsed = dcf::cli::parse_options(argc, argv, 1);
  if (parsed.has_flag("help")) {
    std::cout << "dcf-relay --listen HOST:PORT --upstream HOST:PORT --control HOST:PORT\n"
                 "  Control commands, one per line: BLOCK <token>, ALLOW <token>, STATUS, QUIT.\n";
    return 0;
  }
  const auto listen_text = parsed.require("listen");
  const auto upstream_text = parsed.require("upstream");
  const auto control_text = parsed.require("control");
  if (!listen_text || !upstream_text || !control_text) {
    std::cerr << "error: --listen, --upstream and --control are all required\n";
    return 2;
  }
  const auto listen = dcf::wire::parse_address(listen_text.value());
  const auto upstream = dcf::wire::parse_address(upstream_text.value());
  const auto control = dcf::wire::parse_address(control_text.value());
  if (!listen || !upstream || !control) {
    std::cerr << "error: an address is not in host:port form\n";
    return 2;
  }
  Relay relay(listen.value(), upstream.value(), control.value());
  if (!relay.start()) {
    return 3;
  }
  relay.stop();
  return 0;
}
