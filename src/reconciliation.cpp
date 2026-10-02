// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/reconciliation.hpp"

#include <algorithm>

namespace dcf {
namespace {

void add(std::vector<DivergenceRecord>& divergences, DivergenceKind kind, DelegationId delegation,
         SiteId subject, FederationGeneration federation_generation,
         MembershipGeneration membership_generation, std::string detail) {
  DivergenceRecord record;
  record.kind = kind;
  record.delegation = delegation;
  record.subject = subject;
  record.federation_generation = federation_generation;
  record.membership_generation = membership_generation;
  record.detail = std::move(detail);
  divergences.push_back(std::move(record));
}

void finish(std::vector<DivergenceRecord>& divergences) {
  std::sort(divergences.begin(), divergences.end());
}

[[nodiscard]] bool is_refused_state(MembershipState state) noexcept {
  return state == MembershipState::Removed || state == MembershipState::RefusedIncompatible ||
         state == MembershipState::RefusedConflict || state == MembershipState::RefusedAuthority;
}

}  // namespace

std::string_view to_string(DivergenceKind kind) noexcept {
  switch (kind) {
    case DivergenceKind::MembershipRemovedVersusActive:
      return "membership_removed_versus_active";
    case DivergenceKind::DelegationRevokedVersusHeld:
      return "delegation_revoked_versus_held";
    case DivergenceKind::DelegationGrantedVersusUnseen:
      return "delegation_granted_versus_unseen";
    case DivergenceKind::DelegationUnknownToFederation:
      return "delegation_unknown_to_federation";
    case DivergenceKind::MembershipGenerationAhead:
      return "membership_generation_ahead";
    case DivergenceKind::AcceptedGenerationAhead:
      return "accepted_generation_ahead";
    case DivergenceKind::HistoryDigestMismatch:
      return "history_digest_mismatch";
    case DivergenceKind::SiteLocalEpochAhead:
      return "site_local_epoch_ahead";
    case DivergenceKind::ExclusiveGrantDoubleHeld:
      return "exclusive_grant_double_held";
  }
  return "unknown";
}

std::string_view to_string(ReconciliationOutcome outcome) noexcept {
  switch (outcome) {
    case ReconciliationOutcome::InSync:
      return "in_sync";
    case ReconciliationOutcome::SiteAdoptsFederation:
      return "site_adopts_federation";
    case ReconciliationOutcome::SiteAdoptsAndRemainsFenced:
      return "site_adopts_and_remains_fenced";
    case ReconciliationOutcome::FederationRegression:
      return "federation_regression";
    case ReconciliationOutcome::RevocationWins:
      return "revocation_wins";
    case ReconciliationOutcome::ConflictUnresolved:
      return "conflict_unresolved";
    case ReconciliationOutcome::Indeterminate:
      return "indeterminate";
  }
  return "unknown";
}

std::string_view to_string(ConflictResolution::Choice choice) noexcept {
  switch (choice) {
    case ConflictResolution::Choice::FederationAuthoritative:
      return "federation_authoritative";
    case ConflictResolution::Choice::LocalScopeAuthoritative:
      return "local_scope_authoritative";
    case ConflictResolution::Choice::ReissueExclusiveToSingleHolder:
      return "reissue_exclusive_to_single_holder";
  }
  return "unknown";
}

Result<SiteReport> validate_site_report(SiteReport report, const Limits& limits) {
  if (report.site.is_zero()) {
    return make_error(ErrorCode::InvalidArgument, "site report carries a zero site identifier");
  }
  if (static_cast<std::uint64_t>(report.held_delegations.size()) > limits.max_delegations) {
    return make_error(ErrorCode::BoundsExceeded,
                      "site report claims " + std::to_string(report.held_delegations.size()) +
                          " delegations which exceeds the limit of " +
                          std::to_string(limits.max_delegations));
  }
  if (static_cast<std::uint64_t>(report.observed_generations.size()) > limits.max_collection_items) {
    return make_error(ErrorCode::BoundsExceeded,
                      "site report carries " + std::to_string(report.observed_generations.size()) +
                          " observed generations which exceeds the limit of " +
                          std::to_string(limits.max_collection_items));
  }

  std::sort(report.held_delegations.begin(), report.held_delegations.end());
  for (std::size_t index = 1; index < report.held_delegations.size(); ++index) {
    if (report.held_delegations[index - 1] == report.held_delegations[index]) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "site report lists delegation " + to_string(report.held_delegations[index]) +
                            " more than once");
    }
  }

  std::sort(report.observed_generations.begin(), report.observed_generations.end());
  report.observed_generations.erase(
      std::unique(report.observed_generations.begin(), report.observed_generations.end()),
      report.observed_generations.end());

  return report;
}

