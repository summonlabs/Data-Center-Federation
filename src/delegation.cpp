// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/delegation.hpp"

#include <algorithm>
#include <array>

#include "dcf/hash.hpp"

namespace dcf {
namespace {

struct ScopeName {
  DelegationScope scope;
  std::string_view name;
};

constexpr std::array<ScopeName, 7> kScopeNames{{
    {DelegationScope::MembershipObserve, "membership.observe"},
    {DelegationScope::MembershipAdmit, "membership.admit"},
    {DelegationScope::CompatibilityApprove, "compatibility.approve"},
    {DelegationScope::ReconciliationWitness, "reconciliation.witness"},
    {DelegationScope::PolicyAttest, "policy.attest"},
    {DelegationScope::ObservabilityRead, "observability.read"},
    {DelegationScope::DrainSchedule, "drain.schedule"},
}};

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

std::string to_string(DelegationScope scope) {
  for (const ScopeName& entry : kScopeNames) {
    if (entry.scope == scope) {
      return std::string(entry.name);
    }
  }
  return "unknown";
}

std::string to_string(std::uint16_t scope_mask) {
  if (scope_mask == 0) {
    return "none";
  }
  std::string out;
  std::uint16_t remaining = scope_mask;
  for (const ScopeName& entry : kScopeNames) {
    if ((remaining & mask_of(entry.scope)) != 0U) {
      if (!out.empty()) {
        out.push_back(',');
      }
      out.append(entry.name);
      remaining = static_cast<std::uint16_t>(remaining & ~mask_of(entry.scope));
    }
  }
  if (remaining != 0U) {
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append("unrecognised(");
    out.append(std::to_string(remaining));
    out.push_back(')');
  }
  return out;
}

std::optional<std::uint16_t> parse_scope_mask(std::string_view text) noexcept {
  std::uint16_t mask = 0;
  std::size_t offset = 0;
  bool any = false;
  while (offset <= text.size()) {
    const std::size_t comma = text.find(',', offset);
    const std::size_t end = (comma == std::string_view::npos) ? text.size() : comma;
    const std::string_view token = trim(text.substr(offset, end - offset));
    if (token.empty()) {
      return std::nullopt;
    }
    bool matched = false;
    for (const ScopeName& entry : kScopeNames) {
      if (entry.name == token) {
        mask = static_cast<std::uint16_t>(mask | mask_of(entry.scope));
        matched = true;
        break;
      }
    }
    if (!matched) {
      return std::nullopt;
    }
    any = true;
    if (comma == std::string_view::npos) {
      break;
    }
    offset = comma + 1;
  }
  if (!any) {
    return std::nullopt;
  }
  return mask;
}

std::string_view to_string(DelegationStatus status) noexcept {
  switch (status) {
    case DelegationStatus::Active:
      return "active";
    case DelegationStatus::NotYetEffective:
      return "not_yet_effective";
    case DelegationStatus::Expired:
      return "expired";
    case DelegationStatus::Revoked:
      return "revoked";
    case DelegationStatus::SuspendedByPartition:
      return "suspended_by_partition";
    case DelegationStatus::SupersededByMembership:
      return "superseded_by_membership";
  }
  return "unknown";
}

DelegationStatus evaluate_delegation(const DelegationRecord& record, FederationGeneration at,
                                     UnixMillis now, MembershipGeneration membership,
                                     bool partitioned) noexcept {
  // Revocation is checked first and is absolute: no later condition can make a
  // revoked grant effective again, and no earlier condition can make a revoked
  // grant look merely suspended.
  if (record.revoked) {
    return DelegationStatus::Revoked;
  }

  // A grant stamped with a membership generation older than the grantee current
  // one belongs to a superseded incarnation of the site. It is not expired and
  // not revoked; it simply never applied to this incarnation.
  if (record.grant.grantee_membership_generation < membership) {
    return DelegationStatus::SupersededByMembership;
  }

  if (at < record.grant.granted_at) {
    return DelegationStatus::NotYetEffective;
  }

  if (!record.grant.expires_at.is_zero() && record.grant.expires_at <= at) {
    return DelegationStatus::Expired;
  }

  if (record.grant.expires_at_wall.is_set() && record.grant.expires_at_wall.value <= now.value) {
    return DelegationStatus::Expired;
  }

  if (partitioned && !record.grant.survives_partition) {
    return DelegationStatus::SuspendedByPartition;
  }

  if (record.grant.scope_mask == 0) {
    // A grant that delegates nothing is not authority, so it is not active.
    return DelegationStatus::Expired;
  }

  return DelegationStatus::Active;
}

std::optional<ExclusiveConflict> find_exclusive_conflict(const std::vector<DelegationRecord>& ordered,
                                                         const DelegationGrant& candidate,
                                                         FederationGeneration at,
                                                         UnixMillis now) {
  if (!candidate.exclusive) {
    return std::nullopt;
  }
  for (const DelegationRecord& existing : ordered) {
    if (!existing.grant.exclusive) {
      continue;
    }
    if (existing.grant.id == candidate.id) {
      continue;
    }
    const std::uint16_t overlap = static_cast<std::uint16_t>(existing.grant.scope_mask &
                                                             candidate.scope_mask);
    if (overlap == 0U) {
      continue;
    }
    if (!selectors_overlap(existing.grant.selectors, candidate.selectors)) {
      continue;
    }
    // The existing grant blocks the candidate when its fate is already decided in
    // favour of the existing grantee, or when its fate is not yet decided at all.
    // A grant that is already expired or revoked does not block.
    const DelegationStatus status =
        evaluate_delegation(existing, at, now, candidate.grantee_membership_generation,
                            /*partitioned=*/false);
    const bool blocked =
        is_indeterminate(status) || status == DelegationStatus::SupersededByMembership;
    if (!blocked) {
      continue;
    }
    ExclusiveConflict conflict;
    conflict.existing = existing.grant.id;
    conflict.existing_grantee = existing.grant.grantee;
    conflict.overlapping_scopes = overlap;
    return conflict;
  }
  return std::nullopt;
}

Result<std::vector<std::string>> canonicalize_selectors(std::vector<std::string> selectors,
                                                        const Limits& limits) {
  if (static_cast<std::uint64_t>(selectors.size()) > limits.max_collection_items) {
    return make_error(ErrorCode::BoundsExceeded,
                      "selector set carries " + std::to_string(selectors.size()) +
                          " entries which exceeds the limit of " +
                          std::to_string(limits.max_collection_items));
  }
  for (const std::string& selector : selectors) {
    if (selector.empty() || selector.size() > limits.max_text_bytes) {
      return make_error(ErrorCode::InvalidArgument,
                        "selector must be 1.." + std::to_string(limits.max_text_bytes) +
                            " bytes and was " + std::to_string(selector.size()));
    }
    if (!is_valid_utf8(selector)) {
      return make_error(ErrorCode::InvalidArgument, "selector is not well-formed UTF-8");
    }
  }
  std::sort(selectors.begin(), selectors.end());
  for (std::size_t index = 1; index < selectors.size(); ++index) {
    if (selectors[index - 1] == selectors[index]) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "selector '" + sanitize_for_terminal(selectors[index]) +
                            "' is listed more than once");
    }
  }
  return selectors;
}

bool selectors_overlap(const std::vector<std::string>& lhs,
                       const std::vector<std::string>& rhs) noexcept {
  if (lhs.empty() || rhs.empty()) {
    return true;
  }
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < lhs.size() && right < rhs.size()) {
    if (lhs[left] == rhs[right]) {
      return true;
    }
    if (lhs[left] < rhs[right]) {
      ++left;
    } else {
      ++right;
    }
  }
  return false;
}

}  // namespace dcf
