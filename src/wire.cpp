// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/wire.hpp"

#include <cstring>

#include "dcf/hash.hpp"
#include "record_codec.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

namespace dcf::wire {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalid = INVALID_SOCKET;

struct WinsockGuard {
  WinsockGuard() {
    WSADATA data{};
    started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~WinsockGuard() {
    if (started) {
      WSACleanup();
    }
  }
  bool started{false};
};

[[nodiscard]] bool ensure_sockets() {
  static WinsockGuard guard;
  return guard.started;
}

[[nodiscard]] int last_socket_error() { return WSAGetLastError(); }
[[nodiscard]] bool would_block(int code) { return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK; }
void close_socket(NativeSocket socket) { ::closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalid = -1;

[[nodiscard]] bool ensure_sockets() { return true; }

[[nodiscard]] int last_socket_error() { return errno; }
[[nodiscard]] bool would_block(int code) { return code == EAGAIN || code == EWOULDBLOCK || code == EINTR; }
void close_socket(NativeSocket socket) { ::close(socket); }
#endif

[[nodiscard]] NativeSocket to_native(std::uintptr_t value) {
  return static_cast<NativeSocket>(value);
}

[[nodiscard]] std::uintptr_t to_handle(NativeSocket socket) {
  return static_cast<std::uintptr_t>(socket);
}

constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

[[nodiscard]] std::string socket_error_text(const char* what) {
  std::string detail = what;
  detail.append(" failed with socket error ");
  detail.append(std::to_string(last_socket_error()));
  return detail;
}

[[nodiscard]] Result<Ack> write_all(NativeSocket socket, const std::uint8_t* data,
                                    std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
#ifdef _WIN32
    const int chunk = ::send(socket, reinterpret_cast<const char*>(data + written),
                             static_cast<int>(std::min<std::size_t>(size - written, 1U << 20)), 0);
#else
    const ssize_t chunk = ::send(socket, data + written, size - written, MSG_NOSIGNAL);
#endif
    if (chunk <= 0) {
      return make_error(ErrorCode::Io, socket_error_text("send"));
    }
    written += static_cast<std::size_t>(chunk);
  }
  return Ack{};
}

[[nodiscard]] Result<Ack> read_all(NativeSocket socket, std::uint8_t* data, std::size_t size) {
  std::size_t read = 0;
  while (read < size) {
#ifdef _WIN32
    const int chunk = ::recv(socket, reinterpret_cast<char*>(data + read),
                             static_cast<int>(std::min<std::size_t>(size - read, 1U << 20)), 0);
#else
    const ssize_t chunk = ::recv(socket, data + read, size - read, 0);
#endif
    if (chunk == 0) {
      return make_error(ErrorCode::Closed, "the peer closed the connection");
    }
    if (chunk < 0) {
      const int code = last_socket_error();
      if (would_block(code)) {
        return make_error(ErrorCode::Busy, "the peer did not answer within the deadline");
      }
      return make_error(ErrorCode::Io, socket_error_text("recv"));
    }
    read += static_cast<std::size_t>(chunk);
  }
  return Ack{};
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (static_cast<unsigned>(index) * 8U);
  }
  return value;
}

void encode_publish(Encoder& encoder, const Publish& publish) {
  encoder.u64(publish.generation.value());
  detail::put(encoder, publish.authority_digest);
  detail::put(encoder, publish.site_digest);
  encoder.u64(publish.membership_generation.value());
  encoder.u8(publish.membership_state);
  encoder.u64(publish.sequence.value());
}

Result<Publish> decode_publish(Decoder& decoder) {
  Publish publish;
  const auto generation = decoder.u64();
  if (!generation) {
    return generation.error();
  }
  const auto authority = detail::get_digest(decoder);
  if (!authority) {
    return authority.error();
  }
  const auto site = detail::get_digest(decoder);
  if (!site) {
    return site.error();
  }
  const auto membership = decoder.u64();
  if (!membership) {
    return membership.error();
  }
  const auto state = decoder.u8();
  if (!state) {
    return state.error();
  }
  const auto sequence = decoder.u64();
  if (!sequence) {
    return sequence.error();
  }
  publish.generation = FederationGeneration{generation.value()};
  publish.authority_digest = authority.value();
  publish.site_digest = site.value();
  publish.membership_generation = MembershipGeneration{membership.value()};
  publish.membership_state = state.value();
  publish.sequence = JournalSequence{sequence.value()};
  return publish;
}

}  // namespace

