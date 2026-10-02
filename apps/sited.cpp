// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// dcf-sited hosts one site. It owns its own local authority - its local epoch
// and the work it has accepted locally - and it is the only writer of that
// state. The federation never reaches into it, and while the link is down the
// site keeps working.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dcf/cli.hpp"
#include "dcf/json.hpp"
#include "dcf/platform.hpp"
#include "dcf/version.hpp"
#include "dcf/wire.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

// The relay preamble and the local control port are byte level, so this file
// needs two raw helpers rather than the framed protocol.
[[nodiscard]] int recv_raw(std::uintptr_t handle, char* buffer, int size) {
#ifdef _WIN32
  return ::recv(static_cast<SOCKET>(handle), buffer, size, 0);
#else
  return static_cast<int>(
      ::recv(static_cast<int>(handle), buffer, static_cast<std::size_t>(size), 0));
#endif
}

[[nodiscard]] bool send_raw(std::uintptr_t handle, const std::string& text) {
  std::size_t sent = 0;
  while (sent < text.size()) {
#ifdef _WIN32
    const int chunk = ::send(static_cast<SOCKET>(handle), text.data() + sent,
                             static_cast<int>(text.size() - sent), 0);
#else
    const auto chunk = ::send(static_cast<int>(handle), text.data() + sent, text.size() - sent,
                              MSG_NOSIGNAL);
#endif
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

struct Options {
  dcf::SiteId site{};
  std::string display_name{};
  dcf::wire::Address endpoint{"127.0.0.1", 0};
  bool through_relay{false};
  std::vector<std::string> capabilities{};
  dcf::VersionRange protocol{dcf::Version{1, 0}, dcf::Version{9, 9}};
  std::string implementation{"summon.site-runtime"};
  std::string local_state{};
  dcf::wire::Address control{"127.0.0.1", 0};
  bool has_control{false};
  std::uint32_t deadline_ms{30000};
  std::uint32_t reconnect_ms{250};
};

// What the site owns. It is written by the site and by nobody else, and it is
// durable so that a restart of the site process does not lose the site's own
// history.
struct LocalState {
  dcf::SiteLocalEpoch epoch{};
  dcf::AcceptedGeneration accepted{};
  dcf::Digest accepted_history{};
  dcf::MembershipGeneration membership{};
  bool registered{false};

  [[nodiscard]] std::string encode() const {
    std::string text;
    text.append("epoch ").append(std::to_string(epoch.value())).push_back('\n');
    text.append("accepted ").append(std::to_string(accepted.value())).push_back('\n');
    text.append("history ").append(accepted_history.to_hex()).push_back('\n');
    text.append("membership ").append(std::to_string(membership.value())).push_back('\n');
    text.append("registered ").append(registered ? "1" : "0").push_back('\n');
    return text;
  }

  [[nodiscard]] static LocalState decode(const std::string& text) {
    LocalState state;
    std::size_t offset = 0;
    while (offset < text.size()) {
      const std::size_t end = text.find('\n', offset);
      const std::size_t stop = end == std::string::npos ? text.size() : end;
      const std::string line = text.substr(offset, stop - offset);
      const std::size_t space = line.find(' ');
      if (space != std::string::npos) {
        const std::string key = line.substr(0, space);
        const std::string value = line.substr(space + 1);
        if (key == "epoch") {
          const auto parsed = dcf::cli::parse_u64(value, "local epoch");
          if (parsed) {
            state.epoch = dcf::SiteLocalEpoch{parsed.value()};
          }
        } else if (key == "accepted") {
          const auto parsed = dcf::cli::parse_u64(value, "accepted generation");
          if (parsed) {
            state.accepted = dcf::AcceptedGeneration{parsed.value()};
          }
        } else if (key == "history") {
          const auto parsed = dcf::Digest::from_hex(value);
          if (parsed) {
            state.accepted_history = parsed.value();
          }
        } else if (key == "membership") {
          const auto parsed = dcf::cli::parse_u64(value, "membership generation");
          if (parsed) {
            state.membership = dcf::MembershipGeneration{parsed.value()};
          }
        } else if (key == "registered") {
          state.registered = value == "1";
        }
      }
      if (end == std::string::npos) {
        break;
      }
      offset = end + 1;
    }
    return state;
  }
};

struct Site {
  Options options{};
  LocalState local{};
  std::mutex mutex{};
  std::mutex writer{};

  void persist() {
    if (options.local_state.empty()) {
      return;
    }
    std::lock_guard<std::mutex> guard(writer);
    const std::string text = local.encode();
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    static_cast<void>(dcf::platform::write_file_atomically(options.local_state, bytes));
  }

  void load() {
    if (options.local_state.empty()) {
      return;
    }
    const auto present = dcf::platform::exists(options.local_state);
    if (!present || !present.value()) {
      return;
    }
    const auto raw = dcf::platform::read_file(options.local_state, 65536);
    if (!raw) {
      return;
    }
    local = LocalState::decode(std::string(raw.value().begin(), raw.value().end()));
  }

  [[nodiscard]] std::uint64_t advance_epoch() {
    std::lock_guard<std::mutex> guard(mutex);
    local.epoch = dcf::SiteLocalEpoch{local.epoch.value() + 1};
    const std::uint64_t value = local.epoch.value();
    persist();
    return value;
  }

  [[nodiscard]] LocalState snapshot() {
    std::lock_guard<std::mutex> guard(mutex);
    return local;
  }

  void adopt(dcf::AcceptedGeneration generation, const dcf::Digest& history) {
    std::lock_guard<std::mutex> guard(mutex);
    local.accepted = generation;
    if (!history.is_zero()) {
      local.accepted_history = history;
    }
    persist();
  }
};

[[nodiscard]] dcf::CompatibilityDeclaration declaration_for(const Options& options) {
  dcf::CompatibilityDeclaration declaration;
  declaration.implementation = options.implementation;
  declaration.implementation_version = dcf::Version{1, 0};
  declaration.protocol = options.protocol;
  for (const std::string& name : options.capabilities) {
    dcf::CapabilityDeclaration entry;
    entry.id = dcf::CapabilityId{name};
    entry.supported = dcf::VersionRange{dcf::Version{1, 0}, dcf::Version{2, 0}};
    entry.requirement = dcf::CapabilityRequirement::Required;
    declaration.capabilities.push_back(std::move(entry));
  }
  return declaration;
}

void emit(const std::string& line) {
  std::cout << line << "\n";
  std::cout.flush();
}

enum class Pending { None, Register, Activate, Rejoin, Contact };

class SiteRuntime {
 public:
  SiteRuntime(std::shared_ptr<Site> site) : site_(std::move(site)) {}

  [[nodiscard]] bool run_once() {
    auto connection = dcf::wire::connect(site_->options.endpoint);
    if (!connection) {
      return false;
    }
    dcf::wire::Connection link = std::move(connection).value();
    link.set_receive_deadline_ms(site_->options.deadline_ms);

    if (site_->options.through_relay) {
      const std::string preamble = dcf::wire::relay_preamble(dcf::to_string(site_->options.site));
      // The preamble goes through the same connection but is consumed by the
      // relay, so it is written directly rather than framed as a federation
      // message.
      if (!send_raw(link.handle(), preamble)) {
        return false;
      }
    }

    dcf::wire::Message hello;
    hello.kind = dcf::wire::MessageKind::Hello;
    hello.hello.peer = dcf::wire::PeerKind::Site;
    hello.hello.protocol_major = dcf::kProtocolVersionMajor;
    hello.hello.protocol_minor = dcf::kProtocolVersionMinor;
    hello.hello.site = site_->options.site;
    hello.hello.implementation = site_->options.implementation + "/" + dcf::version_string();
    if (!link.send(hello, dcf::Limits{})) {
      return false;
    }
    const auto ack = link.receive(dcf::Limits{});
    if (!ack || ack.value().kind != dcf::wire::MessageKind::HelloAck) {
      return false;
    }
    if (!ack.value().hello_ack.accepted) {
      emit("EVENT hello-refused " + ack.value().hello_ack.detail);
      return false;
    }
    emit("EVENT connected generation " + std::to_string(ack.value().hello_ack.generation.value()));

    const LocalState start = site_->snapshot();
    if (start.registered) {
      site_->adopt(start.accepted, start.accepted_history);
      send_rejoin(link);
    } else {
      send_register(link);
    }

    for (;;) {
      const auto incoming = link.receive(dcf::Limits{});
      if (!incoming) {
        if (incoming.error().code == dcf::ErrorCode::Busy) {
          emit("EVENT idle-timeout");
        }
        return false;
      }
      const dcf::wire::Message& message = incoming.value();
      if (message.kind == dcf::wire::MessageKind::Bye) {
        emit("EVENT closed " + message.bye_reason);
        return false;
      }
      if (message.kind == dcf::wire::MessageKind::Outcome) {
        handle_outcome(link, message.outcome);
        continue;
      }
      if (message.kind == dcf::wire::MessageKind::Publish) {
        handle_publish(link, message.publish);
        continue;
      }
    }
  }

  // The local control port. It is how a site demonstrates that it keeps working
  // while the federation cannot reach it: a local operation succeeds and the
  // site's own epoch advances regardless of the link.
  void serve_control(dcf::wire::Connection connection, std::atomic<bool>& quitting) {
    const auto handle = connection.handle();
    std::string pending;
    char buffer[512];
    for (;;) {
      const int received = recv_raw(handle, buffer, static_cast<int>(sizeof(buffer)));
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
        std::string reply;
        if (command == "STATUS") {
          const LocalState state = site_->snapshot();
          dcf::Json json;
          json.begin_object();
          json.key("site").value(dcf::to_string(site_->options.site));
          json.key("epoch").value(state.epoch.value());
          json.key("accepted_generation").value(state.accepted.value());
          json.key("accepted_history").value(state.accepted_history.to_hex());
          json.key("membership_generation").value(state.membership.value());
          json.key("registered").value(state.registered);
          json.end_object();
          reply = json.str();
        } else if (command == "LOCAL-OP") {
          const std::uint64_t epoch = site_->advance_epoch();
          reply = "OK local-op epoch " + std::to_string(epoch);
          emit("EVENT local-op epoch " + std::to_string(epoch));
        } else if (command == "QUIT") {
          quitting.store(true);
          reply = "BYE";
        } else {
          reply = "ERR unknown site command";
        }
        reply.push_back('\n');
        if (!send_raw(handle, reply)) {
          return;
        }
        newline = pending.find('\n');
      }
    }
  }

 private:
  void send_register(dcf::wire::Connection& link) {
    dcf::Command command;
    command.payload = dcf::RegisterSiteCommand{site_->options.site, site_->options.display_name,
                                               declaration_for(site_->options)};
    command.origin = site_->options.site;
    pending_ = Pending::Register;
    send(link, command);
  }

  void send_rejoin(dcf::wire::Connection& link) {
    const LocalState state = site_->snapshot();
    dcf::SiteReport report;
    report.site = site_->options.site;
    report.accepted = state.accepted;
    report.accepted_history = state.accepted_history;
    report.local_epoch = state.epoch;
    report.membership_generation = state.membership;
    dcf::Command command;
    command.payload = dcf::RejoinCommand{report, false};
    command.origin = site_->options.site;
    pending_ = Pending::Rejoin;
    send(link, command);
  }

  void send_contact(dcf::wire::Connection& link) {
    const LocalState state = site_->snapshot();
    dcf::RecordContactCommand contact;
    contact.site = site_->options.site;
    contact.accepted = state.accepted;
    contact.accepted_history = state.accepted_history;
    contact.local_epoch = state.epoch;
    contact.at = dcf::UnixMillis{static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count())};
    dcf::Command command;
    command.payload = contact;
    command.origin = site_->options.site;
    pending_ = Pending::Contact;
    send(link, command);
  }

  void send_activate(dcf::wire::Connection& link) {
    const LocalState state = site_->snapshot();
    dcf::ActivateSiteCommand activate;
    activate.site = site_->options.site;
    activate.accepted = state.accepted;
    activate.accepted_history = state.accepted_history;
    activate.local_epoch = state.epoch;
    dcf::Command command;
    command.payload = activate;
    command.origin = site_->options.site;
    pending_ = Pending::Activate;
    send(link, command);
  }

  void send(dcf::wire::Connection& link, const dcf::Command& command) {
    dcf::wire::Message message;
    message.kind = dcf::wire::MessageKind::CommandMessage;
    message.command = command;
    if (!link.send(message, dcf::Limits{})) {
      emit("EVENT send-failed");
    }
  }

  void handle_outcome(dcf::wire::Connection& link, const dcf::wire::OutcomeMessage& outcome) {
    const Pending pending = pending_;
    pending_ = Pending::None;
    emit("EVENT outcome " + std::string(dcf::to_string(outcome.code)) + " " +
         (outcome.detail.empty() ? std::string("ok") : outcome.detail));

    if (outcome.code == dcf::ErrorCode::DuplicateIdentity && pending == Pending::Register) {
      // The federation already knows this site, so it is registered after all.
      std::lock_guard<std::mutex> guard(site_->mutex);
      site_->local.registered = true;
      site_->persist();
      return;
    }
    if (outcome.code == dcf::ErrorCode::None && pending == Pending::Register) {
      std::lock_guard<std::mutex> guard(site_->mutex);
      site_->local.registered = true;
      site_->persist();
      return;
    }
    if (pending == Pending::Rejoin || pending == Pending::Activate || pending == Pending::Contact) {
      if (outcome.fenced) {
        emit("EVENT fenced by-federation");
      }
      if (outcome.adopt_generation != 0) {
        site_->adopt(dcf::AcceptedGeneration{outcome.adopt_generation}, outcome.adopt_history);
        emit("EVENT adopted generation " + std::to_string(outcome.adopt_generation));
        if (!outcome.fenced) {
          send_contact(link);
        }
      }
      if (!outcome.reconciliation_outcome.empty()) {
        emit("EVENT reconciliation " + outcome.reconciliation_outcome);
      }
    }
  }

  void handle_publish(dcf::wire::Connection& link, const dcf::wire::Publish& publish) {
    {
      std::lock_guard<std::mutex> guard(site_->mutex);
      site_->local.membership = publish.membership_generation;
    }
    const auto state = static_cast<dcf::MembershipState>(publish.membership_state);
    emit("EVENT publish generation " + std::to_string(publish.generation.value()) + " membership " +
         std::to_string(publish.membership_generation.value()) + " state " +
         std::string(dcf::to_string(state)));

    if (state == dcf::MembershipState::Removed || is_terminal(state)) {
      emit("EVENT terminal " + std::string(dcf::to_string(state)));
      return;
    }
    if (state == dcf::MembershipState::Admitted) {
      // A repeated publish for the same membership generation is the same fact
      // arriving twice, not a request to act twice.
      if (activation_requested_.value() == publish.membership_generation.value() &&
          publish.membership_generation.value() != 0) {
        return;
      }
      activation_requested_ = publish.membership_generation;
      send_activate(link);
      return;
    }
    if (state == dcf::MembershipState::Active || state == dcf::MembershipState::Constrained ||
        state == dcf::MembershipState::Partitioned || state == dcf::MembershipState::Draining) {
      // The site is current with the generation being published, so it accepts
      // it. It reports back only when that is news: reporting an acceptance the
      // federation already holds would produce a publish in reply and the two
      // processes would talk to each other forever.
      const LocalState previous = site_->snapshot();
      const bool news = previous.accepted.value() != publish.generation.value() ||
                        previous.accepted_history != publish.site_digest;
      site_->adopt(dcf::AcceptedGeneration{publish.generation.value()}, publish.site_digest);
      if (news) {
        send_contact(link);
      }
    }
  }

  std::shared_ptr<Site> site_{};
  Pending pending_{Pending::None};
  dcf::MembershipGeneration activation_requested_{};
};

}  // namespace

