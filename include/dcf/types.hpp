// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace dcf {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------
// Every authoritative object in this boundary is named by a 128-bit identifier.
// A federation identifier and a site identifier share a representation but are
// distinct types, so a value of one kind can never be passed where the other is
// expected.
template <class Tag>
struct Identifier {
  std::uint64_t high{0};
  std::uint64_t low{0};

  friend constexpr bool operator==(const Identifier&, const Identifier&) noexcept = default;
  friend constexpr auto operator<=>(const Identifier&, const Identifier&) noexcept = default;

  [[nodiscard]] constexpr bool is_zero() const noexcept { return high == 0 && low == 0; }
};

struct FederationTag;
struct SiteTag;
struct DelegationTag;
struct OperationTag;
struct ReconciliationTag;

using FederationId = Identifier<FederationTag>;
using SiteId = Identifier<SiteTag>;
using DelegationId = Identifier<DelegationTag>;
using OperationId = Identifier<OperationTag>;
using ReconciliationId = Identifier<ReconciliationTag>;

// The canonical text form of an identifier is 32 lower-case hexadecimal digits
// with the high half first: "0123456789abcdeffedcba9876543210". No separators,
// no prefixes, fixed width, so that text and canonical binary agree.
[[nodiscard]] std::string to_hex(std::uint64_t high, std::uint64_t low);
[[nodiscard]] std::optional<std::uint64_t> parse_hex64(std::string_view text) noexcept;

template <class Tag>
[[nodiscard]] std::string to_string(const Identifier<Tag>& id) {
  return to_hex(id.high, id.low);
}

template <class Tag>
[[nodiscard]] std::optional<Identifier<Tag>> parse_identifier(std::string_view text) noexcept {
  if (text.size() != 32) {
    return std::nullopt;
  }
  const auto high = parse_hex64(text.substr(0, 16));
  const auto low = parse_hex64(text.substr(16, 16));
  if (!high.has_value() || !low.has_value()) {
    return std::nullopt;
  }
  Identifier<Tag> id;
  id.high = *high;
  id.low = *low;
  return id;
}

// ---------------------------------------------------------------------------
// Generations
// ---------------------------------------------------------------------------
// Generations are monotonic counters that never wrap. A counter that has
// reached its maximum does not become a smaller counter: increment() reports
// exhaustion so that a caller cannot silently reuse a generation that has
// already been published.
template <class Tag>
class Counter {
 public:
  using value_type = std::uint64_t;

  constexpr Counter() noexcept = default;
  constexpr explicit Counter(value_type value) noexcept : value_(value) {}

