// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dcf/types.hpp"

namespace dcf {

// ---------------------------------------------------------------------------
// Delegation scope
// ---------------------------------------------------------------------------
// A delegation is the only way a site holds authority inside the federation, and
// the scope mask is the complete list of what can be delegated. There is
// deliberately no scope for placement, capacity brokerage, reservations, or
// disaster-recovery sequencing: those belong to other boundaries, and a scope
// that existed here would be an invitation to absorb them.
enum class DelegationScope : std::uint16_t {
  None = 0,
  // Observe federation membership records.
  MembershipObserve = 1U << 0,
  // Admit a candidate site into the federation.
  MembershipAdmit = 1U << 1,
  // Approve or refuse a compatibility declaration.
  CompatibilityApprove = 1U << 2,
  // Witness a reconciliation record after a partition.
  ReconciliationWitness = 1U << 3,
  // Attest that a policy compatibility reference is satisfied locally.
  PolicyAttest = 1U << 4,
  // Read aggregated observability state for the federation.
  ObservabilityRead = 1U << 5,
  // Schedule another site for draining.
  DrainSchedule = 1U << 6,
};

[[nodiscard]] constexpr std::uint16_t mask_of(DelegationScope scope) noexcept {
  return static_cast<std::uint16_t>(scope);
}

[[nodiscard]] constexpr std::uint16_t all_delegation_scopes() noexcept {
  return mask_of(DelegationScope::MembershipObserve) |
         mask_of(DelegationScope::MembershipAdmit) |
         mask_of(DelegationScope::CompatibilityApprove) |
         mask_of(DelegationScope::ReconciliationWitness) |
         mask_of(DelegationScope::PolicyAttest) |
         mask_of(DelegationScope::ObservabilityRead) |
         mask_of(DelegationScope::DrainSchedule);
}

[[nodiscard]] std::string to_string(DelegationScope scope);
[[nodiscard]] std::string to_string(std::uint16_t scope_mask);
// Parses a comma-separated list of scope names. Returns nullopt when any name is
// unknown: the federation never ignores a scope it does not understand, because
// silently narrowing a grant would grant something the caller did not ask for.
[[nodiscard]] std::optional<std::uint16_t> parse_scope_mask(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Grants and revocations
// ---------------------------------------------------------------------------
struct DelegationGrant {
  DelegationId id{};
  SiteId grantee{};
  std::uint16_t scope_mask{0};
  // Opaque, typed references to the objects the grant applies to. The federation
  // compares and stores them; it does not dereference them.
  std::vector<std::string> selectors{};
  // The federation generation at which the grant became authoritative.
  FederationGeneration granted_at{};
  // The grantee membership generation at the moment of the grant. A grant stamped
  // with an older membership generation belongs to a superseded incarnation of the
  // site and is never resurrected.
  MembershipGeneration grantee_membership_generation{};
  // The grant is no longer effective at or after this generation. A zero value
  // means the grant has no generation expiry.
  FederationGeneration expires_at{};
  // An advisory wall-clock expiry. A zero value means no wall-clock expiry.
  // Wall-clock time can end authority; it can never make authority effective.
  UnixMillis expires_at_wall{};
  // The explicit survivability rule for a partition. A grant that does not survive
  // a partition is suspended while the grantee is partitioned, so an isolated site
  // cannot exercise delegated authority that was granted before the link was lost.
  bool survives_partition{false};
  // Exclusive grants may not be double-delegated. When an exclusive grant is
  // outstanding and its grantee fate across a partition is unknown, a second
  // exclusive grant over the same scope and selectors is refused.
  bool exclusive{false};
  // A typed reference to the policy compatibility record the grant was issued
  // under. Stored and compared, never interpreted.
  Digest policy_reference{};

  friend bool operator==(const DelegationGrant&, const DelegationGrant&) = default;
};

struct DelegationRevocation {
  DelegationId id{};
  FederationGeneration revoked_at{};
  std::string reason{};

  friend bool operator==(const DelegationRevocation&, const DelegationRevocation&) = default;
};

// A grant together with its revocation, if any. This is what durable state holds:
// revocations are never removed, so a replayed grant can always be fenced by the
// revocation that superseded it.
struct DelegationRecord {
  DelegationGrant grant{};
  bool revoked{false};
  FederationGeneration revoked_at{};
  std::string revocation_reason{};

  friend bool operator==(const DelegationRecord&, const DelegationRecord&) = default;
};

enum class DelegationStatus : std::uint8_t {
  // The grant is in force at the generation being evaluated.
  Active = 0,
  // The grant exists but its effective-from generation has not been reached.
  NotYetEffective = 1,
  // The grant reached its generation or wall-clock expiry.
  Expired = 2,
  // The grant was revoked. Revocation is irrevocable.
  Revoked = 3,
  // The grant does not survive a partition and its grantee is partitioned.
  SuspendedByPartition = 4,
  // The grant belongs to a superseded membership generation of the grantee.
  SupersededByMembership = 5,
};

[[nodiscard]] std::string_view to_string(DelegationStatus status) noexcept;

// Evaluates one record. "at" is the federation generation of the evaluation,
// "now" is the wall clock, "membership" is the grantee current membership
// generation, and "partitioned" says whether the grantee is currently cut off.
[[nodiscard]] DelegationStatus evaluate_delegation(const DelegationRecord& record,
                                                   FederationGeneration at, UnixMillis now,
                                                   MembershipGeneration membership,
                                                   bool partitioned) noexcept;

// True when the grantee may currently exercise the grant.
[[nodiscard]] constexpr bool confers_authority(DelegationStatus status) noexcept {
  return status == DelegationStatus::Active;
}

// True when the fate of the grant is not yet decided, so a conflicting exclusive
// grant must not be issued over the same scope.
[[nodiscard]] constexpr bool is_indeterminate(DelegationStatus status) noexcept {
  return status == DelegationStatus::Active || status == DelegationStatus::NotYetEffective ||
         status == DelegationStatus::SuspendedByPartition;
}

// ---------------------------------------------------------------------------
// Exclusive-conflict detection
// ---------------------------------------------------------------------------
struct ExclusiveConflict {
  DelegationId existing{};
  SiteId existing_grantee{};
  std::uint16_t overlapping_scopes{0};
};

// Returns the first existing exclusive grant that would be double-delegated by the
// candidate, in canonical delegation-identifier order so that the answer does not
// depend on container iteration order.
[[nodiscard]] std::optional<ExclusiveConflict> find_exclusive_conflict(
    const std::vector<DelegationRecord>& ordered, const DelegationGrant& candidate,
    FederationGeneration at, UnixMillis now);

// Selectors are compared bytewise after canonicalisation: sorted and unique.
[[nodiscard]] Result<std::vector<std::string>> canonicalize_selectors(
    std::vector<std::string> selectors, const Limits& limits);

// True when two selector sets share a member, or when either set is empty. An
// empty selector set means every object in scope, so it overlaps everything.
[[nodiscard]] bool selectors_overlap(const std::vector<std::string>& lhs,
                                     const std::vector<std::string>& rhs) noexcept;

}  // namespace dcf
