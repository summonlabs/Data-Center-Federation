// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/codec.hpp"

#include <cstring>

#include "dcf/hash.hpp"

namespace dcf {
namespace {

[[nodiscard]] std::string limit_detail(const char* field, std::uint64_t requested,
                                       std::uint64_t limit) {
  std::string detail = "field '";
  detail.append(field);
  detail.append("' requested ");
  detail.append(std::to_string(requested));
  detail.append(" bytes/items which exceeds the limit of ");
  detail.append(std::to_string(limit));
  return detail;
}

}  // namespace

void Encoder::u8(std::uint8_t value) { buffer_.push_back(value); }

void Encoder::u16(std::uint16_t value) {
  buffer_.push_back(static_cast<std::uint8_t>(value & 0xffU));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffU));
}

void Encoder::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void Encoder::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void Encoder::i64(std::int64_t value) {
  const auto bits = static_cast<std::uint64_t>(value);
  u64(bits);
}

void Encoder::boolean(bool value) { u8(value ? 1U : 0U); }

void Encoder::text(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value);
}

void Encoder::blob(std::span<const std::uint8_t> value) {
  u64(static_cast<std::uint64_t>(value.size()));
  raw(value);
}

void Encoder::raw(std::span<const std::uint8_t> value) {
  if (value.empty()) {
    return;
  }
  const std::size_t offset = buffer_.size();
  buffer_.resize(offset + value.size());
  std::memcpy(buffer_.data() + offset, value.data(), value.size());
}

void Encoder::raw(std::string_view value) {
  raw(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()),
                                    value.size()));
}

Result<std::span<const std::uint8_t>> Decoder::take(std::size_t count) {
  if (count > remaining()) {
    std::string detail = "truncated: needed ";
    detail.append(std::to_string(count));
    detail.append(" bytes at offset ");
    detail.append(std::to_string(offset_));
    detail.append(" but only ");
    detail.append(std::to_string(remaining()));
    detail.append(" remain");
    return make_error(ErrorCode::MalformedInput, std::move(detail));
  }
  const std::span<const std::uint8_t> slice = data_.subspan(offset_, count);
  offset_ += count;
  return slice;
}

Result<std::uint8_t> Decoder::u8() {
  const auto slice = take(1);
  if (!slice) {
    return slice.error();
  }
  return slice.value()[0];
}

Result<std::uint16_t> Decoder::u16() {
  const auto slice = take(2);
  if (!slice) {
    return slice.error();
  }
  const auto* bytes = slice.value().data();
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1])
                                                               << 8));
}

Result<std::uint32_t> Decoder::u32() {
  const auto slice = take(4);
  if (!slice) {
    return slice.error();
  }
  const auto* bytes = slice.value().data();
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (static_cast<unsigned>(index) * 8U);
  }
  return value;
}

Result<std::uint64_t> Decoder::u64() {
  const auto slice = take(8);
  if (!slice) {
    return slice.error();
  }
  const auto* bytes = slice.value().data();
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (static_cast<unsigned>(index) * 8U);
  }
  return value;
}

Result<std::int64_t> Decoder::i64() {
  const auto bits = u64();
  if (!bits) {
    return bits.error();
  }
  return static_cast<std::int64_t>(bits.value());
}

Result<bool> Decoder::boolean() {
  const auto byte = u8();
  if (!byte) {
    return byte.error();
  }
  if (byte.value() == 0U) {
    return false;
  }
  if (byte.value() == 1U) {
    return true;
  }
  return make_error(ErrorCode::MalformedInput,
                    "boolean field encoded as " + std::to_string(byte.value()) +
                        " which is neither 0 nor 1");
}

Result<std::string> Decoder::text() {
  const auto length = u32();
  if (!length) {
    return length.error();
  }
  const std::uint64_t requested = length.value();
  if (requested > limits_.max_text_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      limit_detail("text", requested, limits_.max_text_bytes));
  }
  const auto slice = take(static_cast<std::size_t>(requested));
  if (!slice) {
    return slice.error();
  }
  const std::string_view view(reinterpret_cast<const char*>(slice.value().data()),
                              slice.value().size());
  if (!is_valid_utf8(view)) {
    return make_error(ErrorCode::MalformedInput, "text field is not well-formed UTF-8");
  }
  return std::string(view);
}

Result<std::vector<std::uint8_t>> Decoder::blob() {
  const auto length = u64();
  if (!length) {
    return length.error();
  }
  const std::uint64_t requested = length.value();
  if (requested > limits_.max_frame_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      limit_detail("blob", requested, limits_.max_frame_bytes));
  }
  const auto slice = take(static_cast<std::size_t>(requested));
  if (!slice) {
    return slice.error();
  }
  return std::vector<std::uint8_t>(slice.value().begin(), slice.value().end());
}

Result<std::span<const std::uint8_t>> Decoder::raw(std::size_t count) { return take(count); }

Result<std::uint32_t> Decoder::collection_count() {
  const auto count = u32();
  if (!count) {
    return count.error();
  }
  if (static_cast<std::uint64_t>(count.value()) > limits_.max_collection_items) {
    return make_error(ErrorCode::BoundsExceeded,
                      limit_detail("collection", count.value(), limits_.max_collection_items));
  }
  return count.value();
}

Result<Ack> Decoder::require_end() const {
  if (!at_end()) {
    std::string detail = "trailing bytes: ";
    detail.append(std::to_string(remaining()));
    detail.append(" byte(s) remain after the object at offset ");
    detail.append(std::to_string(offset_));
    return make_error(ErrorCode::MalformedInput, std::move(detail));
  }
  return Ack{};
}

}  // namespace dcf
