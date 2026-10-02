// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <string>
#include <vector>

#include "dcf/codec.hpp"
#include "dcf/hash.hpp"
#include "dcf/types.hpp"
#include "test_harness.hpp"

namespace {

using dcf::Ack;
using dcf::Digest;
using dcf::Decoder;
using dcf::Encoder;
using dcf::ErrorCode;
using dcf::FederationId;
using dcf::Limits;
using dcf::Result;
using dcf::SiteId;
using dcf::Version;
using dcf::VersionRange;

[[nodiscard]] Digest digest_from_hex(const std::string& text) {
  const auto parsed = Digest::from_hex(text);
  return parsed.has_value() ? *parsed : Digest{};
}

}  // namespace

DCF_TEST(identity, hex_round_trip) {
  const FederationId id{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
  DCF_CHECK_EQ(dcf::to_string(id), std::string("0123456789abcdeffedcba9876543210"));
  const auto parsed = dcf::parse_identifier<dcf::FederationTag>(dcf::to_string(id));
  DCF_REQUIRE(parsed.has_value());
  DCF_CHECK_EQ(*parsed, id);
}

DCF_TEST(identity, parse_rejects_bad_input) {
  DCF_CHECK(!dcf::parse_identifier<dcf::SiteTag>("").has_value());
  DCF_CHECK(!dcf::parse_identifier<dcf::SiteTag>("0123456789abcdeffedcba987654321").has_value());
  DCF_CHECK(!dcf::parse_identifier<dcf::SiteTag>("0123456789abcdeffedcba98765432100").has_value());
  DCF_CHECK(!dcf::parse_identifier<dcf::SiteTag>("0123456789abcdeffedcba987654321g").has_value());
  DCF_CHECK(dcf::parse_identifier<dcf::SiteTag>("0123456789ABCDEFFEDCBA9876543210").has_value());
}

DCF_TEST(identity, identifiers_of_different_kinds_are_distinct_types) {
  static_assert(!std::is_same_v<SiteId, FederationId>);
  const SiteId site{1, 2};
  DCF_CHECK(!site.is_zero());
  DCF_CHECK(SiteId{}.is_zero());
}

DCF_TEST(counter, increment_and_exhaustion) {
  dcf::FederationGeneration generation;
  DCF_CHECK(generation.is_zero());
  const auto next = generation.increment();
  DCF_REQUIRE(next.has_value());
  DCF_CHECK_EQ(next->value(), 1ULL);

  const dcf::FederationGeneration last{0xffffffffffffffffULL};
  DCF_CHECK(last.exhausted());
  DCF_CHECK(!last.increment().has_value());
}

DCF_TEST(arithmetic, overflow_helpers) {
  DCF_CHECK(!dcf::add_overflows(1, 2));
  DCF_CHECK(dcf::add_overflows(0xffffffffffffffffULL, 1));
  DCF_CHECK(!dcf::multiply_overflows(0, 0xffffffffffffffffULL));
  DCF_CHECK(!dcf::multiply_overflows(2, 3));
  DCF_CHECK(dcf::multiply_overflows(0x100000000ULL, 0x100000000ULL));
}

DCF_TEST(arithmetic, checked_narrowing) {
  DCF_CHECK_EQ(dcf::narrow<std::uint8_t>(200).value(), static_cast<std::uint8_t>(200));
  DCF_CHECK(!dcf::narrow<std::uint8_t>(256).has_value());
  DCF_CHECK(!dcf::narrow<std::uint8_t>(-1).has_value());
  DCF_CHECK(!dcf::narrow<std::uint32_t>(static_cast<std::int64_t>(-5)).has_value());
  DCF_CHECK_EQ(dcf::narrow<std::uint64_t>(std::uint32_t{7}).value(), 7ULL);
  DCF_CHECK(!dcf::narrow<std::int32_t>(std::numeric_limits<std::uint64_t>::max()).has_value());
}

DCF_TEST(version, range_semantics) {
  const VersionRange range{Version{1, 0}, Version{2, 5}};
  DCF_CHECK(range.contains(Version{1, 0}));
  DCF_CHECK(range.contains(Version{2, 5}));
  DCF_CHECK(!range.contains(Version{0, 9}));
  DCF_CHECK(!range.contains(Version{2, 6}));
  const VersionRange empty{Version{3, 0}, Version{2, 0}};
  DCF_CHECK(empty.is_empty());
  DCF_CHECK(!empty.contains(Version{2, 5}));
  DCF_CHECK(!empty.intersects(range));
  DCF_CHECK(range.intersects(VersionRange{Version{2, 5}, Version{9, 9}}));
  DCF_CHECK(!range.intersects(VersionRange{Version{3, 0}, Version{9, 9}}));
}

DCF_TEST(hash, sha256_known_vectors) {
  DCF_CHECK_EQ(dcf::digest_of("").to_hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  DCF_CHECK_EQ(dcf::digest_of("abc").to_hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  DCF_CHECK_EQ(
      digest_from_hex(dcf::digest_of(
                          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
                          .to_hex())
          .to_hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

DCF_TEST(hash, sha256_incremental_matches_oneshot) {
  std::string payload;
  for (int index = 0; index < 5000; ++index) {
    payload.push_back(static_cast<char>('a' + (index % 26)));
  }
  const Digest oneshot = dcf::digest_of(payload);

  for (std::size_t chunk = 1; chunk <= 130; chunk += 7) {
    dcf::Sha256 hasher;
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const std::size_t length = std::min(chunk, payload.size() - offset);
      hasher.update(std::string_view(payload).substr(offset, length));
      offset += length;
    }
    DCF_CHECK_EQ(hasher.finalize(), oneshot);
  }
}

DCF_TEST(hash, sha256_streaming_boundaries) {
  // 55, 56, 63, 64 and 65 bytes are the padding-length boundaries.
  for (const std::size_t size : {std::size_t{0}, std::size_t{55}, std::size_t{56}, std::size_t{63},
                                 std::size_t{64}, std::size_t{65}, std::size_t{119},
                                 std::size_t{120}, std::size_t{128}}) {
    const std::string payload(size, 'x');
    dcf::Sha256 hasher;
    hasher.update(payload);
    const Digest digest = hasher.finalize();
    DCF_CHECK(!digest.is_zero());
    DCF_CHECK_EQ(digest, dcf::digest_of(payload));
  }
}

DCF_TEST(hash, crc32c_known_vector) {
  DCF_CHECK_EQ(dcf::crc32c("123456789"), 0xe3069283U);
  DCF_CHECK_EQ(dcf::crc32c(""), 0U);
  const std::string prefix = "1234";
  const std::string suffix = "56789";
  const std::uint32_t seed =
      dcf::crc32c_extend(0xffffffffU,
                         std::span<const std::uint8_t>(
                             reinterpret_cast<const std::uint8_t*>(prefix.data()), prefix.size()));
  DCF_CHECK_EQ(dcf::crc32c_extend(seed, std::span<const std::uint8_t>(
                                            reinterpret_cast<const std::uint8_t*>(suffix.data()),
                                            suffix.size())) ^
                   0xffffffffU,
               dcf::crc32c("123456789"));
}

DCF_TEST(hash, utf8_validation) {
  DCF_CHECK(dcf::is_valid_utf8(""));
  DCF_CHECK(dcf::is_valid_utf8("plain ascii"));
  DCF_CHECK(dcf::is_valid_utf8("caf\xc3\xa9"));
  DCF_CHECK(dcf::is_valid_utf8("\xe6\x97\xa5\xe6\x9c\xac"));
  DCF_CHECK(dcf::is_valid_utf8("\xf0\x9f\x9a\x80"));
  DCF_CHECK(!dcf::is_valid_utf8("\x80"));
  DCF_CHECK(!dcf::is_valid_utf8("\xc3"));
  DCF_CHECK(!dcf::is_valid_utf8("\xc0\x80"));
  DCF_CHECK(!dcf::is_valid_utf8("\xc1\xbf"));
  DCF_CHECK(!dcf::is_valid_utf8("\xe0\x80\x80"));
  DCF_CHECK(!dcf::is_valid_utf8("\xed\xa0\x80"));
  DCF_CHECK(!dcf::is_valid_utf8("\xf5\x80\x80\x80"));
  DCF_CHECK(!dcf::is_valid_utf8("\xf0\x82\x82\xac"));
  DCF_CHECK(!dcf::is_valid_utf8("\xff"));
  DCF_CHECK(!dcf::is_valid_utf8("a\xc3"));
}

DCF_TEST(codec, round_trip_scalars) {
  Encoder encoder;
  encoder.u8(0xabU);
  encoder.u16(0xbeefU);
  encoder.u32(0xdeadbeefU);
  encoder.u64(0x0123456789abcdefULL);
  encoder.i64(-1234567890123LL);
  encoder.boolean(true);
  encoder.boolean(false);
  encoder.text("hello \xe6\x97\xa5\xe6\x9c\xac");
  encoder.blob(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>("\x00\x01\x02"), 3));

  Decoder decoder(encoder.view());
  DCF_CHECK_EQ(decoder.u8().value(), static_cast<std::uint8_t>(0xab));
  DCF_CHECK_EQ(decoder.u16().value(), static_cast<std::uint16_t>(0xbeef));
  DCF_CHECK_EQ(decoder.u32().value(), 0xdeadbeefU);
  DCF_CHECK_EQ(decoder.u64().value(), 0x0123456789abcdefULL);
  DCF_CHECK_EQ(decoder.i64().value(), -1234567890123LL);
  DCF_CHECK_EQ(decoder.boolean().value(), true);
  DCF_CHECK_EQ(decoder.boolean().value(), false);
  DCF_CHECK_EQ(decoder.text().value(), std::string("hello \xe6\x97\xa5\xe6\x9c\xac"));
  const auto blob = decoder.blob();
  DCF_REQUIRE(blob.has_value());
  DCF_CHECK_EQ(blob.value().size(), std::size_t{3});
  DCF_CHECK(decoder.require_end().has_value());
}

DCF_TEST(codec, canonical_encoding_is_stable) {
  Encoder first;
  first.u32(1);
  first.text("a");
  Encoder second;
  second.u32(1);
  second.text("a");
  DCF_CHECK_EQ(first.bytes(), second.bytes());
  DCF_CHECK_EQ(dcf::digest_of(dcf::to_string(first.bytes())),
               dcf::digest_of(dcf::to_string(second.bytes())));
}

DCF_TEST(codec, rejects_truncated_input) {
  Encoder encoder;
  encoder.u64(0x1122334455667788ULL);
  Decoder decoder(std::span<const std::uint8_t>(encoder.view().data(), 5));
  const auto value = decoder.u64();
  DCF_REQUIRE(!value.has_value());
  DCF_CHECK_EQ(value.error().code, ErrorCode::MalformedInput);
}

DCF_TEST(codec, rejects_trailing_bytes) {
  Encoder encoder;
  encoder.u32(1);
  encoder.u8(0);
  Decoder decoder(encoder.view());
  DCF_CHECK(decoder.u32().has_value());
  const auto end = decoder.require_end();
  DCF_REQUIRE(!end.has_value());
  DCF_CHECK_EQ(end.error().code, ErrorCode::MalformedInput);
}

DCF_TEST(codec, rejects_non_canonical_boolean) {
  Encoder encoder;
  encoder.u8(2);
  Decoder decoder(encoder.view());
  const auto value = decoder.boolean();
  DCF_REQUIRE(!value.has_value());
  DCF_CHECK_EQ(value.error().code, ErrorCode::MalformedInput);
}

DCF_TEST(codec, rejects_invalid_utf8_text) {
  Encoder encoder;
  encoder.u32(2);
  encoder.raw(std::string_view("\xc3\x28", 2));
  Decoder decoder(encoder.view());
  const auto value = decoder.text();
  DCF_REQUIRE(!value.has_value());
  DCF_CHECK_EQ(value.error().code, ErrorCode::MalformedInput);
}

DCF_TEST(codec, rejects_absurd_lengths_before_allocation) {
  Encoder encoder;
  encoder.u32(0xffffffffU);
  const Limits limits;
  Decoder decoder(encoder.view(), limits);
  const auto value = decoder.text();
  DCF_REQUIRE(!value.has_value());
  DCF_CHECK_EQ(value.error().code, ErrorCode::BoundsExceeded);

  Encoder blob_encoder;
  blob_encoder.u64(0xffffffffffffffffULL);
  Decoder blob_decoder(blob_encoder.view(), limits);
  const auto blob = blob_decoder.blob();
  DCF_REQUIRE(!blob.has_value());
  DCF_CHECK_EQ(blob.error().code, ErrorCode::BoundsExceeded);

  Encoder count_encoder;
  count_encoder.u32(0xffffffffU);
  Decoder count_decoder(count_encoder.view(), limits);
  const auto count = count_decoder.collection_count();
  DCF_REQUIRE(!count.has_value());
  DCF_CHECK_EQ(count.error().code, ErrorCode::BoundsExceeded);
}

DCF_TEST(codec, empty_values_round_trip) {
  Encoder encoder;
  encoder.text("");
  encoder.blob({});
  Decoder decoder(encoder.view());
  DCF_CHECK_EQ(decoder.text().value(), std::string{});
  const auto blob = decoder.blob();
  DCF_REQUIRE(blob.has_value());
  DCF_CHECK(blob.value().empty());
  DCF_CHECK(decoder.require_end().has_value());
}

DCF_TEST(codec, digest_hex_round_trip) {
  const std::string hex(64, 'a');
  const auto parsed = Digest::from_hex(hex);
  DCF_REQUIRE(parsed.has_value());
  DCF_CHECK_EQ(parsed->to_hex(), hex);
  DCF_CHECK(!Digest::from_hex("xyz").has_value());
  DCF_CHECK(!Digest::from_hex(std::string(63, 'a')).has_value());
  DCF_CHECK(Digest{}.is_zero());
}