std::string Address::to_string() const {
  return host + ":" + std::to_string(port);
}

Result<Address> parse_address(std::string_view text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return make_error(ErrorCode::InvalidArgument,
                      "address '" + sanitize_for_terminal(std::string(text)) +
                          "' is not in host:port form");
  }
  Address address;
  address.host = std::string(text.substr(0, colon));
  const std::string_view port_text = text.substr(colon + 1);
  std::uint32_t port = 0;
  for (const char character : port_text) {
    if (character < '0' || character > '9') {
      return make_error(ErrorCode::InvalidArgument,
                        "address '" + sanitize_for_terminal(std::string(text)) +
                            "' has a non-numeric port");
    }
    port = port * 10U + static_cast<std::uint32_t>(character - '0');
    if (port > 65535U) {
      return make_error(ErrorCode::InvalidArgument,
                        "address '" + sanitize_for_terminal(std::string(text)) +
                            "' has a port above 65535");
    }
  }
  address.port = static_cast<std::uint16_t>(port);
  return address;
}

void encode(Encoder& encoder, const Message& message) {
  encoder.u8(static_cast<std::uint8_t>(message.kind));
  switch (message.kind) {
    case MessageKind::Hello:
      encoder.u8(static_cast<std::uint8_t>(message.hello.peer));
      encoder.u32(message.hello.protocol_major);
      encoder.u32(message.hello.protocol_minor);
      detail::put(encoder, message.hello.site);
      encoder.text(message.hello.implementation);
      break;
    case MessageKind::HelloAck:
      encoder.boolean(message.hello_ack.accepted);
      encoder.text(message.hello_ack.detail);
      detail::put(encoder, message.hello_ack.federation);
      encoder.u64(message.hello_ack.generation.value());
      detail::put(encoder, message.hello_ack.authority_digest);
      encoder.text(message.hello_ack.relay_token);
      break;
    case MessageKind::CommandMessage:
      detail::put(encoder, message.command);
      break;
    case MessageKind::Outcome:
      detail::put(encoder, message.outcome.code);
      encoder.text(message.outcome.detail);
      encoder.u64(message.outcome.generation.value());
      encoder.u64(message.outcome.membership_generation.value());
      encoder.text(message.outcome.reconciliation_outcome);
      detail::put(encoder, message.outcome.adopt_history);
      encoder.u64(message.outcome.adopt_generation);
      encoder.boolean(message.outcome.fenced);
      break;
    case MessageKind::Publish:
      encode_publish(encoder, message.publish);
      break;
    case MessageKind::Query:
      encoder.u8(static_cast<std::uint8_t>(message.query.kind));
      encoder.text(message.query.argument);
      break;
    case MessageKind::QueryResult:
      encoder.boolean(message.query_result.ok);
      encoder.text(message.query_result.body);
      break;
    case MessageKind::Bye:
      encoder.text(message.bye_reason);
      break;
  }
}

