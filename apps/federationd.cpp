// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// dcf-federationd hosts one federation: it owns the durable store, the runtime,
// and the wire protocol that sites and operators speak to it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dcf/cli.hpp"
#include "dcf/hash.hpp"
#include "dcf/report.hpp"
#include "dcf/runtime.hpp"
#include "dcf/store.hpp"
#include "dcf/version.hpp"
#include "dcf/wire.hpp"

namespace {

class SystemClock final : public dcf::Clock {
 public:
  [[nodiscard]] dcf::UnixMillis wall_clock() const noexcept override {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return dcf::UnixMillis{
        static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count())};
  }
};

struct Options {
  std::string store{};
  dcf::wire::Address listen{"127.0.0.1", 0};
  std::string federation{};
  std::uint64_t compact_every{0};
  std::size_t max_sessions{64};
  std::uint32_t deadline_ms{30000};
  bool quiet{false};
};

[[nodiscard]] dcf::FederationId random_federation() {
  std::random_device device;
  std::seed_seq seed{device(), device(), device(), device()};
  std::mt19937_64 engine(seed);
  dcf::FederationId id;
  id.high = engine();
  id.low = engine();
  if (id.is_zero()) {
    id.low = 1;
  }
  return id;
}

// A federation must exist before it can be joined, and the record of its
// existence is a journal entry like every other. It is written here, before the
// runtime is open, so that opening the runtime is always opening a federation
// that already exists.
[[nodiscard]] dcf::Result<dcf::FederationId> ensure_federation(const Options& options,
                                                               const dcf::Clock& clock) {
  auto opened = dcf::JournalStore::open(options.store, dcf::Limits{});
  if (!opened) {
    return opened.error();
  }
  dcf::JournalStore store = std::move(opened).value();
  if (store.recovered_state().initialized()) {
    const dcf::FederationId existing = store.recovered_state().federation();
    const auto closed = store.close();
    if (!closed) {
      return closed.error();
    }
    return existing;
  }
  dcf::FederationId federation = random_federation();
  if (!options.federation.empty()) {
    const auto parsed = dcf::parse_identifier<dcf::FederationTag>(options.federation);
    if (!parsed.has_value()) {
      return dcf::make_error(dcf::ErrorCode::InvalidArgument,
                             "'" + dcf::sanitize_for_terminal(options.federation) +
                                 "' is not a federation identifier");
    }
    federation = parsed.value();
  }
  const auto committed = store.commit(dcf::genesis_entry(federation, clock.wall_clock()));
  if (!committed) {
    return committed.error();
  }
  const auto closed = store.close();
  if (!closed) {
    return closed.error();
  }
  return federation;
}

[[nodiscard]] dcf::wire::Publish publish_for(const dcf::FederationState& state,
                                             const dcf::SiteId& site) {
  dcf::wire::Publish publish;
  publish.generation = state.generation();
  publish.authority_digest = state.authority_digest();
  publish.site_digest = state.site_authority_digest(site);
  publish.sequence = state.sequence();
  if (const dcf::MembershipRecord* record = state.find_site(site)) {
    publish.membership_generation = record->generation;
    publish.membership_state = static_cast<std::uint8_t>(record->state);
  }
  return publish;
}

// A connected peer. Several threads may want to send to the same peer - the
// session thread answering a command and the broadcaster pushing a state change
// - so sends are serialised. Sends and receives have separate mutexes, because a
// TCP connection is full duplex: holding one lock across a blocking receive
// would stall every other thread that wanted to write to that peer, and the
// broadcaster would end up waiting for the peer to speak first.
class Session {
 public:
  explicit Session(dcf::wire::Connection connection) : connection_(std::move(connection)) {}

  [[nodiscard]] dcf::Result<dcf::Ack> send(const dcf::wire::Message& message) {
    std::lock_guard<std::mutex> guard(send_mutex_);
    return connection_.send(message, dcf::Limits{});
  }

  [[nodiscard]] dcf::Result<dcf::wire::Message> receive() {
    std::lock_guard<std::mutex> guard(receive_mutex_);
    return connection_.receive(dcf::Limits{});
  }

  void set_deadline(std::uint32_t millis) {
    std::lock_guard<std::mutex> guard(send_mutex_);
    connection_.set_receive_deadline_ms(millis);
  }

 private:
  std::mutex send_mutex_{};
  std::mutex receive_mutex_{};
  dcf::wire::Connection connection_{};
};

