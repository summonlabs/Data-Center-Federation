// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "dcf/capability.hpp"
#include "dcf/types.hpp"

namespace dcf {

// ---------------------------------------------------------------------------
// Membership lifecycle
// ---------------------------------------------------------------------------
// A site moves through a small, explicitly enumerated set of states. Refusals
// are states of their own rather than errors on the way to a state, because a
// refused candidate is a fact the federation has to be able to report later:
// "no record of that site" and "that site was refused for this reason" are
// different answers, and collapsing them would lose the reason.
enum class MembershipState : std::uint8_t {
  // The site has asked to join and nothing has been decided.
  Candidate = 0,
  // The compatibility declaration has been evaluated and accepted.
  CompatibilityValidated = 1,
  // The site has been admitted but has not yet confirmed that it accepted the
  // federation generation it was admitted at.
  Admitted = 2,
  // The site has confirmed the generation and takes part normally.
  Active = 3,
  // The site takes part with a narrowed scope: a deprecated capability, a
  // missing optional capability, or an operator-applied restriction.
  Constrained = 4,
  // The site is disconnected. This is a membership state and not merely a link
  // property because being cut off changes what the site may do, and the
  // federation must be able to say so about a site it cannot currently reach.
  Partitioned = 5,
  // The site is being removed in an orderly way.
  Draining = 6,
  // The site is no longer a member. Removal is terminal: a later message from
  // the site is fenced rather than treated as a rejoin.
  Removed = 7,
  // The compatibility declaration was refused.
  RefusedIncompatible = 8,
  // The candidate conflicted with existing authoritative state: a duplicate
  // identity, a replayed admission, or a stale generation.
  RefusedConflict = 9,
  // The request came from something that is not entitled to make it.
  RefusedAuthority = 10,
};

[[nodiscard]] std::string_view to_string(MembershipState state) noexcept;
// True when the transition is allowed by the lifecycle. The table is total: any
// pair not listed is illegal, including every transition out of a terminal state.
[[nodiscard]] bool is_legal_transition(MembershipState from, MembershipState to) noexcept;
[[nodiscard]] bool is_terminal(MembershipState state) noexcept;
// True when the state counts as a member of the federation, whether or not it
// can currently be reached.
[[nodiscard]] bool counts_as_member(MembershipState state) noexcept;
// True when the state means the site is cut off from the federation.
[[nodiscard]] bool is_disconnected(MembershipState state) noexcept;

// ---------------------------------------------------------------------------
// Connectivity
// ---------------------------------------------------------------------------
// Link state is observed from the transport, never inferred from the absence of
// traffic alone: "nothing has arrived" and "the link is down" are different
// facts and the runtime reports Unknown rather than guessing.
enum class LinkState : std::uint8_t {
  Unknown = 0,
  Connected = 1,
  Degraded = 2,
  Partitioned = 3,
};

[[nodiscard]] std::string_view to_string(LinkState state) noexcept;
// True when the link state means the site can be reached right now.
[[nodiscard]] constexpr bool is_reachable(LinkState state) noexcept {
  return state == LinkState::Connected || state == LinkState::Degraded;
}

// ---------------------------------------------------------------------------
// The membership record
// ---------------------------------------------------------------------------
struct MembershipRecord {
  SiteId site{};
  std::string display_name{};
  MembershipState state{MembershipState::Candidate};
  // Advanced on every accepted membership transition for this site, so that a
  // delegation or a message can be stamped against the incarnation of the site
  // that it belongs to.
  MembershipGeneration generation{};
  // The federation generation at which the current state was entered.
  FederationGeneration entered_at{};
  // The state to return to when a partition ends. Recorded explicitly rather
  // than inferred, so that a second partition while returning cannot lose it.
  MembershipState pre_partition_state{MembershipState::Active};

  // What the site last told the federation about itself.
  AcceptedGeneration accepted{};
  SiteLocalEpoch local_epoch{};
  Digest accepted_history{};
  UnixMillis last_contact{};

  CompatibilityDeclaration declaration{};
  Digest declaration_digest{};
  CompatibilityReport admission{};

  // The narrowed scope in force while the site is Constrained. Zero means the
  // narrowing is not scope based.
  std::uint16_t constrained_scopes{0};
  LinkState link{LinkState::Unknown};
  std::string transition_reason{};

  friend bool operator==(const MembershipRecord&, const MembershipRecord&) = default;
};

// True when the federation has positively observed that the site is cut off.
// "Unknown" is not partition: the absence of evidence about a link is not
// evidence that the link is down, and treating it as one would suspend every
// delegation until connectivity happened to be reported.
[[nodiscard]] constexpr bool is_partitioned(const MembershipRecord& record) noexcept {
  return record.link == LinkState::Partitioned || is_disconnected(record.state);
}

}  // namespace dcf
