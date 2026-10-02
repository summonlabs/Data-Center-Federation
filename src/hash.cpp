// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/hash.hpp"

#include <algorithm>
#include <cstring>

namespace dcf {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::array<std::uint32_t, 8> kInitialState{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                                     0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                                     0x1f83d9abU, 0x5be0cd19U};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value,
                                                   unsigned amount) noexcept {
  return (value >> amount) | (value << (32U - amount));
}

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0x82f63b78U) : (crc >> 1);
    }
    table[index] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

}  // namespace

Sha256::Sha256() noexcept : state_(kInitialState) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t offset = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                      (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                      static_cast<std::uint32_t>(block[offset + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotate_right(schedule[index - 15], 7) ^
                             rotate_right(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotate_right(schedule[index - 2], 17) ^
                             rotate_right(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
  if (data.empty()) {
    return;
  }
  total_bytes_ += static_cast<std::uint64_t>(data.size());

  const std::uint8_t* cursor = data.data();
  std::size_t remaining = data.size();

  if (buffered_ > 0) {
    const std::size_t wanted = block_bytes - buffered_;
    const std::size_t taken = std::min(wanted, remaining);
    std::memcpy(buffer_.data() + buffered_, cursor, taken);
    buffered_ += taken;
    cursor += taken;
    remaining -= taken;
    if (buffered_ == block_bytes) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (remaining >= block_bytes) {
    compress(cursor);
    cursor += block_bytes;
    remaining -= block_bytes;
  }

  if (remaining > 0) {
    std::memcpy(buffer_.data(), cursor, remaining);
    buffered_ = remaining;
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                       text.size()));
}

Digest Sha256::finalize() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8U;

  std::array<std::uint8_t, block_bytes * 2> padding{};
  const std::size_t padding_length = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
  padding[0] = 0x80U;
  for (std::size_t index = 0; index < 8; ++index) {
    padding[padding_length + index] =
        static_cast<std::uint8_t>((bit_length >> ((7U - index) * 8U)) & 0xffU);
  }

  // The padding is applied through update(), which keeps the length bookkeeping
  // in one place. total_bytes_ is captured first because update() advances it.
  const std::size_t total_padding = padding_length + 8;
  const std::uint64_t saved_total = total_bytes_;
  update(std::span<const std::uint8_t>(padding.data(), total_padding));
  total_bytes_ = saved_total;

  Digest digest;
  for (std::size_t index = 0; index < 8; ++index) {
    const std::uint32_t word = state_[index];
    digest.bytes[index * 4] = static_cast<std::uint8_t>((word >> 24) & 0xffU);
    digest.bytes[index * 4 + 1] = static_cast<std::uint8_t>((word >> 16) & 0xffU);
    digest.bytes[index * 4 + 2] = static_cast<std::uint8_t>((word >> 8) & 0xffU);
    digest.bytes[index * 4 + 3] = static_cast<std::uint8_t>(word & 0xffU);
  }

  state_ = kInitialState;
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
  return digest;
}

std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept {
  std::uint32_t crc = seed;
  for (const std::uint8_t byte : data) {
    crc = kCrc32cTable[static_cast<std::size_t>((crc ^ byte) & 0xffU)] ^ (crc >> 8);
  }
  return crc;
}

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
  return crc32c_extend(0xffffffffU, data) ^ 0xffffffffU;
}

std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size()));
}

bool is_valid_utf8(std::string_view text) noexcept {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  std::size_t index = 0;

  while (index < size) {
    const unsigned char lead = bytes[index];
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;

    if (lead < 0x80U) {
      ++index;
      continue;
    }
    if ((lead & 0xe0U) == 0xc0U) {
      continuation = 1;
      code_point = lead & 0x1fU;
    } else if ((lead & 0xf0U) == 0xe0U) {
      continuation = 2;
      code_point = lead & 0x0fU;
    } else if ((lead & 0xf8U) == 0xf0U) {
      continuation = 3;
      code_point = lead & 0x07U;
      if (code_point > 4U) {
        return false;
      }
    } else {
      return false;
    }

    if (index + continuation >= size) {
      return false;
    }
    for (std::size_t step = 1; step <= continuation; ++step) {
      const unsigned char next = bytes[index + step];
      if ((next & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3fU);
    }

    static constexpr std::array<std::uint32_t, 4> kMinimumCodePoint{0U, 0x80U, 0x800U, 0x10000U};
    if (code_point < kMinimumCodePoint[continuation]) {
      return false;
    }
    if (code_point > 0x10ffffU) {
      return false;
    }
    if (code_point >= 0xd800U && code_point <= 0xdfffU) {
      return false;
    }

    index += continuation + 1;
  }
  return true;
}

std::string sanitize_for_terminal(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    const auto value = static_cast<unsigned char>(character);
    if (value >= 0x20U && value < 0x7fU) {
      out.push_back(character);
    } else {
      out.push_back('?');
    }
  }
  return out;
}

}  // namespace dcf
