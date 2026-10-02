// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/engine.hpp"

#include <algorithm>
#include <chrono>
#include <type_traits>

namespace dcf {
namespace {

class SystemClock final : public Clock {
 public:
  [[nodiscard]] UnixMillis wall_clock() const noexcept override {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return UnixMillis{static_cast<std::int64_t>(millis)};
  }
};

[[nodiscard]] std::shared_ptr<const Clock> default_clock() {
  static const std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>();
  return clock;
}

[[nodiscard]] CommandOutcome refuse(const FederationState& state, ErrorCode code,
                                    std::string detail) {
  CommandOutcome outcome;
  outcome.code = code;
  outcome.detail = std::move(detail);
  outcome.generation = state.generation();
  return outcome;
}

[[nodiscard]] CommandOutcome accept(FederationGeneration next,
                                    MembershipGeneration membership) {
  CommandOutcome outcome;
  outcome.code = ErrorCode::None;
  outcome.generation = next;
  outcome.membership_generation = membership;
  return outcome;
}

// The outcome a durable receipt replays. The stored code and detail are returned
// verbatim so that a retry sees exactly what the original attempt saw.
[[nodiscard]] CommandOutcome replay_receipt(const IdempotencyReceipt& receipt,
                                            const FederationState& state) {
  CommandOutcome outcome;
  outcome.code = receipt.outcome;
  outcome.detail = receipt.outcome_detail + " (replayed from receipt at journal sequence " +
                   std::to_string(receipt.committed.value()) + ")";
  outcome.generation = receipt.generation;
  outcome.membership_generation = receipt.membership_generation;
  if (outcome.generation.value() > state.generation().value()) {
    outcome.generation = state.generation();
  }
  return outcome;
}

[[nodiscard]] std::vector<DelegationRecord> ordered_delegations(const FederationState& state) {
  std::vector<DelegationRecord> out;
  out.reserve(state.delegations().size());
  for (const auto& entry : state.delegations()) {
    out.push_back(entry.second);
  }
  return out;
}

// True when the origin holds an active delegation covering the scope, evaluated
// against the current generation, the current membership generation of the
// origin, and whether the origin is currently cut off.
[[nodiscard]] bool holds_scope(const FederationState& state, const MembershipRecord& origin,
                               DelegationScope scope, FederationGeneration at, UnixMillis now) {
  const std::uint16_t wanted = mask_of(scope);
  const bool partitioned = is_partitioned(origin);
  for (const auto& entry : state.delegations()) {
    const DelegationRecord& record = entry.second;
    if (!(record.grant.grantee == origin.site)) {
      continue;
    }
    if ((record.grant.scope_mask & wanted) == 0U) {
      continue;
    }
    const DelegationStatus status =
        evaluate_delegation(record, at, now, origin.generation, partitioned);
    if (confers_authority(status)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool exclusive_double_held(const FederationState& state, const SiteId& site,
                                         FederationGeneration at, UnixMillis now) {
  const MembershipRecord* record = state.find_site(site);
  if (record == nullptr) {
    return false;
  }
  const bool partitioned = is_partitioned(*record);
  for (const auto& mine : state.delegations()) {
    if (!(mine.second.grant.grantee == site) || !mine.second.grant.exclusive) {
      continue;
    }
    if (!is_indeterminate(evaluate_delegation(mine.second, at, now, record->generation,
                                              partitioned))) {
      continue;
    }
    for (const auto& other : state.delegations()) {
      if (other.second.grant.grantee == site || !other.second.grant.exclusive) {
        continue;
      }
      if ((other.second.grant.scope_mask & mine.second.grant.scope_mask) == 0U) {
        continue;
      }
      if (!selectors_overlap(other.second.grant.selectors, mine.second.grant.selectors)) {
        continue;
      }
      const MembershipRecord* holder = state.find_site(other.second.grant.grantee);
      if (holder == nullptr) {
        continue;
      }
      if (is_indeterminate(evaluate_delegation(other.second, at, now, holder->generation,
                                               is_partitioned(*holder)))) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

JournalEntry genesis_entry(FederationId federation, UnixMillis created) {
  JournalEntry entry;
  entry.sequence = JournalSequence{1};
  entry.generation = FederationGeneration{1};
  entry.recorded_at = created;
  entry.changes.push_back(GenesisMutation{federation, created});
  return entry;
}

FederationEngine::FederationEngine(Limits limits, std::shared_ptr<const Clock> clock)
    : limits_(limits), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = default_clock();
  }
}

FederationEngine::FederationEngine() : FederationEngine(Limits{}, default_clock()) {}

UnixMillis FederationEngine::now() const noexcept { return clock_->wall_clock(); }

Result<Ack> FederationEngine::replay(const JournalEntry& entry) {
  return state_.apply(entry, limits_);
}

Result<Ack> FederationEngine::restore(FederationState state) {
  if (state.sequence().value() < state_.sequence().value()) {
    return make_error(ErrorCode::StaleGeneration,
                      "recovery offered sequence " + std::to_string(state.sequence().value()) +
                          " which is behind the sequence already installed, " +
                          std::to_string(state_.sequence().value()));
  }
  state_ = std::move(state);
  return Ack{};
}

Result<Ack> FederationEngine::commit(const Plan& plan) {
  if (!plan.persists) {
    return Ack{};
  }
  return state_.apply(plan.entry, limits_);
}

Result<CommandOutcome> FederationEngine::submit(const Command& command) {
  const auto plan = evaluate(command);
  if (!plan) {
    return plan.error();
  }
  const auto committed = commit(plan.value());
  if (!committed) {
    return committed.error();
  }
  return plan.value().outcome;
}

CommandOutcome FederationEngine::check_authority(const Command& command) const {
  if (command.origin.is_zero()) {
    // The federation operator holds the federation's own authority.
    return accept(state_.generation(), MembershipGeneration{});
  }

  const MembershipRecord* origin = state_.find_site(command.origin);

  if (is_self_scoped(command.payload)) {
    const auto subject = subject_site(command.payload);
    if (!subject.has_value() || !(subject.value() == command.origin)) {
      return refuse(state_, ErrorCode::InsufficientAuthority,
                    "site " + to_string(command.origin) + " may only submit " +
                        std::string(command_name(command.payload)) + " about itself");
    }
    if (origin == nullptr) {
      // A site with no record yet can only be introducing itself. Any other act
      // would be a claim to authority the federation has never granted.
      if (std::holds_alternative<RegisterSiteCommand>(command.payload)) {
        return accept(state_.generation(), MembershipGeneration{});
      }
      return refuse(state_, ErrorCode::NotFound,
                    "site " + to_string(command.origin) +
                        " has no membership record and cannot submit " +
                        std::string(command_name(command.payload)));
    }
    if (is_terminal(origin->state) &&
        !std::holds_alternative<RejoinCommand>(command.payload) &&
        !std::holds_alternative<RecordContactCommand>(command.payload)) {
      return refuse(state_, ErrorCode::Revoked,
                    "site " + to_string(command.origin) + " is recorded as " +
                        std::string(to_string(origin->state)) +
                        " and may only report in, not act");
    }
    return accept(state_.generation(), MembershipGeneration{});
  }

  if (origin == nullptr) {
    return refuse(state_, ErrorCode::NotFound,
                  "site " + to_string(command.origin) +
                      " has no membership record and cannot submit commands");
  }

  const auto scope = required_scope(command.payload);
  if (!scope.has_value()) {
    return refuse(state_, ErrorCode::InsufficientAuthority,
                  std::string(command_name(command.payload)) +
                      " is reserved to the federation operator");
  }

  // Delegated authority is exactly what a partition can make stale, so a
  // delegated act is only accepted when it was composed against the current
  // generation.
  if (!(command.origin_generation == state_.generation())) {
    return refuse(state_, ErrorCode::StaleGeneration,
                  "site " + to_string(command.origin) + " composed this command against generation " +
                      std::to_string(command.origin_generation.value()) +
                      " and the federation is at generation " +
                      std::to_string(state_.generation().value()));
  }

  if (!holds_scope(state_, *origin, scope.value(), state_.generation(), now())) {
    return refuse(state_, ErrorCode::InsufficientAuthority,
                  "site " + to_string(command.origin) + " holds no active delegation for scope " +
                      to_string(scope.value()) + " at generation " +
                      std::to_string(state_.generation().value()));
  }

  return accept(state_.generation(), MembershipGeneration{});
}

CommandOutcome FederationEngine::dispatch(const Command& command,
                                          std::vector<MutationPayload>& changes,
                                          FederationGeneration next,
                                          bool& keep_on_refusal) const {
  const UnixMillis now = clock_->wall_clock();

  return std::visit(
      [&](const auto& payload) -> CommandOutcome {
        using T = std::decay_t<decltype(payload)>;

        if constexpr (std::is_same_v<T, RegisterSiteCommand>) {
          const MembershipRecord* existing = state_.find_site(payload.site);
          if (existing != nullptr) {
            return refuse(state_, ErrorCode::DuplicateIdentity,
                          "site " + to_string(payload.site) + " is already recorded as " +
                              std::string(to_string(existing->state)));
          }
          if (payload.site.is_zero()) {
            return refuse(state_, ErrorCode::InvalidArgument, "a site identifier may not be zero");
          }
          if (static_cast<std::uint64_t>(state_.sites().size()) >= limits_.max_sites) {
            return refuse(state_, ErrorCode::CapacityExhausted,
                          "the federation already holds " + std::to_string(state_.sites().size()) +
                              " sites which is the configured maximum of " +
                              std::to_string(limits_.max_sites));
          }
          auto declaration = canonicalize(payload.declaration, limits_);
          if (!declaration) {
            return refuse(state_, declaration.error().code, declaration.error().detail);
          }
          MembershipRecord record;
          record.site = payload.site;
          record.display_name = payload.display_name;
          record.state = MembershipState::Candidate;
          record.generation = MembershipGeneration{1};
          record.entered_at = next;
          record.declaration = std::move(declaration).value();
          record.declaration_digest = declaration_digest(record.declaration);
          record.admission.decision = AdmissionDecision::Refused;
          record.admission.declaration = record.declaration_digest;
          changes.push_back(SiteRegisteredMutation{record});
          return accept(next, record.generation);
        }

        else if constexpr (std::is_same_v<T, ValidateSiteCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          if (!(record->state == MembershipState::Candidate)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) + " is " +
                              std::string(to_string(record->state)) +
                              " and compatibility is only evaluated for a candidate");
          }
          const CompatibilityReport report = evaluate_compatibility(
              record->declaration, state_.window_list(), next, limits_);
          CommandOutcome outcome = accept(next, record->generation);
          outcome.compatibility = report;
          const MembershipState resulting =
              report.decision == AdmissionDecision::Refused
                  ? MembershipState::RefusedIncompatible
                  : MembershipState::CompatibilityValidated;
          changes.push_back(
              CompatibilityEvaluatedMutation{payload.site, resulting, 0, report});
          return outcome;
        }

        else if constexpr (std::is_same_v<T, AdmitSiteCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          if (!(record->state == MembershipState::CompatibilityValidated)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) + " is " +
                              std::string(to_string(record->state)) +
                              " and may only be admitted from compatibility_validated");
          }
          if (!(payload.expected_generation == record->generation)) {
            return refuse(state_, ErrorCode::StaleGeneration,
                          "site " + to_string(payload.site) + " is at membership generation " +
                              std::to_string(record->generation.value()) +
                              " and the admission was composed against generation " +
                              std::to_string(payload.expected_generation.value()));
          }
          changes.push_back(MembershipTransitionMutation{payload.site, MembershipState::Admitted,
                                                         MembershipState::Active, 0,
                                                         "admitted after compatibility validation"});
          return accept(next, MembershipGeneration{record->generation.value() + 1});
        }

        else if constexpr (std::is_same_v<T, ActivateSiteCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          if (!(record->state == MembershipState::Admitted)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) + " is " +
                              std::string(to_string(record->state)) +
                              " and may only be activated from admitted");
          }
          if (payload.accepted.value() > state_.generation().value()) {
            return refuse(state_, ErrorCode::Indeterminate,
                          "site " + to_string(payload.site) + " claims to have accepted generation " +
                              std::to_string(payload.accepted.value()) +
                              " which this federation has not issued; it is at generation " +
                              std::to_string(state_.generation().value()));
          }
          if (payload.accepted.value() == state_.generation().value() &&
              payload.accepted_history != state_.site_authority_digest(payload.site)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) +
                              " claims to have accepted history " +
                              payload.accepted_history.to_hex() +
                              " at generation " + std::to_string(state_.generation().value()) +
                              " and this federation holds " +
                              state_.site_authority_digest(payload.site).to_hex());
          }
          changes.push_back(MembershipTransitionMutation{payload.site, MembershipState::Active,
                                                         MembershipState::Active, 0,
                                                         "site confirmed the admitted generation"});
          SiteReportedStateMutation reported;
          reported.site = payload.site;
          reported.accepted = payload.accepted;
          reported.local_epoch = payload.local_epoch;
          reported.accepted_history = payload.accepted_history;
          reported.contact = now;
          changes.push_back(reported);
          return accept(next, MembershipGeneration{record->generation.value() + 1});
        }

