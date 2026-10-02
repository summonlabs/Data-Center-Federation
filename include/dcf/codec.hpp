// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dcf/types.hpp"

namespace dcf {

// Canonical byte serialization.
//
// One encoder serves three callers: the durable journal, the wire protocol, and
// the state digest. Because the same bytes describe a value in all three
// places, a digest computed over stored state and a digest computed over a
// message that carries the same state agree by construction.
//
// The encoding is:
//   * fixed-width unsigned integers in little-endian byte order;
//   * signed integers as their two's-complement bit pattern, little-endian;
//   * booleans as a single byte that must be 0 or 1;
//   * text as a 32-bit byte length followed by the raw UTF-8 bytes;
//   * blobs as a 64-bit byte length followed by the raw bytes;
//   * collections as a 32-bit item count followed by the items in order.
//
// There is exactly one encoding of any value, so a digest is a function of the
// value and not of the path that produced it.
class Encoder {
 public:
  Encoder() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void text(std::string_view value);
  void blob(std::span<const std::uint8_t> value);
  void raw(std::span<const std::uint8_t> value);
  void raw(std::string_view value);

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
  [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
    return std::span<const std::uint8_t>(buffer_.data(), buffer_.size());
  }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

  void reserve(std::size_t amount) { buffer_.reserve(amount); }
  void clear() noexcept { buffer_.clear(); }

 private:
  std::vector<std::uint8_t> buffer_{};
};

// The decoder is the adversarial boundary. Every read is bounds checked against
// the remaining bytes, every length is checked against the configured limits
// before allocation, every boolean must be exactly 0 or 1, and every text field
// must be valid UTF-8. A decode that leaves unread bytes behind is a failure,
// not a silently accepted prefix.
class Decoder {
 public:
  Decoder(std::span<const std::uint8_t> data, const Limits& limits) noexcept
      : data_(data), limits_(limits) {}
  explicit Decoder(std::span<const std::uint8_t> data) noexcept
      : data_(data), limits_(default_limits()) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::string> text();
  [[nodiscard]] Result<std::vector<std::uint8_t>> blob();
  [[nodiscard]] Result<std::span<const std::uint8_t>> raw(std::size_t count);
  // A collection count, already checked against the per-object limit.
  [[nodiscard]] Result<std::uint32_t> collection_count();

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
  // Fails when bytes remain. Every decode of a whole object ends with this so
  // that trailing or padded input is rejected rather than ignored.
  [[nodiscard]] Result<Ack> require_end() const;

 private:
  [[nodiscard]] Result<std::span<const std::uint8_t>> take(std::size_t count);

  std::span<const std::uint8_t> data_{};
  Limits limits_{};
  std::size_t offset_{0};
};

[[nodiscard]] inline std::string_view to_string(const std::vector<std::uint8_t>& bytes) noexcept {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace dcf