Result<Message> decode_message(Decoder& decoder, const Limits& limits) {
  const auto tag = decoder.u8();
  if (!tag) {
    return tag.error();
  }
  Message message;
  switch (static_cast<MessageKind>(tag.value())) {
    case MessageKind::Hello: {
      message.kind = MessageKind::Hello;
      const auto peer = decoder.u8();
      if (!peer) {
        return peer.error();
      }
      if (peer.value() > static_cast<std::uint8_t>(PeerKind::Operator)) {
        return make_error(ErrorCode::MalformedInput,
                          "hello names peer kind " + std::to_string(peer.value()));
      }
      message.hello.peer = static_cast<PeerKind>(peer.value());
      const auto major = decoder.u32();
      if (!major) {
        return major.error();
      }
      const auto minor = decoder.u32();
      if (!minor) {
        return minor.error();
      }
      message.hello.protocol_major = major.value();
      message.hello.protocol_minor = minor.value();
      const auto site = detail::get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      message.hello.site = site.value();
      const auto implementation = decoder.text();
      if (!implementation) {
        return implementation.error();
      }
      message.hello.implementation = implementation.value();
      break;
    }
    case MessageKind::HelloAck: {
      message.kind = MessageKind::HelloAck;
      const auto accepted = decoder.boolean();
      if (!accepted) {
        return accepted.error();
      }
      message.hello_ack.accepted = accepted.value();
      const auto detail = decoder.text();
      if (!detail) {
        return detail.error();
      }
      message.hello_ack.detail = detail.value();
      const auto federation = detail::get_identifier<FederationTag>(decoder);
      if (!federation) {
        return federation.error();
      }
      message.hello_ack.federation = federation.value();
      const auto generation = decoder.u64();
      if (!generation) {
        return generation.error();
      }
      message.hello_ack.generation = FederationGeneration{generation.value()};
      const auto digest = detail::get_digest(decoder);
      if (!digest) {
        return digest.error();
      }
      message.hello_ack.authority_digest = digest.value();
      const auto token = decoder.text();
      if (!token) {
        return token.error();
      }
      message.hello_ack.relay_token = token.value();
      break;
    }
    case MessageKind::CommandMessage: {
      message.kind = MessageKind::CommandMessage;
      const auto command = detail::get_command(decoder);
      if (!command) {
        return command.error();
      }
      message.command = command.value();
      break;
    }
    case MessageKind::Outcome: {
      message.kind = MessageKind::Outcome;
      const auto code = detail::get_error_code(decoder);
      if (!code) {
        return code.error();
      }
      message.outcome.code = code.value();
      const auto detail = decoder.text();
      if (!detail) {
        return detail.error();
      }
      message.outcome.detail = detail.value();
      const auto generation = decoder.u64();
      if (!generation) {
        return generation.error();
      }
      const auto membership = decoder.u64();
      if (!membership) {
        return membership.error();
      }
      message.outcome.generation = FederationGeneration{generation.value()};
      message.outcome.membership_generation = MembershipGeneration{membership.value()};
      const auto outcome_text = decoder.text();
      if (!outcome_text) {
        return outcome_text.error();
      }
      message.outcome.reconciliation_outcome = outcome_text.value();
      const auto digest = detail::get_digest(decoder);
      if (!digest) {
        return digest.error();
      }
      message.outcome.adopt_history = digest.value();
      const auto adopt = decoder.u64();
      if (!adopt) {
        return adopt.error();
      }
      message.outcome.adopt_generation = adopt.value();
      const auto fenced = decoder.boolean();
      if (!fenced) {
        return fenced.error();
      }
      message.outcome.fenced = fenced.value();
      break;
    }
    case MessageKind::Publish: {
      message.kind = MessageKind::Publish;
      const auto publish = decode_publish(decoder);
      if (!publish) {
        return publish.error();
      }
      message.publish = publish.value();
      break;
    }
    case MessageKind::Query: {
      message.kind = MessageKind::Query;
      const auto kind = decoder.u8();
      if (!kind) {
        return kind.error();
      }
      if (kind.value() > static_cast<std::uint8_t>(QueryKind::Stats)) {
        return make_error(ErrorCode::MalformedInput,
                          "query kind " + std::to_string(kind.value()) + " is not known");
      }
      message.query.kind = static_cast<QueryKind>(kind.value());
      const auto argument = decoder.text();
      if (!argument) {
        return argument.error();
      }
      message.query.argument = argument.value();
      break;
    }
    case MessageKind::QueryResult: {
      message.kind = MessageKind::QueryResult;
      const auto ok = decoder.boolean();
      if (!ok) {
        return ok.error();
      }
      message.query_result.ok = ok.value();
      const auto body = decoder.text();
      if (!body) {
        return body.error();
      }
      message.query_result.body = body.value();
      break;
    }
    case MessageKind::Bye: {
      message.kind = MessageKind::Bye;
      const auto reason = decoder.text();
      if (!reason) {
        return reason.error();
      }
      message.bye_reason = reason.value();
      break;
    }
    default:
      return make_error(ErrorCode::MalformedInput,
                        "message kind " + std::to_string(tag.value()) + " is not known");
  }
  const auto trailing = decoder.require_end();
  if (!trailing) {
    return trailing.error();
  }
  static_cast<void>(limits);
  return message;
}