        else if constexpr (std::is_same_v<T, ConstrainSiteCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          if (!(record->state == MembershipState::Active ||
                record->state == MembershipState::Partitioned)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) + " is " +
                              std::string(to_string(record->state)) +
                              " and may only be constrained from active or partitioned");
          }
          changes.push_back(MembershipTransitionMutation{
              payload.site, MembershipState::Constrained, record->pre_partition_state,
              payload.scopes, payload.reason});
          return accept(next, MembershipGeneration{record->generation.value() + 1});
        }

        else if constexpr (std::is_same_v<T, BeginDrainCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          if (!counts_as_member(record->state)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) + " is " +
                              std::string(to_string(record->state)) +
                              " and is not a member that can be drained");
          }
          if (record->state == MembershipState::Draining) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) +
                              " is already draining; draining a draining site is not a "
                              "transition the lifecycle defines");
          }
          changes.push_back(MembershipTransitionMutation{payload.site, MembershipState::Draining,
                                                         record->pre_partition_state,
                                                         record->constrained_scopes,
                                                         payload.reason});
          return accept(next, MembershipGeneration{record->generation.value() + 1});
        }

        else if constexpr (std::is_same_v<T, CancelDrainCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          if (!(record->state == MembershipState::Draining)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) + " is " +
                              std::string(to_string(record->state)) + " and is not draining");
          }
          const MembershipState target = record->constrained_scopes == 0U
                                             ? MembershipState::Active
                                             : MembershipState::Constrained;
          changes.push_back(MembershipTransitionMutation{payload.site, target,
                                                         record->pre_partition_state,
                                                         record->constrained_scopes,
                                                         payload.reason});
          return accept(next, MembershipGeneration{record->generation.value() + 1});
        }

        else if constexpr (std::is_same_v<T, RemoveSiteCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          // The lifecycle table is the authority on what may follow what. A
          // candidate that was never admitted is not a member to be removed,
          // and removal from a refused state is not a transition at all.
          if (!is_legal_transition(record->state, MembershipState::Removed)) {
            return refuse(state_, ErrorCode::Conflict,
                          "site " + to_string(payload.site) + " is " +
                              std::string(to_string(record->state)) +
                              " and cannot be removed from that state");
          }
          changes.push_back(MembershipTransitionMutation{payload.site, MembershipState::Removed,
                                                         record->pre_partition_state, 0,
                                                         payload.reason});
          return accept(next, MembershipGeneration{record->generation.value() + 1});
        }

        else if constexpr (std::is_same_v<T, SetConnectivityCommand>) {
          const MembershipRecord* record = state_.find_site(payload.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.site) + " is not known to this federation");
          }
          if (record->link == payload.link) {
            return accept(state_.generation(), record->generation);
          }
          changes.push_back(ConnectivityChangedMutation{payload.site, payload.link});

          const bool losing_reach = payload.link == LinkState::Partitioned;
          const bool regaining_reach = is_reachable(payload.link);
          if (losing_reach && counts_as_member(record->state) &&
              !(record->state == MembershipState::Partitioned)) {
            changes.push_back(MembershipTransitionMutation{payload.site,
                                                           MembershipState::Partitioned,
                                                           record->state, 0,
                                                           "the link to the site was lost"});
          } else if (regaining_reach && record->state == MembershipState::Partitioned) {
            const MembershipState target = is_legal_transition(MembershipState::Partitioned,
                                                               record->pre_partition_state)
                                               ? record->pre_partition_state
                                               : MembershipState::Active;
            changes.push_back(MembershipTransitionMutation{
                payload.site, target, record->pre_partition_state, record->constrained_scopes,
                "the link to the site was restored; the site returns to its pre-partition state"});
          }
          return accept(next, MembershipGeneration{record->generation.value() + 1});
        }

        else if constexpr (std::is_same_v<T, DeclareWindowCommand>) {
          CompatibilityWindow window = payload.window;
          if (!is_valid_capability_name(window.capability.value)) {
            return refuse(state_, ErrorCode::InvalidArgument,
                          "window names an invalid capability");
          }
          if (window.offered.is_empty()) {
            return refuse(state_, ErrorCode::InvalidArgument,
                          "window offers an empty version range");
          }
          if (!window.soft_deprecate.is_zero() && window.soft_deprecate < window.effective_from) {
            return refuse(state_, ErrorCode::InvalidArgument,
                          "a window may not deprecate before it takes effect");
          }
          if (!window.hard_remove.is_zero() &&
              window.hard_remove < window.soft_deprecate) {
            return refuse(state_, ErrorCode::InvalidArgument,
                          "a window may not remove a capability before deprecating it");
          }
          changes.push_back(CompatibilityWindowDeclaredMutation{window});
          return accept(next, MembershipGeneration{});
        }

        else if constexpr (std::is_same_v<T, GrantDelegationCommand>) {
          DelegationGrant grant = payload.grant;
          if (grant.id.is_zero()) {
            return refuse(state_, ErrorCode::InvalidArgument,
                          "a delegation identifier may not be zero");
          }
          if (state_.find_delegation(grant.id) != nullptr) {
            return refuse(state_, ErrorCode::DuplicateIdentity,
                          "delegation " + to_string(grant.id) + " already exists");
          }
          const MembershipRecord* grantee = state_.find_site(grant.grantee);
          if (grantee == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "delegation names site " + to_string(grant.grantee) +
                              " which is not known to this federation");
          }
          if (grant.scope_mask == 0U ||
              (grant.scope_mask & static_cast<std::uint16_t>(~all_delegation_scopes())) != 0U) {
            return refuse(state_, ErrorCode::InvalidArgument,
                          "delegation scope mask " + to_string(grant.scope_mask) +
                              " is empty or names scopes this boundary does not define");
          }
          if (!(grant.grantee_membership_generation == grantee->generation)) {
            return refuse(state_, ErrorCode::StaleGeneration,
                          "delegation is stamped against membership generation " +
                              std::to_string(grant.grantee_membership_generation.value()) +
                              " and site " + to_string(grant.grantee) + " is at generation " +
                              std::to_string(grantee->generation.value()));
          }
          if (!grant.granted_at.is_zero() && !(grant.granted_at == next)) {
            return refuse(state_, ErrorCode::StaleGeneration,
                          "delegation declares it takes effect at generation " +
                              std::to_string(grant.granted_at.value()) +
                              " and this command would commit at generation " +
                              std::to_string(next.value()));
          }
          auto selectors = canonicalize_selectors(grant.selectors, limits_);
          if (!selectors) {
            return refuse(state_, selectors.error().code, selectors.error().detail);
          }
          grant.selectors = std::move(selectors).value();
          grant.granted_at = next;

          if (grant.exclusive) {
            const auto conflict =
                find_exclusive_conflict(ordered_delegations(state_), grant, state_.generation(), now);
            if (conflict.has_value()) {
              return refuse(
                  state_, ErrorCode::Conflict,
                  "exclusive authority over " + to_string(conflict->overlapping_scopes) +
                      " is already delegated by " + to_string(conflict->existing) + " to site " +
                      to_string(conflict->existing_grantee) +
                      " and its fate across the current state is not decided");
            }
          }

          changes.push_back(DelegationGrantedMutation{grant});
          return accept(next, MembershipGeneration{});
        }

        else if constexpr (std::is_same_v<T, RevokeDelegationCommand>) {
          const DelegationRecord* record = state_.find_delegation(payload.delegation);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "delegation " + to_string(payload.delegation) + " does not exist");
          }
          if (record->revoked) {
            CommandOutcome outcome = accept(state_.generation(), MembershipGeneration{});
            outcome.code = ErrorCode::None;
            outcome.detail = "delegation " + to_string(payload.delegation) +
                             " was already revoked at generation " +
                             std::to_string(record->revoked_at.value()) +
                             "; revocation is monotonic and no change was made";
            return outcome;
          }
          DelegationRevocation revocation;
          revocation.id = payload.delegation;
          revocation.revoked_at = next;
          revocation.reason = payload.reason;
          changes.push_back(DelegationRevokedMutation{revocation});
          return accept(next, MembershipGeneration{});
        }

        else if constexpr (std::is_same_v<T, RejoinCommand>) {
          const MembershipRecord* record = state_.find_site(payload.report.site);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "site " + to_string(payload.report.site) +
                              " is not known to this federation");
          }
          auto report = validate_site_report(payload.report, limits_);
          if (!report) {
            return refuse(state_, report.error().code, report.error().detail);
          }
          // A report is evidence. Whatever the reconciliation decides, the
          // record of what was reported and what was decided about it is kept, so
          // that a conflict is visible to an operator rather than merely refused.
          keep_on_refusal = true;
          ReconciliationInputs inputs;
          inputs.federation_generation = state_.generation();
          inputs.site = *record;
          inputs.delegations = state_.delegations_for(payload.report.site);
          inputs.federation_history = state_.site_authority_digest(payload.report.site);
          inputs.report = std::move(report).value();
          inputs.exclusive_double_held =
              exclusive_double_held(state_, payload.report.site, state_.generation(), now);
          inputs.now = now;

          const ReconciliationEvaluation evaluation =
              evaluate_reconciliation(inputs, limits_);

          ReconciliationRecord recon;
          recon.id = ReconciliationId{state_.sequence().value() + 1, payload.report.site.low};
          recon.site = payload.report.site;
          recon.federation_generation = state_.generation();
          recon.site_reported_generation = inputs.report.accepted;
          recon.federation_history = inputs.federation_history;
          recon.site_history = inputs.report.accepted_history;
          recon.outcome = evaluation.outcome;
          recon.divergences = evaluation.divergences;
          recon.committed = JournalSequence{state_.sequence().value() + 1};
          recon.observed_at = now;
          changes.push_back(ReconciliationRecordedMutation{recon});

          SiteReportedStateMutation reported;
          reported.site = payload.report.site;
          reported.accepted = inputs.report.accepted;
          reported.local_epoch = inputs.report.local_epoch;
          reported.accepted_history = inputs.report.accepted_history;
          reported.contact = now;
          changes.push_back(reported);

          CommandOutcome outcome = accept(state_.generation(), record->generation);
          outcome.reconciliation = evaluation;
          outcome.detail = std::string(to_string(evaluation.outcome));
          if (evaluation.outcome == ReconciliationOutcome::ConflictUnresolved ||
              evaluation.outcome == ReconciliationOutcome::FederationRegression ||
              evaluation.outcome == ReconciliationOutcome::Indeterminate) {
            outcome.code = evaluation.outcome == ReconciliationOutcome::FederationRegression
                               ? ErrorCode::Indeterminate
                               : ErrorCode::Conflict;
          }
          return outcome;
        }

        else if constexpr (std::is_same_v<T, ResolveConflictCommand>) {
          const ReconciliationRecord* record =
              state_.find_reconciliation(payload.resolution.reconciliation);
          if (record == nullptr) {
            return refuse(state_, ErrorCode::NotFound,
                          "reconciliation " + to_string(payload.resolution.reconciliation) +
                              " does not exist");
          }
          if (record->resolved) {
            return refuse(state_, ErrorCode::Conflict,
                          "reconciliation " + to_string(payload.resolution.reconciliation) +
                              " has already been resolved");
          }
          if (record->outcome != ReconciliationOutcome::ConflictUnresolved) {
            return refuse(state_, ErrorCode::Conflict,
                          "reconciliation " + to_string(payload.resolution.reconciliation) +
                              " is recorded as " + std::string(to_string(record->outcome)) +
                              " and there is nothing to resolve");
          }
          if (payload.resolution.authority_reference.empty()) {
            return refuse(state_, ErrorCode::InvalidArgument,
                          "a conflict resolution must name the authority it is made under");
          }
          changes.push_back(ConflictResolvedMutation{payload.resolution.reconciliation,
                                                     payload.resolution.choice,
                                                     payload.resolution.authority_reference});

          if (payload.resolution.choice ==
              ConflictResolution::Choice::ReissueExclusiveToSingleHolder) {
            for (const DivergenceRecord& divergence : record->divergences) {
              if (divergence.kind != DivergenceKind::ExclusiveGrantDoubleHeld) {
                continue;
              }
              for (const auto& entry : state_.delegations()) {
                if (!entry.second.grant.exclusive || entry.second.revoked) {
                  continue;
                }
                if (entry.second.grant.grantee == record->site) {
                  continue;
                }
                DelegationRevocation revocation;
                revocation.id = entry.second.grant.id;
                revocation.revoked_at = next;
                revocation.reason =
                    "exclusive authority reissued to a single holder by resolution of " +
                    to_string(record->id);
                changes.push_back(DelegationRevokedMutation{revocation});
              }
            }
          }
          return accept(next, MembershipGeneration{});
        }

        else {
        const MembershipRecord* record = state_.find_site(payload.site);
        if (record == nullptr) {
          return refuse(state_, ErrorCode::NotFound,
                        "site " + to_string(payload.site) + " is not known to this federation");
        }
        if (payload.accepted.value() > state_.generation().value()) {
          return refuse(state_, ErrorCode::Indeterminate,
                        "site " + to_string(payload.site) +
                            " reports it accepted generation " +
                            std::to_string(payload.accepted.value()) +
                            " which this federation has not issued; it is at generation " +
                            std::to_string(state_.generation().value()));
        }
        if (payload.accepted.value() == state_.generation().value() &&
            payload.accepted_history != state_.site_authority_digest(payload.site)) {
          return refuse(state_, ErrorCode::Conflict,
                        "site " + to_string(payload.site) + " reports history " +
                            payload.accepted_history.to_hex() + " at generation " +
                            std::to_string(state_.generation().value()) +
                            " and this federation holds " +
                            state_.site_authority_digest(payload.site).to_hex());
        }
        SiteReportedStateMutation reported;
        reported.site = payload.site;
        reported.accepted = payload.accepted;
        reported.local_epoch = payload.local_epoch;
        reported.accepted_history = payload.accepted_history;
        reported.contact = payload.at;
        changes.push_back(reported);
        return accept(state_.generation(), record->generation);
        }
      },
      command.payload);
}

