// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "dcf/capability.hpp"
#include "dcf/delegation.hpp"
#include "dcf/membership.hpp"
#include "dcf/types.hpp"

namespace dcf {

// ---------------------------------------------------------------------------
// What a reconnecting site reports
// ---------------------------------------------------------------------------
// The report is the site's own account of what it believes, expressed only in
// terms the federation can check: a generation it accepted, a digest of the
// history it accepted, its own local epoch, and the delegation identifiers it
// still considers itself to hold. The federation never asks the site what the
// federation's state is, and never lets the site's answer define it.
struct SiteReport {
  SiteId site{};
  AcceptedGeneration accepted{};
  SiteLocalEpoch local_epoch{};
  // The digest of the federation state the site last accepted. Zero means the
  // site has no accepted history, which is only legitimate for a site that has
  // never been active.
  Digest accepted_history{};
  // The digest of the site's own local authority records. The federation stores
  // it and compares it across reconnects; it does not interpret it.
  Digest local_history{};
  MembershipGeneration membership_generation{};
  // Delegations the site still believes it holds.
  std::vector<DelegationId> held_delegations{};
  // Federation generations the site has observed. Used to distinguish "the site
  // is behind" from "the site saw a generation this federation never issued".
  std::vector<FederationGeneration> observed_generations{};

  friend bool operator==(const SiteReport&, const SiteReport&) = default;
};

// Validates a report received from a peer: counts, ordering, duplicates, and
// text are all checked before anything derived from the report is used.
[[nodiscard]] Result<SiteReport> validate_site_report(SiteReport report, const Limits& limits);

// ---------------------------------------------------------------------------
// Divergences
// ---------------------------------------------------------------------------
enum class DivergenceKind : std::uint8_t {
  // The federation removed the member and the site still considers itself a
  // member.
  MembershipRemovedVersusActive = 0,
  // The federation revoked a delegation the site still holds.
  DelegationRevokedVersusHeld = 1,
  // The federation granted a delegation the site has never seen.
  DelegationGrantedVersusUnseen = 2,
  // The site holds a delegation the federation has no record of at all.
  DelegationUnknownToFederation = 3,
  // The site reported a membership generation ahead of the one the federation
  // issued. That generation was never issued by this federation.
  MembershipGenerationAhead = 4,
  // The site accepted a federation generation this federation never issued.
  AcceptedGenerationAhead = 5,
  // The site accepted the current generation but reports a different history
  // digest for it, so two different states are being called the same generation.
  HistoryDigestMismatch = 6,
  // The site carries local history the federation has not seen.
  SiteLocalEpochAhead = 7,
  // An exclusive grant is held on both sides of the partition by different
  // grantees.
  ExclusiveGrantDoubleHeld = 8,
};

[[nodiscard]] std::string_view to_string(DivergenceKind kind) noexcept;

struct DivergenceRecord {
  DivergenceKind kind{DivergenceKind::HistoryDigestMismatch};
  DelegationId delegation{};
  SiteId subject{};
  FederationGeneration federation_generation{};
  MembershipGeneration membership_generation{};
  std::string detail{};

