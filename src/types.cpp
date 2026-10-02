// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/types.hpp"

#include <cstdio>

#include "dcf/hash.hpp"

namespace dcf {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] constexpr int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

}  // namespace

std::string to_hex(std::uint64_t high, std::uint64_t low) {
  std::string out;
  out.resize(32);
  for (int index = 15; index >= 0; --index) {
    const auto shift = static_cast<unsigned>(index) * 4U;
    const auto nibble_high = static_cast<std::uint8_t>((high >> shift) & 0xFU);
    const auto nibble_low = static_cast<std::uint8_t>((low >> shift) & 0xFU);
    out[static_cast<std::size_t>(15 - index)] = kHexDigits[nibble_high];
    out[static_cast<std::size_t>(31 - index)] = kHexDigits[nibble_low];
  }
  return out;
}

std::optional<std::uint64_t> parse_hex64(std::string_view text) noexcept {
  if (text.size() != 16) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    const int nibble = hex_value(character);
    if (nibble < 0) {
      return std::nullopt;
    }
    value = (value << 4) | static_cast<std::uint64_t>(nibble);
  }
  return value;
}

std::string Version::to_string() const {
  std::string out = std::to_string(major);
  out.push_back('.');
  out.append(std::to_string(minor));
  return out;
}

std::string Digest::to_hex() const {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    out.push_back(kHexDigits[(byte >> 4) & 0xFU]);
    out.push_back(kHexDigits[byte & 0xFU]);
  }
  return out;
}

std::optional<Digest> Digest::from_hex(std::string_view text) noexcept {
  if (text.size() != 64) {
    return std::nullopt;
  }
  Digest digest;
  for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    digest.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return digest;
}

Digest digest_of(std::string_view bytes) {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finalize();
}

Clock::~Clock() = default;

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::None:
      return "none";
    case ErrorCode::InvalidArgument:
      return "invalid_argument";
    case ErrorCode::MalformedInput:
      return "malformed_input";
    case ErrorCode::BoundsExceeded:
      return "bounds_exceeded";
    case ErrorCode::ArithmeticOverflow:
      return "arithmetic_overflow";
    case ErrorCode::NotFound:
      return "not_found";
    case ErrorCode::DuplicateIdentity:
      return "duplicate_identity";
    case ErrorCode::Conflict:
      return "conflict";
    case ErrorCode::StaleGeneration:
      return "stale_generation";
    case ErrorCode::Revoked:
      return "revoked";
    case ErrorCode::Incompatible:
      return "incompatible";
    case ErrorCode::InsufficientAuthority:
      return "insufficient_authority";
    case ErrorCode::PartitionSuspected:
      return "partition_suspected";
    case ErrorCode::Unsupported:
      return "unsupported";
    case ErrorCode::Indeterminate:
      return "indeterminate";
    case ErrorCode::IdempotencyConflict:
      return "idempotency_conflict";
    case ErrorCode::Corruption:
      return "corruption";
    case ErrorCode::TornTail:
      return "torn_tail";
    case ErrorCode::InteriorCorruption:
      return "interior_corruption";
    case ErrorCode::Io:
      return "io";
    case ErrorCode::Locked:
      return "locked";
    case ErrorCode::Busy:
      return "busy";
    case ErrorCode::Closed:
      return "closed";
    case ErrorCode::CapacityExhausted:
      return "capacity_exhausted";
    case ErrorCode::ShuttingDown:
      return "shutting_down";
    case ErrorCode::RefusedByPolicy:
      return "refused_by_policy";
    case ErrorCode::Internal:
      return "internal";
  }
  return "unknown";
}

const Limits& default_limits() noexcept {
  static const Limits limits{};
  return limits;
}

}  // namespace dcf