ReconciliationEvaluation evaluate_reconciliation(const ReconciliationInputs& inputs,
                                                 const Limits& limits) {
  ReconciliationEvaluation evaluation;
  std::vector<DivergenceRecord>& divergences = evaluation.divergences;

  const FederationGeneration federation_generation = inputs.federation_generation;
  const AcceptedGeneration reported = inputs.report.accepted;
  const MembershipGeneration reported_membership = inputs.report.membership_generation;

  // A site that claims a generation beyond the one this federation has issued is
  // describing a state this federation cannot produce. Rolling the site back to
  // local truth would destroy authority that was legitimately published, so the
  // federation refuses to decide and says so.
  if (federation_generation.value() < reported.value()) {
    add(divergences, DivergenceKind::AcceptedGenerationAhead, DelegationId{}, inputs.report.site,
        federation_generation, reported_membership,
        "site reports it accepted federation generation " +
            std::to_string(reported.value()) + " and this federation is at generation " +
            std::to_string(federation_generation.value()));
    finish(divergences);
    evaluation.outcome = ReconciliationOutcome::FederationRegression;
    return evaluation;
  }

  if (inputs.site.generation.value() < reported_membership.value()) {
    add(divergences, DivergenceKind::MembershipGenerationAhead, DelegationId{},
        inputs.report.site, federation_generation, reported_membership,
        "site reports membership generation " + std::to_string(reported_membership.value()) +
            " and this federation holds generation " +
            std::to_string(inputs.site.generation.value()));
    finish(divergences);
    evaluation.outcome = ReconciliationOutcome::Indeterminate;
    return evaluation;
  }

  // Removal and refusal are terminal. A message from a removed member is fenced:
  // the site is told to adopt the current generation and told that it remains
  // outside. It is never quietly reinstated by a rejoin.
  if (is_refused_state(inputs.site.state)) {
    add(divergences, DivergenceKind::MembershipRemovedVersusActive, DelegationId{},
        inputs.report.site, federation_generation, reported_membership,
        std::string("the federation records this site as ") +
            std::string(to_string(inputs.site.state)) + " and the site is attempting to rejoin");
    finish(divergences);
    evaluation.outcome = ReconciliationOutcome::SiteAdoptsAndRemainsFenced;
    evaluation.adopt = AcceptedGeneration{federation_generation.value()};
    evaluation.fenced = true;
    return evaluation;
  }

  bool authority_lost = false;

  // Every delegation the site still believes it holds is checked against the
  // durable record. Revocation is monotonic, so a revoked grant is never
  // restored, and a grant the federation has no record of is never invented.
  std::size_t cursor = 0;
  for (const DelegationId& held : inputs.report.held_delegations) {
    while (cursor < inputs.delegations.size() && inputs.delegations[cursor].grant.id < held) {
      ++cursor;
    }
    if (cursor == inputs.delegations.size() || !(inputs.delegations[cursor].grant.id == held)) {
      authority_lost = true;
      add(divergences, DivergenceKind::DelegationUnknownToFederation, held, inputs.report.site,
          federation_generation, reported_membership,
          "the site holds delegation " + to_string(held) +
              " and the federation has no record of it");
      continue;
    }
    if (inputs.delegations[cursor].revoked) {
      authority_lost = true;
      add(divergences, DivergenceKind::DelegationRevokedVersusHeld, held, inputs.report.site,
          federation_generation, reported_membership,
          "the site holds delegation " + to_string(held) + " which was revoked at federation generation " +
              std::to_string(inputs.delegations[cursor].revoked_at.value()));
    }
  }

  if (inputs.exclusive_double_held) {
    add(divergences, DivergenceKind::ExclusiveGrantDoubleHeld, DelegationId{}, inputs.report.site,
        federation_generation, reported_membership,
        "an exclusive grant is claimed by this site and by another site; exclusive authority may "
        "not be held twice across a partition");
    finish(divergences);
    evaluation.outcome = ReconciliationOutcome::ConflictUnresolved;
    return evaluation;
  }

  if (authority_lost) {
    finish(divergences);
    evaluation.outcome = ReconciliationOutcome::SiteAdoptsAndRemainsFenced;
    evaluation.adopt = AcceptedGeneration{federation_generation.value()};
    evaluation.fenced = true;
    return evaluation;
  }

  if (reported.value() < federation_generation.value()) {
    if (inputs.site.local_epoch < inputs.report.local_epoch &&
        !inputs.report.local_history.is_zero()) {
      add(divergences, DivergenceKind::SiteLocalEpochAhead, DelegationId{}, inputs.report.site,
          federation_generation, reported_membership,
          "the site reports local epoch " + std::to_string(inputs.report.local_epoch.value()) +
              " and the federation last recorded " +
              std::to_string(inputs.site.local_epoch.value()) +
              "; local authority is unaffected and the divergence is recorded");
    }
    finish(divergences);
    evaluation.outcome = ReconciliationOutcome::SiteAdoptsFederation;
    evaluation.adopt = AcceptedGeneration{federation_generation.value()};
    return evaluation;
  }

  // The site reports the current generation. Two states may only share a
  // generation if they are the same state, so the history digests must agree.
  if (inputs.report.accepted_history != inputs.federation_history) {
    add(divergences, DivergenceKind::HistoryDigestMismatch, DelegationId{}, inputs.report.site,
        federation_generation, reported_membership,
        "site accepted history " + inputs.report.accepted_history.to_hex() +
            " and the federation holds " + inputs.federation_history.to_hex() +
            " at the same generation " + std::to_string(federation_generation.value()));
    finish(divergences);
    evaluation.outcome = ReconciliationOutcome::ConflictUnresolved;
    return evaluation;
  }

  if (inputs.site.local_epoch.value() < inputs.report.local_epoch.value() &&
      !inputs.report.local_history.is_zero()) {
    add(divergences, DivergenceKind::SiteLocalEpochAhead, DelegationId{}, inputs.report.site,
        federation_generation, reported_membership,
        "the site reports local epoch " + std::to_string(inputs.report.local_epoch.value()) +
            " and the federation last recorded " +
            std::to_string(inputs.site.local_epoch.value()));
  }

  // A report that claims more delegations than the limits allow is rejected
  // before it is used, so the loop above never runs on an unbounded list.
  if (static_cast<std::uint64_t>(divergences.size()) > limits.max_reconciliations) {
    divergences.resize(static_cast<std::size_t>(limits.max_reconciliations));
  }

  finish(divergences);
  evaluation.outcome = ReconciliationOutcome::InSync;
  return evaluation;
}

}  // namespace dcf
