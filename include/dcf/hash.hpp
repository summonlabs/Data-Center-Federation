// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dcf/types.hpp"

namespace dcf {

// Incremental SHA-256. Used for canonical state digests, history digests, and
// request digests. The implementation is first party so that a digest recorded
// by one build can be recomputed by any other build with no dependency.
class Sha256 {
 public:
  static constexpr std::size_t digest_bytes = 32;
  static constexpr std::size_t block_bytes = 64;

  Sha256() noexcept;

  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view text) noexcept;

  // Completes the digest. The object is reset and can be reused.
  [[nodiscard]] Digest finalize() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, block_bytes> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bytes_{0};
};

// CRC-32C (Castagnoli), the framing checksum for the durable journal and for
// the wire protocol. A checksum mismatch is how a torn record and a corrupted
// record are told apart from a complete one.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::string_view text) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;

// True when the byte string is well-formed UTF-8 with no overlong forms,
// surrogate code points, or values above U+10FFFF.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Replaces every byte that is not a printable ASCII character with '?'. Used
// only for terminal output of untrusted text, never for stored state.
[[nodiscard]] std::string sanitize_for_terminal(std::string_view text);

}  // namespace dcf