// The sessions that should hear about a change. A publish is built per site
// because it carries that site's own membership generation and digest.
class SessionRegistry {
 public:
  void add(const dcf::SiteId& site, std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> guard(mutex_);
    sessions_[site] = std::move(session);
  }

  void remove(const dcf::SiteId& site) {
    std::lock_guard<std::mutex> guard(mutex_);
    sessions_.erase(site);
  }

  // The registry lock is released before anything is sent, so a slow peer
  // cannot hold up the registry itself.
  void broadcast(const dcf::FederationRuntime& runtime) {
    std::vector<std::pair<dcf::SiteId, std::shared_ptr<Session>>> targets;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      targets.reserve(sessions_.size());
      for (const auto& entry : sessions_) {
        targets.emplace_back(entry.first, entry.second);
      }
    }
    if (targets.empty()) {
      return;
    }
    const dcf::FederationState state = runtime.snapshot();
    for (const auto& entry : targets) {
      dcf::wire::Message message;
      message.kind = dcf::wire::MessageKind::Publish;
      message.publish = publish_for(state, entry.first);
      if (!entry.second->send(message)) {
        remove(entry.first);
      }
    }
  }

 private:
  std::mutex mutex_{};
  std::map<dcf::SiteId, std::shared_ptr<Session>> sessions_{};
};

// Connectivity is reported to the runtime as a command, so it is durable and
// ordered with every other change rather than kept in a side channel.
void report_connectivity(dcf::FederationRuntime& runtime, const dcf::SiteId& site,
                         dcf::LinkState link) {
  const dcf::MembershipRecord* record = runtime.snapshot().find_site(site);
  if (record == nullptr || record->link == link) {
    return;
  }
  dcf::Command command;
  command.payload = dcf::SetConnectivityCommand{site, link};
  static_cast<void>(runtime.submit_sync(command));
}

void log(const Options& options, const std::string& text) {
  if (options.quiet) {
    return;
  }
  std::cout << text << "\n";
  std::cout.flush();
}