int main(int argc, char** argv) {
  const dcf::cli::OptionSet parsed = dcf::cli::parse_options(argc, argv, 1);
  if (parsed.has_flag("help")) {
    std::cout << "dcf-sited --id HEX --name NAME (--relay HOST:PORT | --direct HOST:PORT)\n"
                 "          [--capabilities A,B] [--protocol RANGE] [--implementation NAME]\n"
                 "          [--local-state FILE] [--control HOST:PORT] [--deadline-ms N]\n";
    return 0;
  }
  auto site = std::make_shared<Site>();
  Options& options = site->options;

  const auto id_text = parsed.require("id");
  const auto name_text = parsed.require("name");
  if (!id_text || !name_text) {
    std::cerr << "error: --id and --name are required\n";
    return 2;
  }
  const auto id = dcf::parse_identifier<dcf::SiteTag>(id_text.value());
  if (!id.has_value()) {
    std::cerr << "error: '" << id_text.value() << "' is not a site identifier\n";
    return 2;
  }
  options.site = id.value();
  options.display_name = name_text.value();

  if (const auto relay = parsed.get("relay"); relay.has_value()) {
    const auto address = dcf::wire::parse_address(relay.value());
    if (!address) {
      std::cerr << "error: " << address.error().detail << "\n";
      return 2;
    }
    options.endpoint = address.value();
    options.through_relay = true;
  } else if (const auto direct = parsed.get("direct"); direct.has_value()) {
    const auto address = dcf::wire::parse_address(direct.value());
    if (!address) {
      std::cerr << "error: " << address.error().detail << "\n";
      return 2;
    }
    options.endpoint = address.value();
    options.through_relay = false;
  } else {
    std::cerr << "error: either --relay or --direct is required\n";
    return 2;
  }

  if (const auto capabilities = parsed.get("capabilities"); capabilities.has_value()) {
    const auto items = dcf::cli::split_list(capabilities.value(), ',');
    if (!items) {
      std::cerr << "error: " << items.error().detail << "\n";
      return 2;
    }
    options.capabilities = items.value();
  }
  if (const auto protocol = parsed.get("protocol"); protocol.has_value()) {
    const auto range = dcf::cli::parse_range(protocol.value());
    if (!range) {
      std::cerr << "error: " << range.error().detail << "\n";
      return 2;
    }
    options.protocol = range.value();
  }
  options.implementation = parsed.get_or("implementation", options.implementation);
  options.local_state = parsed.get_or("local-state", "");
  if (const auto control = parsed.get("control"); control.has_value()) {
    const auto address = dcf::wire::parse_address(control.value());
    if (!address) {
      std::cerr << "error: " << address.error().detail << "\n";
      return 2;
    }
    options.control = address.value();
    options.has_control = true;
  }
  if (parsed.has("deadline-ms")) {
    const auto value = dcf::cli::parse_u64(parsed.get_or("deadline-ms", "30000"), "peer deadline");
    if (!value) {
      std::cerr << "error: " << value.error().detail << "\n";
      return 2;
    }
    options.deadline_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(value.value(), 3600000));
  }

  site->load();
  std::cout << "site " << dcf::to_string(options.site) << " name " << options.display_name << "\n";
  std::cout << "endpoint " << options.endpoint.to_string()
            << (options.through_relay ? " (through relay)" : " (direct)") << "\n";
  std::cout << "local-epoch " << site->snapshot().epoch.value() << "\n";
  std::cout << "READY" << std::endl;

  std::atomic<bool> quitting{false};
  std::thread control_thread;
  std::unique_ptr<dcf::wire::Listener> control_listener;
  if (options.has_control) {
    auto listener = dcf::wire::Listener::bind(options.control);
    if (!listener) {
      std::cerr << "error: " << listener.error().detail << "\n";
      return 3;
    }
    control_listener = std::make_unique<dcf::wire::Listener>(std::move(listener).value());
    const auto port = control_listener->port();
    std::cout << "control " << options.control.host << ":"
              << (port.has_value() ? std::to_string(port.value()) : std::string("?")) << "\n";
    std::cout.flush();
    control_thread = std::thread([&] {
      for (;;) {
        auto accepted = control_listener->accept();
        if (!accepted) {
          return;
        }
        std::thread([&site, &quitting, connection = std::move(accepted).value()]() mutable {
          SiteRuntime runtime(site);
          runtime.serve_control(std::move(connection), quitting);
        }).detach();
      }
    });
  }

  SiteRuntime runtime(site);
  std::uint32_t backoff = 25;
  while (!quitting.load()) {
    const bool connected = runtime.run_once();
    if (quitting.load()) {
      break;
    }
    if (connected) {
      backoff = 25;
    } else {
      backoff = std::min<std::uint32_t>(backoff * 2U, options.reconnect_ms * 8U);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
  }

  if (control_listener) {
    control_listener->close();
  }
  if (control_thread.joinable()) {
    control_thread.join();
  }
  return 0;
}