Result<FederationEngine::Plan> FederationEngine::evaluate(const Command& command) const {
  Plan plan;
  const FederationGeneration current = state_.generation();
  const Digest request = command_digest(command);

  if (!state_.initialized()) {
    plan.outcome = refuse(state_, ErrorCode::Indeterminate,
                          "this federation has not been created yet");
    return plan;
  }

  // Idempotency is checked before anything else: a retry must see the original
  // answer even if the world has moved on since.
  if (!command.operation.is_zero()) {
    const IdempotencyKey key{state_.federation(), command.origin, command.operation};
    if (const IdempotencyReceipt* existing = state_.find_receipt(key)) {
      if (!(existing->request == request)) {
        plan.outcome = refuse(state_, ErrorCode::IdempotencyConflict,
                              "operation " + to_string(command.operation) +
                                  " was already committed as a different request");
        return plan;
      }
      plan.outcome = replay_receipt(*existing, state_);
      return plan;
    }
  }

  const CommandOutcome authority = check_authority(command);
  if (authority.code != ErrorCode::None) {
    plan.outcome = authority;
  } else {
    const auto next_generation = current.increment();
    if (!next_generation.has_value()) {
      plan.outcome = refuse(state_, ErrorCode::ArithmeticOverflow,
                            "the federation generation has reached its maximum");
      return plan;
    }
    bool keep_on_refusal = false;
    plan.outcome =
        dispatch(command, plan.entry.changes, next_generation.value(), keep_on_refusal);
    if (plan.outcome.code != ErrorCode::None && !keep_on_refusal) {
      // A refusal changes nothing, so the entry carries no changes even when the
      // dispatch produced some before deciding against them.
      plan.entry.changes.clear();
    }
  }

  const bool changes_authority =
      std::any_of(plan.entry.changes.begin(), plan.entry.changes.end(),
                  [](const MutationPayload& payload) {
                    return mutation_changes_authority(payload);
                  });

  plan.entry.sequence = JournalSequence{state_.sequence().value() + 1};
  plan.entry.recorded_at = clock_->wall_clock();
  plan.entry.generation = changes_authority ? current.increment().value() : current;

  if (!command.operation.is_zero()) {
    IdempotencyReceipt receipt;
    receipt.key = IdempotencyKey{state_.federation(), command.origin, command.operation};
    receipt.request = request;
    receipt.outcome = plan.outcome.code;
    receipt.outcome_detail = plan.outcome.detail;
    receipt.generation = plan.outcome.generation;
    receipt.membership_generation = plan.outcome.membership_generation;
    receipt.committed = plan.entry.sequence;
    receipt.recorded_at = plan.entry.recorded_at;
    plan.entry.receipt = std::move(receipt);
  }

  plan.persists = !plan.entry.changes.empty() || plan.entry.receipt.has_value();
  if (!plan.persists) {
    plan.entry = JournalEntry{};
  }
  return plan;
}

}  // namespace dcf