  friend bool operator==(const DivergenceRecord&, const DivergenceRecord&) = default;
  friend auto operator<=>(const DivergenceRecord&, const DivergenceRecord&) = default;
};

// ---------------------------------------------------------------------------
// Reconciliation outcomes
// ---------------------------------------------------------------------------
enum class ReconciliationOutcome : std::uint8_t {
  // The site is exactly at the federation generation and agrees about history.
  InSync = 0,
  // The site is behind and nothing diverges: it adopts the federation
  // generation and continues.
  SiteAdoptsFederation = 1,
  // The site is behind, but it is not entitled to continue: it has been removed
  // or its delegations have been revoked. It adopts the generation and stays
  // fenced.
  SiteAdoptsAndRemainsFenced = 2,
  // The site claims a generation this federation never issued. That means the
  // durable federation state is older than something that was once published.
  // The federation does not demote the site and does not adopt the claim; it
  // reports Indeterminate and refuses to reconcile automatically.
  FederationRegression = 3,
  // The two histories conflict and the conflict has a deterministic answer
  // because revocation is monotonic: revoked wins, removed wins.
  RevocationWins = 4,
  // The two histories conflict and no deterministic answer exists. The record
  // stays open until an operator resolves it explicitly.
  ConflictUnresolved = 5,
  // The inputs were not sufficient to decide anything.
  Indeterminate = 6,
};

[[nodiscard]] std::string_view to_string(ReconciliationOutcome outcome) noexcept;

struct ReconciliationEvaluation {
  ReconciliationOutcome outcome{ReconciliationOutcome::Indeterminate};
  std::vector<DivergenceRecord> divergences{};
  // The generation the site must adopt. Zero when the outcome does not ask the
  // site to adopt anything.
  AcceptedGeneration adopt{};
  // True when the site remains fenced after adopting.
  bool fenced{false};

  friend bool operator==(const ReconciliationEvaluation&,
                         const ReconciliationEvaluation&) = default;
};

struct ReconciliationInputs {
  FederationGeneration federation_generation{};
  MembershipRecord site{};
  // Every delegation record whose grantee is this site, ordered by identifier.
  std::vector<DelegationRecord> delegations{};
  // The canonical digest of the federation's authoritative record for this site
  // at the current generation.
  Digest federation_history{};
  SiteReport report{};
  // True when some other site holds an exclusive grant whose scope overlaps a
  // grant this site believes it holds.
  bool exclusive_double_held{false};
  UnixMillis now{};
};

// Decides what a reconnect means. The function is pure and total: it produces a
// deterministic outcome and an ordered list of divergences for any inputs, and
// it never resolves a genuine conflict by preferring whichever side answered
// last.
[[nodiscard]] ReconciliationEvaluation evaluate_reconciliation(
    const ReconciliationInputs& inputs, const Limits& limits);

// ---------------------------------------------------------------------------
// Reconciliation records
// ---------------------------------------------------------------------------
struct ReconciliationRecord {
  ReconciliationId id{};
  SiteId site{};
  FederationGeneration federation_generation{};
  AcceptedGeneration site_reported_generation{};
  Digest federation_history{};
  Digest site_history{};
  ReconciliationOutcome outcome{ReconciliationOutcome::Indeterminate};
  std::vector<DivergenceRecord> divergences{};
  // True once an operator has resolved an open conflict. Until then the
  // federation keeps refusing to act on the affected authority.
  bool resolved{false};
  std::string resolution{};
  JournalSequence committed{};
  UnixMillis observed_at{};

  friend bool operator==(const ReconciliationRecord&, const ReconciliationRecord&) = default;
};

// An explicit operator decision about an open conflict. Naming the record and
// the choice is required: an unresolved conflict is never resolved by a later
// message that merely happens to arrive.
struct ConflictResolution {
  ReconciliationId reconciliation{};
  enum class Choice : std::uint8_t {
    // The federation's record for the site is authoritative and the site's
    // divergent claims are discarded.
    FederationAuthoritative = 0,
    // The site's local history is authoritative for its own local scope, and the
    // federation records that the divergence was accepted knowingly.
    LocalScopeAuthoritative = 1,
    // The exclusive grant is reissued to exactly one holder; the other holder's
    // claim is revoked.
    ReissueExclusiveToSingleHolder = 2,
  };
  Choice choice{Choice::FederationAuthoritative};
  // The authority under which the resolution is made. Recorded verbatim as
  // evidence; the federation does not interpret it.
  std::string authority_reference{};

  friend bool operator==(const ConflictResolution&, const ConflictResolution&) = default;
};

[[nodiscard]] std::string_view to_string(ConflictResolution::Choice choice) noexcept;

}  // namespace dcf