Connection::Connection(Connection&& other) noexcept : socket_(other.socket_) {
  other.socket_ = kInvalidHandle;
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    close();
    socket_ = other.socket_;
    receive_deadline_ms_ = other.receive_deadline_ms_;
    other.socket_ = kInvalidHandle;
  }
  return *this;
}

Connection::~Connection() { close(); }

bool Connection::valid() const noexcept { return socket_ != kInvalidHandle; }

std::uintptr_t Connection::release_handle() noexcept {
  const std::uintptr_t handle = socket_;
  socket_ = kInvalidHandle;
  return handle;
}

Connection Connection::adopt(std::uintptr_t handle) noexcept { return Connection(handle); }

void Connection::close() noexcept {
  if (socket_ != kInvalidHandle) {
    close_socket(to_native(socket_));
    socket_ = kInvalidHandle;
  }
}

Result<Ack> Connection::send(const Message& message, const Limits& limits) {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "the connection is not open");
  }
  Encoder payload_encoder;
  encode(payload_encoder, message);
  const std::span<const std::uint8_t> payload = payload_encoder.view();
  if (payload.size() > limits.max_frame_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      "the encoded message is " + std::to_string(payload.size()) +
                          " bytes which exceeds the limit of " +
                          std::to_string(limits.max_frame_bytes));
  }
  std::vector<std::uint8_t> frame;
  frame.reserve(kFrameOverhead + payload.size());
  put_u32(frame, static_cast<std::uint32_t>(payload.size()));
  put_u32(frame, crc32c(payload));
  frame.insert(frame.end(), payload.begin(), payload.end());
  return write_all(to_native(socket_), frame.data(), frame.size());
}

Result<Message> Connection::receive(const Limits& limits) {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "the connection is not open");
  }
  if (receive_deadline_ms_ != 0) {
#ifdef _WIN32
    const DWORD deadline = receive_deadline_ms_;
    ::setsockopt(to_native(socket_), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&deadline), sizeof(deadline));
#else
    timeval deadline{};
    deadline.tv_sec = static_cast<time_t>(receive_deadline_ms_ / 1000U);
    deadline.tv_usec = static_cast<suseconds_t>((receive_deadline_ms_ % 1000U) * 1000U);
    ::setsockopt(to_native(socket_), SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
#endif
  }

  std::uint8_t header[kFrameOverhead];
  const auto read_header = read_all(to_native(socket_), header, sizeof(header));
  if (!read_header) {
    return read_header.error();
  }
  const std::uint32_t length = read_u32(header);
  const std::uint32_t expected_crc = read_u32(header + 4);
  if (static_cast<std::uint64_t>(length) > limits.max_frame_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      "a peer announced a frame of " + std::to_string(length) +
                          " bytes which exceeds the limit of " +
                          std::to_string(limits.max_frame_bytes));
  }
  std::vector<std::uint8_t> payload(length);
  if (length > 0) {
    const auto read_payload = read_all(to_native(socket_), payload.data(), payload.size());
    if (!read_payload) {
      return read_payload.error();
    }
  }
  if (crc32c(std::span<const std::uint8_t>(payload.data(), payload.size())) != expected_crc) {
    return make_error(ErrorCode::Corruption, "a frame arrived whose checksum does not match");
  }
  Decoder decoder(std::span<const std::uint8_t>(payload.data(), payload.size()), limits);
  return decode_message(decoder, limits);
}

Result<std::uint16_t> Connection::local_port() const {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "the connection is not open");
  }
  sockaddr_in address{};
#ifdef _WIN32
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  if (::getsockname(to_native(socket_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return make_error(ErrorCode::Io, socket_error_text("getsockname"));
  }
  return ntohs(address.sin_port);
}

Listener::Listener(Listener&& other) noexcept : socket_(other.socket_) {
  other.socket_ = kInvalidHandle;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    close();
    socket_ = other.socket_;
    accept_deadline_ms_ = other.accept_deadline_ms_;
    other.socket_ = kInvalidHandle;
  }
  return *this;
}