void serve(std::shared_ptr<Session> session, dcf::FederationRuntime& runtime,
           const Options& options, SessionRegistry& registry, std::atomic<std::size_t>& active) {
  // The session counter and the registry entry are released however this
  // function ends, including on an early refusal.
  dcf::SiteId registered_site{};
  bool site_registered = false;
  dcf::FederationRuntime* runtime_pointer = &runtime;
  struct Guard {
    std::atomic<std::size_t>& counter;
    SessionRegistry& registry;
    const dcf::SiteId& site;
    const bool& registered;
    dcf::FederationRuntime* runtime;
    ~Guard() {
      if (registered) {
        registry.remove(site);
        report_connectivity(*runtime, site, dcf::LinkState::Partitioned);
      }
      counter.fetch_sub(1);
    }
  } guard{active, registry, registered_site, site_registered, runtime_pointer};

  session->set_deadline(options.deadline_ms);
  const auto hello = session->receive();
  if (!hello || hello.value().kind != dcf::wire::MessageKind::Hello) {
    dcf::wire::Message bye;
    bye.kind = dcf::wire::MessageKind::Bye;
    bye.bye_reason = "the first message on a connection must be a hello";
    static_cast<void>(session->send(bye));
    return;
  }
  const dcf::wire::Hello& greeting = hello.value().hello;

  dcf::wire::Message ack;
  ack.kind = dcf::wire::MessageKind::HelloAck;
  ack.hello_ack.federation = runtime.snapshot().federation();
  ack.hello_ack.generation = runtime.generation();
  ack.hello_ack.authority_digest = runtime.authority_digest();

  if (greeting.protocol_major != dcf::kProtocolVersionMajor) {
    ack.hello_ack.accepted = false;
    ack.hello_ack.detail = "this federation speaks protocol major version " +
                           std::to_string(dcf::kProtocolVersionMajor) + " and the peer offered " +
                           std::to_string(greeting.protocol_major);
    static_cast<void>(session->send(ack));
    return;
  }
  if (greeting.peer == dcf::wire::PeerKind::Site && greeting.site.is_zero()) {
    ack.hello_ack.accepted = false;
    ack.hello_ack.detail = "a site must present a non-zero site identifier";
    static_cast<void>(session->send(ack));
    return;
  }
  if (greeting.peer == dcf::wire::PeerKind::Site) {
    ack.hello_ack.relay_token = dcf::to_string(greeting.site);
  } else {
    ack.hello_ack.relay_token = "operator";
  }
  ack.hello_ack.accepted = true;
  ack.hello_ack.detail = "accepted";
  if (!session->send(ack)) {
    return;
  }

  const bool is_site = greeting.peer == dcf::wire::PeerKind::Site;
  const dcf::SiteId site = greeting.site;

  if (is_site) {
    registered_site = site;
    site_registered = true;
    registry.add(site, session);
    // A live session is evidence of a live link, and the end of a session is
    // evidence that the link is gone. The federation records what it observed
    // rather than inferring connectivity from silence.
    report_connectivity(runtime, site, dcf::LinkState::Connected);
  }

  // The first publish tells a site where it stands before it asks for anything.
  if (is_site) {
    dcf::wire::Message publish;
    publish.kind = dcf::wire::MessageKind::Publish;
    publish.publish = publish_for(runtime.snapshot(), site);
    if (!session->send(publish)) {
      return;
    }
  }

  for (;;) {
    const auto incoming = session->receive();
    if (!incoming) {
      if (incoming.error().code == dcf::ErrorCode::Busy) {
        // A silent peer is a fault. It is reported and the connection is closed
        // rather than held open indefinitely.
        log(options, "closing an idle connection after " + std::to_string(options.deadline_ms) +
                         " ms");
        return;
      }
      return;
    }
    const dcf::wire::Message& message = incoming.value();

    if (message.kind == dcf::wire::MessageKind::Bye) {
      return;
    }

    if (message.kind == dcf::wire::MessageKind::Query) {
      const dcf::FederationState state = runtime.snapshot();
      const dcf::RuntimeStats stats = runtime.stats();
      const dcf::RecoveryReport recovery = runtime.recovery();
      dcf::QueryContext context;
      context.state = &state;
      context.stats = &stats;
      context.recovery = &recovery;
      dcf::wire::Message result;
      result.kind = dcf::wire::MessageKind::QueryResult;
      const auto rendered = dcf::render_query(message.query.kind, message.query.argument, context);
      result.query_result.ok = rendered.has_value();
      result.query_result.body =
          rendered.has_value() ? rendered.value() : rendered.error().detail;
      if (!session->send(result)) {
        return;
      }
      continue;
    }

    if (message.kind != dcf::wire::MessageKind::CommandMessage) {
      dcf::wire::Message bye;
      bye.kind = dcf::wire::MessageKind::Bye;
      bye.bye_reason = "that message is not something a peer may send to a federation";
      static_cast<void>(session->send(bye));
      return;
    }

    const dcf::FederationGeneration generation_before = runtime.generation();
    const auto outcome = runtime.submit_sync(message.command);
    dcf::wire::Message reply;
    reply.kind = dcf::wire::MessageKind::Outcome;
    if (!outcome) {
      reply.outcome.code = outcome.error().code;
      reply.outcome.detail = outcome.error().detail;
      reply.outcome.generation = runtime.generation();
    } else {
      reply.outcome.code = outcome.value().code;
      reply.outcome.detail = outcome.value().detail;
      reply.outcome.generation = outcome.value().generation;
      reply.outcome.membership_generation = outcome.value().membership_generation;
      if (outcome.value().reconciliation.has_value()) {
        const dcf::ReconciliationEvaluation& evaluation = *outcome.value().reconciliation;
        reply.outcome.reconciliation_outcome = std::string(dcf::to_string(evaluation.outcome));
        reply.outcome.adopt_generation = evaluation.adopt.value();
        reply.outcome.fenced = evaluation.fenced;
        reply.outcome.adopt_history = runtime.snapshot().site_authority_digest(site);
      }
    }
    if (!session->send(reply)) {
      return;
    }

    // A change is pushed to every connected site, not only to the one that
    // asked, because a membership change concerns the whole federation. A
    // command that left the generation alone is not pushed: a site that answers
    // a publish would otherwise answer forever.
    if (outcome.has_value() && outcome.value().code == dcf::ErrorCode::None &&
        outcome.value().generation.value() != generation_before.value()) {
      registry.broadcast(runtime);
    } else if (is_site) {
      dcf::wire::Message publish;
      publish.kind = dcf::wire::MessageKind::Publish;
      publish.publish = publish_for(runtime.snapshot(), site);
      if (!session->send(publish)) {
        return;
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  const dcf::cli::OptionSet parsed = dcf::cli::parse_options(argc, argv, 1);
  if (parsed.has_flag("help")) {
    std::cout << "dcf-federationd --store DIR --listen HOST:PORT [--federation HEX]\n"
                 "                [--compact-every N] [--max-sessions N] [--deadline-ms N] "
                 "[--quiet]\n";
    return 0;
  }
  options.quiet = parsed.has_flag("quiet");
  const auto store = parsed.require("store");
  if (!store) {
    std::cerr << "error: " << store.error().detail << "\n";
    return 2;
  }
  options.store = store.value();
  const auto listen = parsed.require("listen");
  if (!listen) {
    std::cerr << "error: " << listen.error().detail << "\n";
    return 2;
  }
  const auto address = dcf::wire::parse_address(listen.value());
  if (!address) {
    std::cerr << "error: " << address.error().detail << "\n";
    return 2;
  }
  options.listen = address.value();
  options.federation = parsed.get_or("federation", "");
  if (parsed.has("compact-every")) {
    const auto value = dcf::cli::parse_u64(parsed.get_or("compact-every", "0"), "compaction cadence");
    if (!value) {
      std::cerr << "error: " << value.error().detail << "\n";
      return 2;
    }
    options.compact_every = value.value();
  }
  if (parsed.has("max-sessions")) {
    const auto value = dcf::cli::parse_u64(parsed.get_or("max-sessions", "64"), "session limit");
    if (!value) {
      std::cerr << "error: " << value.error().detail << "\n";
      return 2;
    }
    options.max_sessions = static_cast<std::size_t>(value.value());
  }
  if (parsed.has("deadline-ms")) {
    const auto value = dcf::cli::parse_u64(parsed.get_or("deadline-ms", "30000"), "peer deadline");
    if (!value) {
      std::cerr << "error: " << value.error().detail << "\n";
      return 2;
    }
    options.deadline_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(value.value(), 3600000));
  }

  const SystemClock clock;
  const auto federation = ensure_federation(options, clock);
  if (!federation) {
    std::cerr << "error: " << federation.error().detail << "\n";
    return 3;
  }

  dcf::RuntimeOptions runtime_options;
  runtime_options.store_root = options.store;
  runtime_options.limits = dcf::Limits{};
  runtime_options.command_queue_capacity = 4096;
  runtime_options.compact_every_entries = options.compact_every;

  auto runtime = dcf::FederationRuntime::open(
      runtime_options, std::make_shared<SystemClock>());
  if (!runtime) {
    std::cerr << "error: " << runtime.error().detail << "\n";
    return 3;
  }
  std::unique_ptr<dcf::FederationRuntime> service = std::move(runtime).value();

  auto listener = dcf::wire::Listener::bind(options.listen);
  if (!listener) {
    std::cerr << "error: " << listener.error().detail << "\n";
    return 3;
  }
  dcf::wire::Listener bound = std::move(listener).value();
  const auto port = bound.port();
  if (!port) {
    std::cerr << "error: " << port.error().detail << "\n";
    return 3;
  }

  const dcf::RecoveryReport recovery = service->recovery();
  std::cout << "federation " << dcf::to_string(federation.value()) << "\n";
  std::cout << "listening " << options.listen.host << ":" << port.value() << "\n";
  std::cout << "generation " << service->generation().value() << " sequence "
            << service->last_sequence().value() << "\n";
  std::cout << "recovery reopened=" << (recovery.reopened ? 1 : 0)
            << " replayed=" << recovery.entries_replayed
            << " torn_tail_bytes=" << recovery.torn_tail_bytes_removed << "\n";
  std::cout << "READY" << std::endl;

  std::atomic<std::size_t> active{0};
  SessionRegistry registry;
  std::vector<std::thread> sessions;
  for (;;) {
    auto connection = bound.accept();
    if (!connection) {
      if (connection.error().code == dcf::ErrorCode::Busy) {
        continue;
      }
      std::cerr << "error: " << connection.error().detail << "\n";
      break;
    }
    if (active.load() >= options.max_sessions) {
      dcf::wire::Message bye;
      bye.kind = dcf::wire::MessageKind::Bye;
      bye.bye_reason = "this federation is at its session limit";
      static_cast<void>(connection.value().send(bye, dcf::Limits{}));
      continue;
    }
    active.fetch_add(1);
    auto session = std::make_shared<Session>(std::move(connection).value());
    sessions.emplace_back([session = std::move(session), &service, &options, &registry,
                           &active]() mutable {
      serve(std::move(session), *service, options, registry, active);
    });
  }

  for (std::thread& session : sessions) {
    if (session.joinable()) {
      session.join();
    }
  }
  static_cast<void>(service->shutdown());
  return 0;
}
