// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/membership.hpp"

namespace dcf {

std::string_view to_string(MembershipState state) noexcept {
  switch (state) {
    case MembershipState::Candidate:
      return "candidate";
    case MembershipState::CompatibilityValidated:
      return "compatibility_validated";
    case MembershipState::Admitted:
      return "admitted";
    case MembershipState::Active:
      return "active";
    case MembershipState::Constrained:
      return "constrained";
    case MembershipState::Partitioned:
      return "partitioned";
    case MembershipState::Draining:
      return "draining";
    case MembershipState::Removed:
      return "removed";
    case MembershipState::RefusedIncompatible:
      return "refused_incompatible";
    case MembershipState::RefusedConflict:
      return "refused_conflict";
    case MembershipState::RefusedAuthority:
      return "refused_authority";
  }
  return "unknown";
}

bool is_legal_transition(MembershipState from, MembershipState to) noexcept {
  switch (from) {
    case MembershipState::Candidate:
      switch (to) {
        case MembershipState::CompatibilityValidated:
        case MembershipState::RefusedIncompatible:
        case MembershipState::RefusedConflict:
        case MembershipState::RefusedAuthority:
        case MembershipState::Removed:
          return true;
        default:
          return false;
      }
    case MembershipState::CompatibilityValidated:
      switch (to) {
        case MembershipState::Admitted:
        case MembershipState::RefusedIncompatible:
        case MembershipState::RefusedConflict:
        case MembershipState::RefusedAuthority:
          return true;
        default:
          return false;
      }
    case MembershipState::Admitted:
      switch (to) {
        case MembershipState::Active:
        case MembershipState::Draining:
        case MembershipState::Partitioned:
        case MembershipState::Removed:
          return true;
        default:
          return false;
      }
    case MembershipState::Active:
    case MembershipState::Constrained:
      switch (to) {
        case MembershipState::Active:
        case MembershipState::Constrained:
        case MembershipState::Partitioned:
        case MembershipState::Draining:
        case MembershipState::Removed:
          return true;
        default:
          return false;
      }
    case MembershipState::Partitioned:
      switch (to) {
        case MembershipState::Active:
        case MembershipState::Constrained:
        case MembershipState::Draining:
        case MembershipState::Removed:
        case MembershipState::Admitted:
          return true;
        default:
          return false;
      }
    case MembershipState::Draining:
      switch (to) {
        case MembershipState::Active:
        case MembershipState::Partitioned:
        case MembershipState::Removed:
          return true;
        default:
          return false;
      }
    case MembershipState::Removed:
    case MembershipState::RefusedIncompatible:
    case MembershipState::RefusedConflict:
    case MembershipState::RefusedAuthority:
      return false;
  }
  return false;
}

bool is_terminal(MembershipState state) noexcept {
  switch (state) {
    case MembershipState::Removed:
    case MembershipState::RefusedIncompatible:
    case MembershipState::RefusedConflict:
    case MembershipState::RefusedAuthority:
      return true;
    default:
      return false;
  }
}

bool counts_as_member(MembershipState state) noexcept {
  switch (state) {
    case MembershipState::Admitted:
    case MembershipState::Active:
    case MembershipState::Constrained:
    case MembershipState::Partitioned:
    case MembershipState::Draining:
      return true;
    default:
      return false;
  }
}

bool is_disconnected(MembershipState state) noexcept { return state == MembershipState::Partitioned; }

std::string_view to_string(LinkState state) noexcept {
  switch (state) {
    case LinkState::Unknown:
      return "unknown";
    case LinkState::Connected:
      return "connected";
    case LinkState::Degraded:
      return "degraded";
    case LinkState::Partitioned:
      return "partitioned";
  }
  return "unknown";
}

}  // namespace dcf
