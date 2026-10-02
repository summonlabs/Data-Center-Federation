// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dcf/codec.hpp"
#include "dcf/command.hpp"
#include "dcf/types.hpp"

namespace dcf::wire {
class Connection;
}

namespace dcf::wire {

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
// Every message on the wire is one frame:
//
//   u32 payload length | u32 CRC-32C of the payload | payload
//
// The same framing, and the same canonical codec, describes a durable journal
// record and a network message. A peer cannot present bytes that the journal
// would have accepted but the decoder would interpret differently, because there
// is only one decoder.
inline constexpr std::uint32_t kFrameOverhead = 8;

enum class MessageKind : std::uint8_t {
  Hello = 0,
  HelloAck = 1,
  CommandMessage = 2,
  Outcome = 3,
  Publish = 4,
  Query = 5,
  QueryResult = 6,
  Bye = 7,
};

enum class PeerKind : std::uint8_t {
  Site = 0,
  Operator = 1,
};

struct Hello {
  PeerKind peer{PeerKind::Site};
  std::uint32_t protocol_major{0};
  std::uint32_t protocol_minor{0};
  SiteId site{};
  std::string implementation{};

  friend bool operator==(const Hello&, const Hello&) = default;
};

struct HelloAck {
  bool accepted{false};
  std::string detail{};
  FederationId federation{};
  FederationGeneration generation{};
  Digest authority_digest{};
  // The name the peer should use when it announces itself to a relay, so that a
  // relay can partition one site without partitioning the federation.
  std::string relay_token{};

  friend bool operator==(const HelloAck&, const HelloAck&) = default;
};

// What the federation tells a connected site about itself. A site uses this to
// learn its membership generation and to know which generation it has accepted.
struct Publish {
  FederationGeneration generation{};
  Digest authority_digest{};
  Digest site_digest{};
  MembershipGeneration membership_generation{};
  std::uint8_t membership_state{0};
  JournalSequence sequence{};

  friend bool operator==(const Publish&, const Publish&) = default;
};

struct OutcomeMessage {
  ErrorCode code{ErrorCode::None};
  std::string detail{};
  FederationGeneration generation{};
  MembershipGeneration membership_generation{};
  std::string reconciliation_outcome{};
  Digest adopt_history{};
  std::uint64_t adopt_generation{0};
  bool fenced{false};

  friend bool operator==(const OutcomeMessage&, const OutcomeMessage&) = default;
};

enum class QueryKind : std::uint8_t {
  Version = 0,
  Summary = 1,
  Sites = 2,
  Site = 3,
  Delegations = 4,
  Windows = 5,
  Reconciliations = 6,
  Receipts = 7,
  Recovery = 8,
  Stats = 9,
};

struct Query {
  QueryKind kind{QueryKind::Summary};
  std::string argument{};

  friend bool operator==(const Query&, const Query&) = default;
};

struct QueryResult {
  bool ok{true};
  std::string body{};

  friend bool operator==(const QueryResult&, const QueryResult&) = default;
};

struct Message {
  MessageKind kind{MessageKind::Bye};
  Hello hello{};
  HelloAck hello_ack{};
  Command command{};
  OutcomeMessage outcome{};
  Publish publish{};
  Query query{};
  QueryResult query_result{};
  std::string bye_reason{};
};

void encode(Encoder& encoder, const Message& message);
[[nodiscard]] Result<Message> decode_message(Decoder& decoder, const Limits& limits);

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------
// A blocking TCP transport with an explicit receive deadline. The deadline is
// part of the protocol, not a test convenience: a peer that stops answering is a
// fault that has to be reported, and a client that waits forever cannot report
// anything.

struct Address {
  std::string host{};
  std::uint16_t port{0};

  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] Result<Address> parse_address(std::string_view text);

class Connection {
 public:
  Connection() = default;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;
  ~Connection();

  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;

  // Raw handle access. A relay forwards bytes without parsing them, so it needs
  // the underlying socket; nothing else in the runtime uses these.
  [[nodiscard]] std::uintptr_t handle() const noexcept { return socket_; }
  [[nodiscard]] std::uintptr_t release_handle() noexcept;
  [[nodiscard]] static Connection adopt(std::uintptr_t handle) noexcept;

  [[nodiscard]] Result<Ack> send(const Message& message, const Limits& limits);
  [[nodiscard]] Result<Message> receive(const Limits& limits);
  // Milliseconds; zero means wait indefinitely.
  void set_receive_deadline_ms(std::uint32_t millis) noexcept { receive_deadline_ms_ = millis; }
  [[nodiscard]] Result<std::uint16_t> local_port() const;

 private:
  friend class Listener;
  friend Result<Connection> connect(const Address& address);

  explicit Connection(std::uintptr_t socket) noexcept : socket_(socket) {}

  std::uintptr_t socket_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
  std::uint32_t receive_deadline_ms_{0};
};

class Listener {
 public:
  Listener() = default;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  ~Listener();

  [[nodiscard]] static Result<Listener> bind(const Address& address, std::uint16_t backlog = 16);
  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;
  // Blocks until a peer connects. When a deadline is set the accept reports
  // Expired rather than waiting forever, which lets a host observe its own
  // shutdown condition.
  [[nodiscard]] Result<Connection> accept();
  void set_accept_deadline_ms(std::uint32_t millis) noexcept { accept_deadline_ms_ = millis; }
  [[nodiscard]] Result<std::uint16_t> port() const;

 private:
  std::uintptr_t socket_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
  std::uint32_t accept_deadline_ms_{0};
};

[[nodiscard]] Result<Connection> connect(const Address& address);

// ---------------------------------------------------------------------------
// The relay preamble
// ---------------------------------------------------------------------------
// A relay forwards bytes between sites and the federation, and partitions one
// site by closing exactly that site's connections. To do that it has to know
// which connection belongs to which site, so a client announces itself with one
// line before the federation protocol begins. The line is consumed by the relay
// and never reaches the federation.
inline constexpr const char* kRelayMagic = "DCF-RELAY-CLIENT ";

[[nodiscard]] std::string relay_preamble(const std::string& token);

}  // namespace dcf::wire