Listener::~Listener() { close(); }

bool Listener::valid() const noexcept { return socket_ != kInvalidHandle; }

void Listener::close() noexcept {
  if (socket_ != kInvalidHandle) {
    close_socket(to_native(socket_));
    socket_ = kInvalidHandle;
  }
}

Result<Listener> Listener::bind(const Address& address, std::uint16_t backlog) {
  if (!ensure_sockets()) {
    return make_error(ErrorCode::Io, "the platform socket layer could not be initialised");
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalid) {
    return make_error(ErrorCode::Io, socket_error_text("socket"));
  }
  int reuse = 1;
  static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                                 reinterpret_cast<const char*>(&reuse), sizeof(reuse)));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(address.port);
  if (::inet_pton(AF_INET, address.host.c_str(), &local.sin_addr) != 1) {
    close_socket(socket);
    return make_error(ErrorCode::InvalidArgument,
                      "host '" + sanitize_for_terminal(address.host) +
                          "' is not an IPv4 address");
  }
  if (::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    const std::string detail = socket_error_text("bind");
    close_socket(socket);
    return make_error(ErrorCode::Io, detail + " for " + address.to_string());
  }
  if (::listen(socket, static_cast<int>(backlog)) != 0) {
    const std::string detail = socket_error_text("listen");
    close_socket(socket);
    return make_error(ErrorCode::Io, detail);
  }
  Listener listener;
  listener.socket_ = to_handle(socket);
  return listener;
}

Result<Connection> Listener::accept() {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "the listener is not open");
  }
  if (accept_deadline_ms_ != 0) {
#ifdef _WIN32
    const DWORD deadline = accept_deadline_ms_;
    ::setsockopt(to_native(socket_), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&deadline), sizeof(deadline));
#else
    timeval deadline{};
    deadline.tv_sec = static_cast<time_t>(accept_deadline_ms_ / 1000U);
    deadline.tv_usec = static_cast<suseconds_t>((accept_deadline_ms_ % 1000U) * 1000U);
    ::setsockopt(to_native(socket_), SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
#endif
  }
  sockaddr_in peer{};
#ifdef _WIN32
  int length = sizeof(peer);
#else
  socklen_t length = sizeof(peer);
#endif
  const NativeSocket accepted =
      ::accept(to_native(socket_), reinterpret_cast<sockaddr*>(&peer), &length);
  if (accepted == kInvalid) {
    const int code = last_socket_error();
    if (would_block(code)) {
      return make_error(ErrorCode::Busy, "no peer connected within the deadline");
    }
    return make_error(ErrorCode::Io, socket_error_text("accept"));
  }
  int no_delay = 1;
  static_cast<void>(::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)));
  return Connection(to_handle(accepted));
}

Result<std::uint16_t> Listener::port() const {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "the listener is not open");
  }
  sockaddr_in address{};
#ifdef _WIN32
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  if (::getsockname(to_native(socket_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return make_error(ErrorCode::Io, socket_error_text("getsockname"));
  }
  return ntohs(address.sin_port);
}

Result<Connection> connect(const Address& address) {
  if (!ensure_sockets()) {
    return make_error(ErrorCode::Io, "the platform socket layer could not be initialised");
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalid) {
    return make_error(ErrorCode::Io, socket_error_text("socket"));
  }
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(address.port);
  if (::inet_pton(AF_INET, address.host.c_str(), &remote.sin_addr) != 1) {
    close_socket(socket);
    return make_error(ErrorCode::InvalidArgument,
                      "host '" + sanitize_for_terminal(address.host) +
                          "' is not an IPv4 address");
  }
  if (::connect(socket, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
    const std::string detail = socket_error_text("connect");
    close_socket(socket);
    return make_error(ErrorCode::Io, detail + " to " + address.to_string());
  }
  int no_delay = 1;
  static_cast<void>(::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)));
  return Connection(to_handle(socket));
}

std::string relay_preamble(const std::string& token) {
  std::string line = kRelayMagic;
  line.append(token);
  line.push_back('\n');
  return line;
}

}  // namespace dcf::wire
