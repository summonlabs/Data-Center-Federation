// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The multiprocess suite. Every process here is a real operating-system process
// with its own address space, and the link between a site and the federation is
// a real TCP path through the relay. A partition is produced by the relay
// closing that path, which is what a partition is: the bytes stop, and then the
// connection is gone.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "dcf/cli.hpp"
#include "dcf/json.hpp"
#include "dcf/version.hpp"
#include "dcf/wire.hpp"
#include "process.hpp"
#include "test_harness.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#endif

namespace {

namespace fs = std::filesystem;
using dcf::wire::Address;

constexpr const char* kSiteId = "00000000000000010000000000000001";
constexpr const char* kSecondSiteId = "00000000000000020000000000000002";

void settle(int millis) { std::this_thread::sleep_for(std::chrono::milliseconds(millis)); }

// Finds the port a daemon reported on a line containing the marker.
[[nodiscard]] std::uint16_t port_after(const std::string& path, const std::string& marker) {
  const std::string text = dcf::test::read_text_file(path);
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t end = text.find('\n', offset);
    const std::string line = text.substr(offset, end == std::string::npos ? std::string::npos
                                                                          : end - offset);
    if (line.find(marker) != std::string::npos) {
      const std::size_t colon = line.rfind(':');
      if (colon != std::string::npos) {
        std::string digits = line.substr(colon + 1);
        // A text-mode stdout on Windows turns every newline into a carriage
        // return and a newline, so the field is trimmed before it is parsed.
        while (!digits.empty() && (digits.back() < '0' || digits.back() > '9')) {
          digits.pop_back();
        }
        const auto value = dcf::cli::parse_u64(digits, "port");
        if (value.has_value() && value.value() > 0 && value.value() <= 65535) {
          return static_cast<std::uint16_t>(value.value());
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return 0;
}

// A line-oriented client for the relay control port and the site control port.
class LineClient {
 public:
  LineClient() = default;
  LineClient(const LineClient&) = delete;
  LineClient& operator=(const LineClient&) = delete;
  LineClient(LineClient&&) = default;
  LineClient& operator=(LineClient&&) = default;

  [[nodiscard]] static LineClient open(const Address& address) {
    LineClient client;
    auto connection = dcf::wire::connect(address);
    if (connection.has_value()) {
      client.connection_ = std::move(connection).value();
      client.connected_ = true;
    }
    return client;
  }

  [[nodiscard]] bool connected() const noexcept { return connected_; }

  [[nodiscard]] bool send_line(const std::string& text) {
    if (!connected_) {
      return false;
    }
    const std::string line = text + "\n";
    std::size_t sent = 0;
    while (sent < line.size()) {
      const int chunk = send_bytes(line.data() + sent, line.size() - sent);
      if (chunk <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(chunk);
    }
    return true;
  }

  [[nodiscard]] std::string read_line(int attempts = 200) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
      while (true) {
        const std::size_t newline = buffer_.find('\n');
        if (newline == std::string::npos) {
          break;
        }
        std::string line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        while (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      char chunk[512];
      const int received = receive_bytes(chunk, static_cast<int>(sizeof(chunk)));
      if (received <= 0) {
        settle(10);
        continue;
      }
      buffer_.append(chunk, static_cast<std::size_t>(received));
    }
    return {};
  }

 private:
  [[nodiscard]] int send_bytes(const char* data, std::size_t size) {
#ifdef _WIN32
    return ::send(static_cast<SOCKET>(connection_.handle()), data, static_cast<int>(size), 0);
#else
    return static_cast<int>(::send(static_cast<int>(connection_.handle()), data, size,
                                   MSG_NOSIGNAL));
#endif
  }

  [[nodiscard]] int receive_bytes(char* data, int size) {
#ifdef _WIN32
    return ::recv(static_cast<SOCKET>(connection_.handle()), data, size, 0);
#else
    return static_cast<int>(::recv(static_cast<int>(connection_.handle()), data,
                                   static_cast<std::size_t>(size), 0));
#endif
  }

  dcf::wire::Connection connection_{};
  std::string buffer_{};
  bool connected_{false};
};

// One operator exchange over the federation protocol.
[[nodiscard]] dcf::Result<dcf::wire::Message> operator_exchange(const Address& endpoint,
                                                           const dcf::wire::Message& request) {
  auto connection = dcf::wire::connect(endpoint);
  if (!connection) {
    return connection.error();
  }
  dcf::wire::Connection link = std::move(connection).value();
  link.set_receive_deadline_ms(30000);
  dcf::wire::Message hello;
  hello.kind = dcf::wire::MessageKind::Hello;
  hello.hello.peer = dcf::wire::PeerKind::Operator;
  hello.hello.protocol_major = dcf::kProtocolVersionMajor;
  hello.hello.protocol_minor = dcf::kProtocolVersionMinor;
  hello.hello.implementation = "cluster-test";
  const auto sent = link.send(hello, dcf::Limits{});
  if (!sent) {
    return sent.error();
  }
  const auto ack = link.receive(dcf::Limits{});
  if (!ack || !ack.value().hello_ack.accepted) {
    return dcf::make_error(dcf::ErrorCode::Closed, "the operator greeting was refused");
  }
  const auto request_sent = link.send(request, dcf::Limits{});
  if (!request_sent) {
    return request_sent.error();
  }
  return link.receive(dcf::Limits{});
}

// A poll that opens a new connection every time turns a slow machine into a
// connection storm, and the storm then becomes the thing under test. This keeps
// one session open and reconnects only when the connection is actually gone.
class Poller {
 public:
  explicit Poller(Address endpoint) : endpoint_(std::move(endpoint)) {}
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;
  ~Poller() = default;

  [[nodiscard]] std::string sites() {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!connected_ && !connect()) {
        settle(50);
        continue;
      }
      dcf::wire::Message request;
      request.kind = dcf::wire::MessageKind::Query;
      request.query.kind = dcf::wire::QueryKind::Sites;
      const auto sent = connection_.send(request, dcf::Limits{});
      if (!sent) {
        connected_ = false;
        continue;
      }
      const auto reply = connection_.receive(dcf::Limits{});
      if (!reply || reply.value().kind != dcf::wire::MessageKind::QueryResult) {
        connected_ = false;
        continue;
      }
      return reply.value().query_result.body;
    }
    return {};
  }

 private:
  [[nodiscard]] bool connect() {
    auto connection = dcf::wire::connect(endpoint_);
    if (!connection) {
      return false;
    }
    connection_ = std::move(connection).value();
    connection_.set_receive_deadline_ms(30000);
    dcf::wire::Message hello;
    hello.kind = dcf::wire::MessageKind::Hello;
    hello.hello.peer = dcf::wire::PeerKind::Operator;
    hello.hello.protocol_major = dcf::kProtocolVersionMajor;
    hello.hello.protocol_minor = dcf::kProtocolVersionMinor;
    hello.hello.implementation = "cluster-test-poller";
    if (!connection_.send(hello, dcf::Limits{})) {
      return false;
    }
    const auto ack = connection_.receive(dcf::Limits{});
    if (!ack || !ack.value().hello_ack.accepted) {
      return false;
    }
    connected_ = true;
    return true;
  }

  Address endpoint_{};
  dcf::wire::Connection connection_{};
  bool connected_{false};
};

[[nodiscard]] dcf::CommandOutcome submit_operator(const Address& endpoint, dcf::CommandPayload payload) {
  dcf::wire::Message message;
  message.kind = dcf::wire::MessageKind::CommandMessage;
  message.command.payload = std::move(payload);
  const auto reply = operator_exchange(endpoint, message);
  dcf::CommandOutcome outcome;
  if (!reply || reply.value().kind != dcf::wire::MessageKind::Outcome) {
    outcome.code = dcf::ErrorCode::Io;
    outcome.detail = "no outcome was received";
    return outcome;
  }
  outcome.code = reply.value().outcome.code;
  outcome.detail = reply.value().outcome.detail;
  outcome.generation = reply.value().outcome.generation;
  outcome.membership_generation = reply.value().outcome.membership_generation;
  return outcome;
}

[[nodiscard]] std::string query_operator(const Address& endpoint, dcf::wire::QueryKind kind,
                                         const std::string& argument) {
  dcf::wire::Message message;
  message.kind = dcf::wire::MessageKind::Query;
  message.query.kind = kind;
  message.query.argument = argument;
  const auto reply = operator_exchange(endpoint, message);
  if (!reply || reply.value().kind != dcf::wire::MessageKind::QueryResult) {
    return {};
  }
  return reply.value().query_result.body;
}

// Extracts a numeric field from a flat JSON object without a JSON parser.
[[nodiscard]] std::uint64_t field_u64(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t at = json.find(needle);
  if (at == std::string::npos) {
    return 0;
  }
  const std::size_t start = at + needle.size();
  std::size_t end = start;
  while (end < json.size() && (json[end] >= '0' && json[end] <= '9')) {
    ++end;
  }
  if (end == start) {
    return 0;
  }
  const auto value = dcf::cli::parse_u64(json.substr(start, end - start), "field");
  return value.has_value() ? value.value() : 0;
}

[[nodiscard]] std::string field_string(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const std::size_t at = json.find(needle);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = at + needle.size();
  const std::size_t end = json.find('"', start);
  if (end == std::string::npos) {
    return {};
  }
  return json.substr(start, end - start);
}

struct Cluster {
  std::string root{};
  Address federation{"127.0.0.1", 0};
  Address relay_data{"127.0.0.1", 0};
  Address relay_control{"127.0.0.1", 0};
  dcf::test::ChildProcess federation_process{};
  dcf::test::ChildProcess relay_process{};
  dcf::test::ChildProcess site_process{};
  std::string last_error{};
  std::string federation_log{};
  std::string relay_log{};
  std::string site_log{};

  [[nodiscard]] bool start_federation(const std::string& executable,
                                      const std::string& federation_id) {
    std::vector<std::string> arguments{"--store",     root + "/store",
                                       "--listen",    "127.0.0.1:0",
                                       "--compact-every", "0",
                                       "--quiet"};
    if (!federation_id.empty()) {
      arguments.push_back("--federation");
      arguments.push_back(federation_id);
    }
    federation_process = dcf::test::ChildProcess::spawn(executable, arguments, federation_log);
    if (!federation_process.valid()) {
      last_error = "spawn failed: " + dcf::test::last_spawn_error();
      return false;
    }
    if (!dcf::test::wait_for_marker(federation_log, "READY", 1200, 25)) {
      last_error = "no readiness marker; log was: " + dcf::test::read_text_file(federation_log);
      return false;
    }
    federation.port = port_after(federation_log, "listening");
    return federation.port != 0;
  }

  [[nodiscard]] bool start_relay(const std::string& executable) {
    relay_process = dcf::test::ChildProcess::spawn(
        executable,
        {"--listen", "127.0.0.1:0", "--upstream", federation.to_string(), "--control",
         "127.0.0.1:0"},
        relay_log);
    if (!relay_process.valid()) {
      last_error = "relay spawn failed: " + dcf::test::last_spawn_error();
      return false;
    }
    if (!dcf::test::wait_for_marker(relay_log, "READY", 1200, 25)) {
      last_error = "no relay readiness marker; log was: " +
                   dcf::test::read_text_file(relay_log);
      return false;
    }
    relay_data.port = port_after(relay_log, "relay-data");
    relay_control.port = port_after(relay_log, "relay-control");
    return relay_data.port != 0 && relay_control.port != 0;
  }

  [[nodiscard]] bool start_site(const std::string& executable, const std::string& site_id,
                                const std::string& name, const std::string& state_file,
                                const std::string& log) {
    site_log = log;
    site_process = dcf::test::ChildProcess::spawn(
        executable,
        {"--id", site_id, "--name", name, "--relay", relay_data.to_string(), "--capabilities",
         "dcf.membership", "--local-state", state_file, "--control", "127.0.0.1:0"},
        site_log);
    if (!site_process.valid()) {
      last_error = "site spawn failed: " + dcf::test::last_spawn_error();
      return false;
    }
    if (!dcf::test::wait_for_marker(site_log, "READY", 1200, 25)) {
      last_error = "no site readiness marker; log was: " + dcf::test::read_text_file(site_log);
      return false;
    }
    return true;
  }

  [[nodiscard]] std::uint16_t site_control_port() const {
    return port_after(site_log, "control");
  }

  void stop_all() {
    site_process.kill();
    relay_process.kill();
    federation_process.kill();
    static_cast<void>(site_process.wait());
    static_cast<void>(relay_process.wait());
    static_cast<void>(federation_process.wait());
  }
};

}  // namespace

DCF_TEST(cluster, the_programs_the_suite_launches_exist) {
  for (const char* path : {DCF_FEDERATIOND_PATH, DCF_SITED_PATH, DCF_RELAY_PATH, DCF_CTL_PATH}) {
    dcf_ctx.note(std::string("program: ") + path);
    DCF_CHECK(fs::exists(path));
  }
}

DCF_TEST(cluster, membership_activation_partition_reconnect) {
  const std::string root = dcf::test::make_scratch_directory("cluster-partition");
  Cluster cluster;
  cluster.root = root;
  cluster.federation_log = root + "/federation.log";
  cluster.relay_log = root + "/relay.log";
  cluster.site_log = root + "/site.log";

  const bool federation_started = cluster.start_federation(DCF_FEDERATIOND_PATH, "");
  DCF_CHECK_EQ(cluster.last_error, std::string{});
  DCF_REQUIRE(federation_started);
  const bool relay_started = cluster.start_relay(DCF_RELAY_PATH);
  DCF_CHECK_EQ(cluster.last_error, std::string{});
  DCF_REQUIRE(relay_started);
  const bool site_started =
      cluster.start_site(DCF_SITED_PATH, kSiteId, "alpha", root + "/alpha.state", cluster.site_log);
  DCF_CHECK_EQ(cluster.last_error, std::string{});
  DCF_REQUIRE(site_started);

  // A compatibility window has to exist before a site can be validated.
  DCF_CHECK_EQ(submit_operator(cluster.federation,
                               dcf::DeclareWindowCommand{[] {
                                 dcf::CompatibilityWindow window;
                                 window.capability = dcf::CapabilityId{"dcf.membership"};
                                 window.offered = dcf::VersionRange{dcf::Version{1, 0},
                                                                    dcf::Version{3, 0}};
                                 window.effective_from = dcf::FederationGeneration{1};
                                 return window;
                               }()})
                   .code,
               dcf::ErrorCode::None);

  DCF_REQUIRE(dcf::test::wait_for_marker(cluster.site_log, "state candidate", 1200, 25));
  DCF_CHECK_EQ(submit_operator(cluster.federation, dcf::ValidateSiteCommand{*dcf::parse_identifier<dcf::SiteTag>(kSiteId)}).code,
               dcf::ErrorCode::None);

  const std::string sites_before =
      query_operator(cluster.federation, dcf::wire::QueryKind::Sites, "");
  const std::uint64_t membership_generation = field_u64(sites_before, "membership_generation");
  DCF_CHECK_EQ(membership_generation, 2ULL);

  DCF_CHECK_EQ(submit_operator(cluster.federation,
                               dcf::AdmitSiteCommand{*dcf::parse_identifier<dcf::SiteTag>(kSiteId),
                                                     dcf::MembershipGeneration{membership_generation}})
                   .code,
               dcf::ErrorCode::None);
  DCF_CHECK(dcf::test::wait_for_marker(cluster.site_log, "state active", 1200, 25));

  const std::string summary =
      query_operator(cluster.federation, dcf::wire::QueryKind::Summary, "");
  DCF_CHECK(summary.find("\"sites\":1") != std::string::npos);

  // The relay cuts this site off. The federation observes the loss of the
  // session and records the site as partitioned; it does not infer a partition
  // from silence, it observes the path going away.
  LineClient relay = LineClient::open(cluster.relay_control);
  DCF_REQUIRE(relay.connected());
  DCF_REQUIRE(relay.send_line(std::string("BLOCK ") + kSiteId));
  DCF_CHECK(relay.read_line().rfind("OK blocked", 0) == 0);
  // The federation notices because the path is gone, not because a flag was set.
  DCF_CHECK(dcf::test::wait_for_marker(cluster.federation_log, "", 1, 0));
  Poller poller(cluster.federation);
  bool saw_partition = false;
  for (int attempt = 0; attempt < 600 && !saw_partition; ++attempt) {
    saw_partition = poller.sites().find("\"state\":\"partitioned\"") != std::string::npos;
    if (!saw_partition) {
      settle(100);
    }
  }
  DCF_CHECK(saw_partition);

  const std::string partitioned =
      query_operator(cluster.federation, dcf::wire::QueryKind::Sites, "");
  DCF_CHECK(partitioned.find("\"state\":\"partitioned\"") != std::string::npos);

  // While the federation cannot reach it, the site keeps working locally. This
  // is the point of the boundary: disconnected is not stopped.
  LineClient site_control = LineClient::open(Address{"127.0.0.1", cluster.site_control_port()});
  DCF_REQUIRE(site_control.connected());
  for (int index = 1; index <= 3; ++index) {
    DCF_REQUIRE(site_control.send_line("LOCAL-OP"));
    DCF_CHECK_EQ(site_control.read_line(),
                 std::string("OK local-op epoch ") + std::to_string(index));
  }
  DCF_REQUIRE(site_control.send_line("STATUS"));
  const std::string site_status = site_control.read_line();
  DCF_CHECK_EQ(field_u64(site_status, "epoch"), 3ULL);
  DCF_CHECK_EQ(field_u64(site_status, "accepted_generation"), 6ULL);

  // Healing the link produces a real reconnect and a real reconciliation.
  DCF_REQUIRE(relay.send_line(std::string("ALLOW ") + kSiteId));
  DCF_CHECK(relay.read_line().rfind("OK allowed", 0) == 0);
  DCF_CHECK(dcf::test::wait_for_marker(cluster.site_log, "reconciliation site_adopts_federation",
                                       600, 25));

  const std::string healed =
      query_operator(cluster.federation, dcf::wire::QueryKind::Sites, "");
  DCF_CHECK(healed.find("\"state\":\"active\"") != std::string::npos);
  DCF_CHECK_EQ(field_u64(healed, "local_epoch"), 3ULL);
  const std::uint64_t accepted = field_u64(healed, "accepted_generation");
  DCF_CHECK(accepted > 6ULL);

  const std::string reconciliations =
      query_operator(cluster.federation, dcf::wire::QueryKind::Reconciliations, "");
  DCF_CHECK(reconciliations.find("\"outcome\":\"site_adopts_federation\"") != std::string::npos);

  cluster.stop_all();
}

DCF_TEST(cluster, the_installed_cli_drives_the_federation) {
  const std::string root = dcf::test::make_scratch_directory("cluster-cli");
  Cluster cluster;
  cluster.root = root;
  cluster.federation_log = root + "/federation.log";
  cluster.relay_log = root + "/relay.log";
  cluster.site_log = root + "/site.log";

  DCF_REQUIRE(cluster.start_federation(DCF_FEDERATIOND_PATH, ""));

  const std::string output = root + "/cli.log";
  dcf::test::ChildProcess cli = dcf::test::ChildProcess::spawn(
      DCF_CTL_PATH, {"--endpoint", cluster.federation.to_string(), "sites"}, output);
  DCF_REQUIRE(cli.valid());
  static_cast<void>(cli.wait());
  DCF_CHECK(dcf::test::read_text_file(output).find("[]") != std::string::npos);

  const std::string version_output = root + "/cli-version.log";
  dcf::test::ChildProcess version = dcf::test::ChildProcess::spawn(
      DCF_CTL_PATH, {"--endpoint", cluster.federation.to_string(), "version"}, version_output);
  DCF_REQUIRE(version.valid());
  static_cast<void>(version.wait());
  const std::string text = dcf::test::read_text_file(version_output);
  DCF_CHECK(text.find("data-center-federation") != std::string::npos);
  DCF_CHECK(text.find("protocol_version") != std::string::npos);

  cluster.stop_all();
}

DCF_TEST(cluster, a_killed_federation_recovers_from_its_own_store) {
  const std::string root = dcf::test::make_scratch_directory("cluster-restart");
  Cluster cluster;
  cluster.root = root;
  cluster.federation_log = root + "/federation.log";
  cluster.relay_log = root + "/relay.log";
  cluster.site_log = root + "/site.log";

  DCF_REQUIRE(cluster.start_federation(DCF_FEDERATIOND_PATH, ""));
  DCF_REQUIRE(cluster.start_relay(DCF_RELAY_PATH));
  DCF_REQUIRE(cluster.start_site(DCF_SITED_PATH, kSiteId, "alpha", root + "/alpha.state",
                                 cluster.site_log));

  DCF_REQUIRE(submit_operator(cluster.federation,
                              dcf::DeclareWindowCommand{[] {
                                dcf::CompatibilityWindow window;
                                window.capability = dcf::CapabilityId{"dcf.membership"};
                                window.offered = dcf::VersionRange{dcf::Version{1, 0},
                                                                   dcf::Version{3, 0}};
                                window.effective_from = dcf::FederationGeneration{1};
                                return window;
                              }()})
                  .code == dcf::ErrorCode::None);
  DCF_REQUIRE(submit_operator(cluster.federation,
                              dcf::ValidateSiteCommand{
                                  *dcf::parse_identifier<dcf::SiteTag>(kSiteId)})
                  .code == dcf::ErrorCode::None);
  const std::uint64_t membership_generation =
      field_u64(query_operator(cluster.federation, dcf::wire::QueryKind::Sites, ""),
                "membership_generation");
  DCF_REQUIRE(submit_operator(
                  cluster.federation,
                  dcf::AdmitSiteCommand{*dcf::parse_identifier<dcf::SiteTag>(kSiteId),
                                        dcf::MembershipGeneration{membership_generation}})
                  .code == dcf::ErrorCode::None);
  DCF_REQUIRE(dcf::test::wait_for_marker(cluster.site_log, "state active", 1200, 25));

  const std::string before =
      query_operator(cluster.federation, dcf::wire::QueryKind::Summary, "");
  const std::string authority_before = field_string(before, "authority_digest");
  DCF_CHECK(!authority_before.empty());

  // The federation process dies without warning. This is the crash boundary the
  // durable store has to survive.
  cluster.federation_process.kill();
  static_cast<void>(cluster.federation_process.wait());
  settle(200);

  // Bring it back on the same store.
  const std::string restart_log = root + "/federation-restart.log";
  cluster.federation_log = restart_log;
  DCF_REQUIRE(cluster.start_federation(DCF_FEDERATIOND_PATH, ""));
  DCF_CHECK(dcf::test::read_text_file(restart_log).find("recovery reopened=1") !=
            std::string::npos);

  const std::string after = query_operator(cluster.federation, dcf::wire::QueryKind::Summary, "");
  DCF_CHECK_EQ(field_string(after, "authority_digest"), authority_before);
  DCF_CHECK_EQ(field_u64(after, "sites"), 1ULL);

  const std::string sites = query_operator(cluster.federation, dcf::wire::QueryKind::Sites, "");
  DCF_CHECK(sites.find("\"state\":\"active\"") != std::string::npos);
  DCF_CHECK(sites.find(kSiteId) != std::string::npos);

  cluster.stop_all();
}

DCF_TEST(cluster, a_member_removed_during_a_partition_is_fenced_on_reconnect) {
  const std::string root = dcf::test::make_scratch_directory("cluster-fencing");
  Cluster cluster;
  cluster.root = root;
  cluster.federation_log = root + "/federation.log";
  cluster.relay_log = root + "/relay.log";
  cluster.site_log = root + "/site.log";

  DCF_REQUIRE(cluster.start_federation(DCF_FEDERATIOND_PATH, ""));
  DCF_REQUIRE(cluster.start_relay(DCF_RELAY_PATH));
  DCF_REQUIRE(cluster.start_site(DCF_SITED_PATH, kSiteId, "alpha", root + "/alpha.state",
                                 cluster.site_log));

  DCF_REQUIRE(submit_operator(cluster.federation,
                              dcf::DeclareWindowCommand{[] {
                                dcf::CompatibilityWindow window;
                                window.capability = dcf::CapabilityId{"dcf.membership"};
                                window.offered = dcf::VersionRange{dcf::Version{1, 0},
                                                                   dcf::Version{3, 0}};
                                window.effective_from = dcf::FederationGeneration{1};
                                return window;
                              }()})
                  .code == dcf::ErrorCode::None);
  DCF_REQUIRE(submit_operator(cluster.federation,
                              dcf::ValidateSiteCommand{
                                  *dcf::parse_identifier<dcf::SiteTag>(kSiteId)})
                  .code == dcf::ErrorCode::None);
  const std::uint64_t membership_generation =
      field_u64(query_operator(cluster.federation, dcf::wire::QueryKind::Sites, ""),
                "membership_generation");
  DCF_REQUIRE(submit_operator(
                  cluster.federation,
                  dcf::AdmitSiteCommand{*dcf::parse_identifier<dcf::SiteTag>(kSiteId),
                                        dcf::MembershipGeneration{membership_generation}})
                  .code == dcf::ErrorCode::None);
  DCF_REQUIRE(dcf::test::wait_for_marker(cluster.site_log, "state active", 1200, 25));

  LineClient relay = LineClient::open(cluster.relay_control);
  DCF_REQUIRE(relay.connected());
  DCF_REQUIRE(relay.send_line(std::string("BLOCK ") + kSiteId));
  DCF_CHECK(relay.read_line().rfind("OK blocked", 0) == 0);
  Poller poller(cluster.federation);
  bool partitioned_seen = false;
  for (int attempt = 0; attempt < 600 && !partitioned_seen; ++attempt) {
    partitioned_seen = poller.sites().find("\"state\":\"partitioned\"") != std::string::npos;
    if (!partitioned_seen) {
      settle(100);
    }
  }
  DCF_CHECK(partitioned_seen);

  // The member is removed while it cannot hear anything about it.
  DCF_CHECK_EQ(submit_operator(cluster.federation,
                               dcf::RemoveSiteCommand{*dcf::parse_identifier<dcf::SiteTag>(kSiteId),
                                                      "decommissioned during a partition"})
                   .code,
               dcf::ErrorCode::None);

  DCF_REQUIRE(relay.send_line(std::string("ALLOW ") + kSiteId));
  DCF_CHECK(relay.read_line().rfind("OK allowed", 0) == 0);

  DCF_CHECK(dcf::test::wait_for_marker(cluster.site_log,
                                       "reconciliation site_adopts_and_remains_fenced", 1200, 25));
  DCF_CHECK(dcf::test::wait_for_marker(cluster.site_log, "fenced by-federation", 200, 25));

  const std::string sites = query_operator(cluster.federation, dcf::wire::QueryKind::Sites, "");
  DCF_CHECK(sites.find("\"state\":\"removed\"") != std::string::npos);
  DCF_CHECK(sites.find("\"state\":\"active\"") == std::string::npos);

  const std::string reconciliations =
      query_operator(cluster.federation, dcf::wire::QueryKind::Reconciliations, "");
  DCF_CHECK(reconciliations.find("\"outcome\":\"site_adopts_and_remains_fenced\"") !=
            std::string::npos);

  cluster.stop_all();
}

DCF_TEST(cluster, an_exclusive_delegation_cannot_be_double_granted_across_a_partition) {
  const std::string root = dcf::test::make_scratch_directory("cluster-exclusive");
  Cluster cluster;
  cluster.root = root;
  cluster.federation_log = root + "/federation.log";
  cluster.relay_log = root + "/relay.log";
  cluster.site_log = root + "/site.log";

  DCF_REQUIRE(cluster.start_federation(DCF_FEDERATIOND_PATH, ""));
  DCF_REQUIRE(cluster.start_relay(DCF_RELAY_PATH));
  DCF_REQUIRE(cluster.start_site(DCF_SITED_PATH, kSiteId, "alpha", root + "/alpha.state",
                                 cluster.site_log));

  const auto first_site = *dcf::parse_identifier<dcf::SiteTag>(kSiteId);
  const auto second_site = *dcf::parse_identifier<dcf::SiteTag>(kSecondSiteId);
  DCF_REQUIRE(submit_operator(cluster.federation,
                              dcf::RegisterSiteCommand{second_site, "beta", [] {
                                dcf::CompatibilityDeclaration declaration;
                                declaration.implementation = "summon.second-site";
                                declaration.implementation_version = dcf::Version{1, 0};
                                declaration.protocol = dcf::VersionRange{dcf::Version{1, 0},
                                                                        dcf::Version{9, 9}};
                                dcf::CapabilityDeclaration entry;
                                entry.id = dcf::CapabilityId{"dcf.membership"};
                                entry.supported = dcf::VersionRange{dcf::Version{1, 0},
                                                                    dcf::Version{2, 0}};
                                declaration.capabilities.push_back(entry);
                                return declaration;
                              }()})
                  .code == dcf::ErrorCode::None);

  const auto site_generation = [&cluster](const char* identifier) {
    const std::string json = query_operator(cluster.federation, dcf::wire::QueryKind::Site,
                                            identifier);
    return field_u64(json, "membership_generation");
  };

  dcf::DelegationGrant first;
  first.id = dcf::DelegationId{0x51ULL, 0x01ULL};
  first.grantee = first_site;
  first.scope_mask = dcf::mask_of(dcf::DelegationScope::ReconciliationWitness);
  first.grantee_membership_generation =
      dcf::MembershipGeneration{site_generation(kSiteId)};
  first.exclusive = true;
  first.survives_partition = true;
  DCF_CHECK_EQ(submit_operator(cluster.federation, dcf::GrantDelegationCommand{first}).code,
               dcf::ErrorCode::None);

  dcf::DelegationGrant second = first;
  second.id = dcf::DelegationId{0x51ULL, 0x02ULL};
  second.grantee = second_site;
  second.grantee_membership_generation =
      dcf::MembershipGeneration{site_generation(kSecondSiteId)};
  const dcf::CommandOutcome refused =
      submit_operator(cluster.federation, dcf::GrantDelegationCommand{second});
  DCF_CHECK_EQ(refused.code, dcf::ErrorCode::Conflict);
  DCF_CHECK(refused.detail.find("exclusive") != std::string::npos);

  cluster.stop_all();
}