  [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool exhausted() const noexcept {
    return value_ == std::numeric_limits<value_type>::max();
  }

  // Returns the successor, or std::nullopt when the counter is exhausted.
  [[nodiscard]] constexpr std::optional<Counter> increment() const noexcept {
    if (exhausted()) {
      return std::nullopt;
    }
    return Counter(value_ + 1);
  }

  friend constexpr bool operator==(Counter, Counter) noexcept = default;
  friend constexpr auto operator<=>(Counter, Counter) noexcept = default;

 private:
  value_type value_{0};
};

struct FederationGenerationTag;
struct MembershipGenerationTag;
struct AcceptedGenerationTag;
struct DelegationGenerationTag;
struct JournalSequenceTag;
struct SiteLocalEpochTag;

// The federation epoch. It advances once for every committed authoritative
// change to federation membership, delegation, compatibility, or connectivity
// state, and it is the generation that every other authority record is stamped
// against.
using FederationGeneration = Counter<FederationGenerationTag>;
// The generation of one site's own membership record. It advances on every
// accepted membership transition for that site.
using MembershipGeneration = Counter<MembershipGenerationTag>;
// The federation generation a site reports that it has already accepted. Used
// to detect staleness in both directions across a partition.
using AcceptedGeneration = Counter<AcceptedGenerationTag>;
using DelegationGeneration = Counter<DelegationGenerationTag>;
using JournalSequence = Counter<JournalSequenceTag>;
// A site-owned counter. The federation never derives it and never overrides it;
// it exists only so that a site can report that it has local history the
// federation has not seen.
using SiteLocalEpoch = Counter<SiteLocalEpochTag>;

template <class Tag>
[[nodiscard]] constexpr std::int64_t to_i64(const Counter<Tag>& counter) noexcept {
  return static_cast<std::int64_t>(counter.value());
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------
struct Version {
  std::uint32_t major{0};
  std::uint32_t minor{0};

  friend constexpr bool operator==(const Version&, const Version&) noexcept = default;
  friend constexpr auto operator<=>(const Version&, const Version&) noexcept = default;

  [[nodiscard]] std::string to_string() const;
};

// An inclusive version window. Both ends are part of the window. A window with
// "inverted" is empty rather than implicitly reinterpreted.
struct VersionRange {
  Version minimum{};
  Version maximum{};

  [[nodiscard]] constexpr bool is_empty() const noexcept { return maximum < minimum; }
  [[nodiscard]] constexpr bool contains(const Version& candidate) const noexcept {
    return !is_empty() && minimum <= candidate && candidate <= maximum;
  }
  [[nodiscard]] constexpr bool intersects(const VersionRange& other) const noexcept {
    return !is_empty() && !other.is_empty() && minimum <= other.maximum && other.minimum <= maximum;
  }

  friend constexpr bool operator==(const VersionRange&, const VersionRange&) noexcept = default;
  friend constexpr auto operator<=>(const VersionRange&, const VersionRange&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------
// A 256-bit digest over a canonical byte serialization. Two digests that differ
// mean the underlying canonical byte strings differ, which is what makes
// divergence detection exact rather than heuristic.
struct Digest {
  std::array<std::uint8_t, 32> bytes{};

  friend constexpr bool operator==(const Digest&, const Digest&) noexcept = default;
  friend constexpr auto operator<=>(const Digest&, const Digest&) noexcept = default;

  [[nodiscard]] constexpr bool is_zero() const noexcept {
    for (const std::uint8_t byte : bytes) {
      if (byte != 0) {
        return false;
      }
    }
    return true;
  }
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static std::optional<Digest> from_hex(std::string_view text) noexcept;
};

[[nodiscard]] Digest digest_of(std::string_view bytes);

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr bool add_overflows(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  return lhs > std::numeric_limits<std::uint64_t>::max() - rhs;
}

[[nodiscard]] constexpr bool multiply_overflows(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  if (lhs == 0 || rhs == 0) {
    return false;
  }
  return lhs > std::numeric_limits<std::uint64_t>::max() / rhs;
}

// Checked narrowing conversion between integer types. Mixed signedness is
// handled by comparing in the unsigned domain rather than by casting, so a
// negative source value can never be reinterpreted as a large positive one.
template <class To, class From>
[[nodiscard]] constexpr bool fits(From value) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>,
                "fits() is defined for integer types only");
  static_assert(!std::is_same_v<To, bool> && !std::is_same_v<From, bool>,
                "fits() is not defined for bool");
  if constexpr (std::is_signed_v<From>) {
    if (value < 0) {
      return false;
    }
  }
  using UnsignedTo = std::make_unsigned_t<To>;
  using UnsignedFrom = std::make_unsigned_t<From>;
  return std::cmp_less_equal(static_cast<UnsignedFrom>(value),
                             static_cast<UnsignedTo>(std::numeric_limits<To>::max()));
}

template <class To, class From>
[[nodiscard]] constexpr std::optional<To> narrow(From value) noexcept {
  if (!fits<To>(value)) {
    return std::nullopt;
  }
  return static_cast<To>(value);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// Wall-clock milliseconds since the Unix epoch. Wall-clock values are advisory:
// they are recorded as evidence, never used on their own to decide authority.
struct UnixMillis {
  std::int64_t value{0};

  friend constexpr bool operator==(const UnixMillis&, const UnixMillis&) noexcept = default;
  friend constexpr auto operator<=>(const UnixMillis&, const UnixMillis&) noexcept = default;

  [[nodiscard]] constexpr bool is_set() const noexcept { return value != 0; }
};

// The clock is an injected dependency so that expiry behaviour is testable
// without sleeping and without depending on the host clock's resolution.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();

  [[nodiscard]] virtual UnixMillis wall_clock() const noexcept = 0;
};

// A clock whose value only changes when a test changes it.
class ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(UnixMillis start) noexcept : now_(start) {}

  [[nodiscard]] UnixMillis wall_clock() const noexcept override { return now_; }
  void advance(std::int64_t millis) noexcept { now_.value += millis; }
  void set(UnixMillis value) noexcept { now_ = value; }

 private:
  UnixMillis now_{};
};

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
// Each code names a distinct reason a request was not carried out. Codes are
// never collapsed: "the input was malformed" and "the input was well formed but
// named an object that does not exist" are different outcomes with different
// operator responses, so they stay different values.
enum class ErrorCode : std::uint8_t {
  None = 0,
  InvalidArgument,
  MalformedInput,
  BoundsExceeded,
  ArithmeticOverflow,
  NotFound,
  DuplicateIdentity,
  Conflict,
  StaleGeneration,
  Revoked,
  Incompatible,
  InsufficientAuthority,
  PartitionSuspected,
  Unsupported,
  Indeterminate,
  IdempotencyConflict,
  Corruption,
  TornTail,
  InteriorCorruption,
  Io,
  Locked,
  Busy,
  Closed,
  CapacityExhausted,
  ShuttingDown,
  RefusedByPolicy,
  Internal,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

struct Error {
  ErrorCode code{ErrorCode::None};
  std::string detail{};

  friend bool operator==(const Error&, const Error&) = default;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string detail) {
  return Error{code, std::move(detail)};
}

// A value or an error. Construction from T is implicit so that a function
// returning Result<T> can simply return the value, and construction from Error
// is implicit so that a refusal is returned the same way.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] const T* operator->() const { return &std::get<0>(storage_); }
  [[nodiscard]] T* operator->() { return &std::get<0>(storage_); }

  [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
  [[nodiscard]] Error& error() & { return std::get<1>(storage_); }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Error> storage_;
};

// The result of an operation that carries no value.
struct Ack {};

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
// Every externally supplied size is checked against these before anything is
// allocated. They are deliberately generous but finite: a peer cannot ask this
// runtime to allocate an unbounded amount of memory.
struct Limits {
  // Largest single framed message accepted from any peer or read from disk.
  std::uint64_t max_frame_bytes{16ULL * 1024ULL * 1024ULL};
  // Largest text field (names, identifiers passed as text, reason strings).
  std::uint64_t max_text_bytes{4096};
  // Largest collection in a single decoded object.
  std::uint64_t max_collection_items{200000};
  // Largest number of sites a federation will admit.
  std::uint64_t max_sites{100000};
  // Largest number of live delegations.
  std::uint64_t max_delegations{1000000};
  // Largest number of retained reconciliation records.
  std::uint64_t max_reconciliations{1000000};
  // Largest number of durable idempotency receipts retained.
  std::uint64_t max_receipts{1000000};
  // Largest number of capability declarations per site.
  std::uint64_t max_capabilities_per_site{1024};
  // Upper bound on a memory limit for one site's partition-safety behaviour.
  std::uint32_t max_partition_tolerance_days{3650};
};

[[nodiscard]] const Limits& default_limits() noexcept;

}  // namespace dcf
