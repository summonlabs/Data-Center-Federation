// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <memory>
#include <string>
#include <vector>

#include "dcf/engine.hpp"
#include "test_harness.hpp"

namespace {

using namespace dcf;

const FederationId kFederation{0x1111ULL, 0x2222ULL};
const SiteId kAlpha{0x01ULL, 0xaaULL};
const SiteId kBeta{0x02ULL, 0xbbULL};
const SiteId kGamma{0x03ULL, 0xccULL};

CompatibilityDeclaration make_declaration(const std::vector<std::string>& capabilities,
                                          VersionRange protocol = VersionRange{Version{1, 0},
                                                                              Version{9, 9}}) {
  CompatibilityDeclaration declaration;
  declaration.implementation = "summon.test-site";
  declaration.implementation_version = Version{1, 0};
  declaration.protocol = protocol;
  for (const std::string& name : capabilities) {
    CapabilityDeclaration entry;
    entry.id = CapabilityId{name};
    entry.supported = VersionRange{Version{1, 0}, Version{2, 0}};
    entry.requirement = CapabilityRequirement::Required;
    declaration.capabilities.push_back(std::move(entry));
  }
  return declaration;
}

CompatibilityWindow make_window(const std::string& name) {
  CompatibilityWindow window;
  window.capability = CapabilityId{name};
  window.offered = VersionRange{Version{1, 0}, Version{3, 0}};
  window.effective_from = FederationGeneration{1};
  return window;
}

CommandOutcome as_outcome(const Result<CommandOutcome>& result) {
  if (result.has_value()) {
    return result.value();
  }
  CommandOutcome outcome;
  outcome.code = result.error().code;
  outcome.detail = result.error().detail;
  return outcome;
}

class Fixture {
 public:
  explicit Fixture(bool declare_default_window = true)
      : engine_(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000})) {
    const auto seeded = engine_.replay(genesis_entry(kFederation, UnixMillis{1000}));
    seeded_ok = seeded.has_value();
    if (seeded_ok && declare_default_window) {
      seeded_ok = submit(DeclareWindowCommand{make_window("dcf.membership")}).code == ErrorCode::None;
    }
  }

  [[nodiscard]] bool seeded() const { return seeded_ok; }

  CommandOutcome submit(CommandPayload payload, OperationId operation = {},
                        SiteId origin = {}, FederationGeneration origin_generation = {}) {
    Command command;
    command.payload = std::move(payload);
    command.operation = operation;
    command.origin = origin;
    command.origin_generation = origin_generation;
    return as_outcome(engine_.submit(command));
  }

  CommandOutcome window(const std::string& name) {
    return submit(DeclareWindowCommand{make_window(name)});
  }

  CommandOutcome register_site(SiteId site, const std::string& name,
                               const std::vector<std::string>& capabilities,
                               VersionRange protocol = VersionRange{Version{1, 0},
                                                                   Version{9, 9}}) {
    return submit(RegisterSiteCommand{site, name, make_declaration(capabilities, protocol)});
  }

  [[nodiscard]] FederationEngine& engine() { return engine_; }
  [[nodiscard]] const FederationState& state() const { return engine_.state(); }

 private:
  FederationEngine engine_;
  bool seeded_ok{false};
};

// Takes a site from registration all the way to active. It reports failure
// rather than asserting, because the assertion helpers belong to a case body.
[[nodiscard]] bool admit_to_active(Fixture& fixture, SiteId site, const std::string& name,
                                   const std::vector<std::string>& capabilities) {
  if (fixture.register_site(site, name, capabilities).code != ErrorCode::None) {
    return false;
  }
  if (fixture.submit(ValidateSiteCommand{site}).code != ErrorCode::None) {
    return false;
  }
  const MembershipRecord* record = fixture.state().find_site(site);
  if (record == nullptr) {
    return false;
  }
  if (fixture.submit(AdmitSiteCommand{site, record->generation}).code != ErrorCode::None) {
    return false;
  }
  const FederationGeneration current = fixture.state().generation();
  ActivateSiteCommand activate;
  activate.site = site;
  activate.accepted = AcceptedGeneration{current.value()};
  activate.accepted_history = fixture.state().site_authority_digest(site);
  activate.local_epoch = SiteLocalEpoch{1};
  return fixture.submit(activate).code == ErrorCode::None;
}

}  // namespace

DCF_TEST(engine, genesis_establishes_a_federation) {
  Fixture fixture(false);
  DCF_CHECK(fixture.seeded());
  DCF_CHECK_EQ(fixture.state().federation(), kFederation);
  DCF_CHECK_EQ(fixture.state().generation().value(), 1ULL);
  DCF_CHECK_EQ(fixture.state().sequence().value(), 1ULL);
  DCF_CHECK_EQ(fixture.state().site_count(), std::size_t{0});
}

DCF_TEST(engine, lifecycle_reaches_active) {
  Fixture fixture;
  DCF_REQUIRE(fixture.window("dcf.membership").code == ErrorCode::None);
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* record = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(record != nullptr);
  DCF_CHECK_EQ(record->state, MembershipState::Active);
  DCF_CHECK_EQ(record->generation.value(), 4ULL);
  DCF_CHECK(record->accepted.value() >= 1ULL);
}

DCF_TEST(engine, illegal_lifecycle_step_is_refused_without_changing_state) {
  Fixture fixture;
  DCF_REQUIRE(fixture.register_site(kAlpha, "alpha", {"dcf.membership"}).code == ErrorCode::None);
  const FederationGeneration before = fixture.state().generation();
  const auto outcome = fixture.submit(AdmitSiteCommand{kAlpha, MembershipGeneration{1}});
  DCF_CHECK_EQ(outcome.code, ErrorCode::Conflict);
  DCF_CHECK_EQ(fixture.state().generation().value(), before.value());
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::Candidate);
}

DCF_TEST(engine, compatibility_refusal_is_explainable_and_terminal) {
  Fixture fixture;
  DCF_REQUIRE(fixture.window("dcf.membership").code == ErrorCode::None);
  DCF_REQUIRE(fixture.register_site(kAlpha, "alpha", {"dcf.membership", "dcf.missing"}).code ==
              ErrorCode::None);
  const auto outcome = fixture.submit(ValidateSiteCommand{kAlpha});
  DCF_CHECK_EQ(outcome.code, ErrorCode::None);
  DCF_REQUIRE(outcome.compatibility.has_value());
  DCF_CHECK_EQ(outcome.compatibility->decision, AdmissionDecision::Refused);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::RefusedIncompatible);
  DCF_CHECK(!outcome.compatibility->findings.empty());
  DCF_CHECK(outcome.compatibility->findings.front().code ==
            CompatibilityFindingCode::RequiredCapabilityMissing);
  // A refused candidate is terminal: evaluation cannot be repeated.
  DCF_CHECK_EQ(fixture.submit(ValidateSiteCommand{kAlpha}).code, ErrorCode::Conflict);
}

DCF_TEST(engine, protocol_range_disjoint_is_refused) {
  Fixture fixture;
  DCF_REQUIRE(fixture.register_site(kAlpha, "alpha", {"dcf.membership"},
                                    VersionRange{Version{7, 0}, Version{8, 0}})
                  .code == ErrorCode::None);
  const auto outcome = fixture.submit(ValidateSiteCommand{kAlpha});
  DCF_REQUIRE(outcome.compatibility.has_value());
  DCF_CHECK_EQ(outcome.compatibility->decision, AdmissionDecision::Refused);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::RefusedIncompatible);
}

DCF_TEST(engine, deprecated_window_constrains_rather_than_refusing) {
  Fixture fixture;
  CompatibilityWindow window = make_window("dcf.membership");
  window.soft_deprecate = FederationGeneration{2};
  DCF_REQUIRE(fixture.submit(DeclareWindowCommand{window}).code == ErrorCode::None);
  DCF_REQUIRE(fixture.register_site(kAlpha, "alpha", {"dcf.membership"}).code == ErrorCode::None);
  const auto outcome = fixture.submit(ValidateSiteCommand{kAlpha});
  DCF_REQUIRE(outcome.compatibility.has_value());
  DCF_CHECK_EQ(outcome.compatibility->decision, AdmissionDecision::AdmittedConstrained);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::CompatibilityValidated);
}

DCF_TEST(engine, generation_advances_only_on_authority_changes) {
  Fixture fixture;
  const FederationGeneration before_register = fixture.state().generation();
  DCF_REQUIRE(fixture.register_site(kAlpha, "alpha", {"dcf.membership"}).code == ErrorCode::None);
  const FederationGeneration after_register = fixture.state().generation();
  DCF_CHECK_EQ(after_register.value(), before_register.value() + 1);

  // Recording contact is observation, not authority.
  RecordContactCommand heartbeat;
  heartbeat.site = kAlpha;
  heartbeat.local_epoch = SiteLocalEpoch{3};
  heartbeat.at = UnixMillis{2000};
  const auto contact = fixture.submit(heartbeat);
  DCF_CHECK_EQ(contact.code, ErrorCode::None);
  DCF_CHECK_EQ(fixture.state().generation().value(), after_register.value());
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->last_contact.value, 2000);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->local_epoch.value(), 3ULL);
}

DCF_TEST(engine, delegation_grant_and_revocation_are_generation_stamped) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  DelegationGrant grant;
  grant.id = DelegationId{0x5ULL, 0x6ULL};
  grant.grantee = kAlpha;
  grant.scope_mask = mask_of(DelegationScope::MembershipAdmit);
  grant.grantee_membership_generation = alpha->generation;
  grant.survives_partition = false;
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{grant}).code, ErrorCode::None);

  const DelegationRecord* stored = fixture.state().find_delegation(grant.id);
  DCF_REQUIRE(stored != nullptr);
  DCF_CHECK_EQ(stored->grant.granted_at.value(), fixture.state().generation().value());
  DCF_CHECK(!stored->revoked);

  DCF_CHECK_EQ(fixture.submit(RevokeDelegationCommand{grant.id, "no longer needed"}).code,
               ErrorCode::None);
  DCF_CHECK(fixture.state().find_delegation(grant.id)->revoked);
  // Revocation is monotonic: revoking again changes nothing.
  const FederationGeneration before = fixture.state().generation();
  const auto again = fixture.submit(RevokeDelegationCommand{grant.id, "again"});
  DCF_CHECK_EQ(again.code, ErrorCode::None);
  DCF_CHECK_EQ(fixture.state().generation().value(), before.value());
}

DCF_TEST(engine, delegation_stamped_with_a_stale_membership_generation_is_refused) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  DelegationGrant grant;
  grant.id = DelegationId{0x5ULL, 0x7ULL};
  grant.grantee = kAlpha;
  grant.scope_mask = mask_of(DelegationScope::MembershipAdmit);
  grant.grantee_membership_generation = MembershipGeneration{1};
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{grant}).code, ErrorCode::StaleGeneration);
}

DCF_TEST(engine, exclusive_authority_cannot_be_double_delegated) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  DCF_REQUIRE(admit_to_active(fixture, kBeta, "beta", {"dcf.membership"}));

  DelegationGrant first;
  first.id = DelegationId{0x9ULL, 0x1ULL};
  first.grantee = kAlpha;
  first.scope_mask = mask_of(DelegationScope::ReconciliationWitness);
  first.grantee_membership_generation = fixture.state().find_site(kAlpha)->generation;
  first.exclusive = true;
  first.survives_partition = true;
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{first}).code, ErrorCode::None);

  DelegationGrant second = first;
  second.id = DelegationId{0x9ULL, 0x2ULL};
  second.grantee = kBeta;
  second.grantee_membership_generation = fixture.state().find_site(kBeta)->generation;
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{second}).code, ErrorCode::Conflict);

  // Once the first is revoked the exclusive slot is free again.
  DCF_CHECK_EQ(fixture.submit(RevokeDelegationCommand{first.id, "handing over"}).code,
               ErrorCode::None);
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{second}).code, ErrorCode::None);
}

DCF_TEST(engine, remote_delegated_command_requires_current_generation) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  DCF_REQUIRE(admit_to_active(fixture, kBeta, "beta", {"dcf.membership"}));

  DelegationGrant grant;
  grant.id = DelegationId{0xaULL, 0x1ULL};
  grant.grantee = kAlpha;
  grant.scope_mask = mask_of(DelegationScope::MembershipAdmit);
  grant.grantee_membership_generation = fixture.state().find_site(kAlpha)->generation;
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{grant}).code, ErrorCode::None);

  DCF_REQUIRE(fixture.register_site(kGamma, "gamma", {"dcf.membership"}).code == ErrorCode::None);
  const FederationGeneration current = fixture.state().generation();

  // Stamped with an older generation: refused as stale.
  const auto stale = fixture.submit(ValidateSiteCommand{kGamma}, {}, kAlpha,
                                    FederationGeneration{current.value() - 1});
  DCF_CHECK_EQ(stale.code, ErrorCode::StaleGeneration);

  // Stamped correctly: accepted, because the delegation covers the scope.
  const auto fresh = fixture.submit(ValidateSiteCommand{kGamma}, {}, kAlpha, current);
  DCF_CHECK_EQ(fresh.code, ErrorCode::None);

  // A site without the delegation cannot validate anyone.
  DCF_REQUIRE(fixture.register_site(SiteId{4, 4}, "delta", {"dcf.membership"}).code ==
              ErrorCode::None);
  const auto unauthorised =
      fixture.submit(ValidateSiteCommand{SiteId{4, 4}}, {}, kBeta, fixture.state().generation());
  DCF_CHECK_EQ(unauthorised.code, ErrorCode::InsufficientAuthority);
}

DCF_TEST(engine, idempotent_retry_replays_the_recorded_outcome) {
  Fixture fixture;
  const OperationId operation{0x1ULL, 0x2ULL};
  const auto first = fixture.submit(RegisterSiteCommand{kAlpha, "alpha", make_declaration({"dcf.membership"})},
                                    operation, kAlpha);
  DCF_CHECK_EQ(first.code, ErrorCode::None);
  const FederationGeneration after_first = fixture.state().generation();

  const auto retry = fixture.submit(RegisterSiteCommand{kAlpha, "alpha", make_declaration({"dcf.membership"})},
                                    operation, kAlpha);
  DCF_CHECK_EQ(retry.code, ErrorCode::None);
  DCF_CHECK(retry.detail.find("replayed") != std::string::npos);
  DCF_CHECK_EQ(fixture.state().generation().value(), after_first.value());

  // The same operation identifier with a different request is a conflict.
  const auto conflicting =
      fixture.submit(RegisterSiteCommand{kBeta, "beta", make_declaration({"dcf.membership"})},
                     operation, kAlpha);
  DCF_CHECK_EQ(conflicting.code, ErrorCode::IdempotencyConflict);
}

DCF_TEST(engine, partition_suspends_non_surviving_delegated_authority) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  DCF_REQUIRE(admit_to_active(fixture, kBeta, "beta", {"dcf.membership"}));

  DelegationGrant grant;
  grant.id = DelegationId{0xbULL, 0x1ULL};
  grant.grantee = kAlpha;
  grant.scope_mask = mask_of(DelegationScope::MembershipAdmit);
  grant.grantee_membership_generation = fixture.state().find_site(kAlpha)->generation;
  grant.survives_partition = false;
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{grant}).code, ErrorCode::None);

  DCF_REQUIRE(fixture.register_site(kGamma, "gamma", {"dcf.membership"}).code == ErrorCode::None);

  // While connected, alpha may validate.
  DCF_CHECK_EQ(fixture.submit(ValidateSiteCommand{kGamma}, {}, kAlpha, fixture.state().generation())
                   .code,
               ErrorCode::None);

  DCF_REQUIRE(fixture.submit(SetConnectivityCommand{kAlpha, LinkState::Partitioned}).code ==
              ErrorCode::None);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::Partitioned);

  DCF_REQUIRE(fixture.register_site(SiteId{5, 5}, "epsilon", {"dcf.membership"}).code ==
              ErrorCode::None);
  const auto suspended =
      fixture.submit(ValidateSiteCommand{SiteId{5, 5}}, {}, kAlpha, fixture.state().generation());
  DCF_CHECK_EQ(suspended.code, ErrorCode::InsufficientAuthority);

  // Reconnect returns the site to the state it held before the partition.
  DCF_REQUIRE(fixture.submit(SetConnectivityCommand{kAlpha, LinkState::Connected}).code ==
              ErrorCode::None);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::Active);
}

DCF_TEST(engine, removed_member_is_fenced_and_never_reinstated) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  DCF_REQUIRE(fixture.submit(RemoveSiteCommand{kAlpha, "decommissioned"}).code == ErrorCode::None);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::Removed);

  // The site tries to re-register and to rejoin.
  DCF_CHECK_EQ(fixture.submit(RegisterSiteCommand{kAlpha, "alpha", make_declaration({"dcf.membership"})})
                   .code,
               ErrorCode::DuplicateIdentity);

  SiteReport report;
  report.site = kAlpha;
  report.accepted = AcceptedGeneration{fixture.state().generation().value() - 1};
  report.membership_generation = fixture.state().find_site(kAlpha)->generation;
  const auto rejoin = fixture.submit(RejoinCommand{report, false});
  DCF_CHECK_EQ(rejoin.code, ErrorCode::None);
  DCF_REQUIRE(rejoin.reconciliation.has_value());
  DCF_CHECK_EQ(rejoin.reconciliation->outcome, ReconciliationOutcome::SiteAdoptsAndRemainsFenced);
  DCF_CHECK(rejoin.reconciliation->fenced);
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->state, MembershipState::Removed);
}

DCF_TEST(engine, rejoin_from_a_behind_site_adopts_the_federation_generation) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  SiteReport report;
  report.site = kAlpha;
  report.accepted = AcceptedGeneration{alpha->accepted.value()};
  report.membership_generation = alpha->generation;
  const auto rejoin = fixture.submit(RejoinCommand{report, false}, {}, kAlpha);
  DCF_REQUIRE(rejoin.reconciliation.has_value());
  DCF_CHECK_EQ(rejoin.reconciliation->outcome, ReconciliationOutcome::SiteAdoptsFederation);
  DCF_CHECK_EQ(rejoin.reconciliation->adopt.value(), fixture.state().generation().value());
}

DCF_TEST(engine, rejoin_in_sync_is_recognised) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  SiteReport report;
  report.site = kAlpha;
  report.accepted = AcceptedGeneration{fixture.state().generation().value()};
  report.accepted_history = fixture.state().site_authority_digest(kAlpha);
  report.membership_generation = alpha->generation;
  const auto rejoin = fixture.submit(RejoinCommand{report, false}, {}, kAlpha);
  DCF_REQUIRE(rejoin.reconciliation.has_value());
  DCF_CHECK_EQ(rejoin.reconciliation->outcome, ReconciliationOutcome::InSync);
  DCF_CHECK(rejoin.reconciliation->divergences.empty());
}

DCF_TEST(engine, rejoin_with_a_divergent_history_is_never_last_write_wins) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  SiteReport report;
  report.site = kAlpha;
  report.accepted = AcceptedGeneration{fixture.state().generation().value()};
  report.accepted_history = Digest{};  // a different history at the same generation
  report.membership_generation = alpha->generation;
  const auto rejoin = fixture.submit(RejoinCommand{report, false}, {}, kAlpha);
  DCF_CHECK_EQ(rejoin.code, ErrorCode::Conflict);
  DCF_REQUIRE(rejoin.reconciliation.has_value());
  DCF_CHECK_EQ(rejoin.reconciliation->outcome, ReconciliationOutcome::ConflictUnresolved);
  DCF_CHECK(!rejoin.reconciliation->divergences.empty());
}

DCF_TEST(engine, rejoin_claiming_an_unknown_generation_is_indeterminate) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  SiteReport report;
  report.site = kAlpha;
  report.accepted = AcceptedGeneration{fixture.state().generation().value() + 50};
  report.observed_generations.push_back(FederationGeneration{fixture.state().generation().value() + 50});
  report.membership_generation = alpha->generation;
  const auto rejoin = fixture.submit(RejoinCommand{report, false}, {}, kAlpha);
  DCF_CHECK_EQ(rejoin.code, ErrorCode::Indeterminate);
  DCF_REQUIRE(rejoin.reconciliation.has_value());
  DCF_CHECK_EQ(rejoin.reconciliation->outcome, ReconciliationOutcome::FederationRegression);
  // The federation did not adopt the site's claim.
  DCF_CHECK_EQ(fixture.state().find_site(kAlpha)->accepted.value(), alpha->accepted.value());
}

DCF_TEST(engine, rejoin_holding_a_revoked_delegation_is_fenced) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  DelegationGrant grant;
  grant.id = DelegationId{0xcULL, 0x1ULL};
  grant.grantee = kAlpha;
  grant.scope_mask = mask_of(DelegationScope::ObservabilityRead);
  grant.grantee_membership_generation = alpha->generation;
  DCF_CHECK_EQ(fixture.submit(GrantDelegationCommand{grant}).code, ErrorCode::None);
  DCF_CHECK_EQ(fixture.submit(RevokeDelegationCommand{grant.id, "withdrawn"}).code, ErrorCode::None);

  SiteReport report;
  report.site = kAlpha;
  report.accepted = AcceptedGeneration{fixture.state().generation().value() - 1};
  report.membership_generation = alpha->generation;
  report.held_delegations.push_back(grant.id);
  const auto rejoin = fixture.submit(RejoinCommand{report, false}, {}, kAlpha);
  DCF_REQUIRE(rejoin.reconciliation.has_value());
  DCF_CHECK_EQ(rejoin.reconciliation->outcome, ReconciliationOutcome::SiteAdoptsAndRemainsFenced);
  DCF_CHECK(rejoin.reconciliation->fenced);
}

DCF_TEST(engine, malformed_site_report_is_refused_before_use) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  SiteReport report;
  report.site = kAlpha;
  report.membership_generation = alpha->generation;
  report.held_delegations.push_back(DelegationId{1, 1});
  report.held_delegations.push_back(DelegationId{1, 1});
  const auto rejoin = fixture.submit(RejoinCommand{report, false}, {}, kAlpha);
  DCF_CHECK_EQ(rejoin.code, ErrorCode::DuplicateIdentity);
}

DCF_TEST(engine, conflict_resolution_requires_an_unresolved_record) {
  Fixture fixture;
  DCF_REQUIRE(admit_to_active(fixture, kAlpha, "alpha", {"dcf.membership"}));
  const MembershipRecord* alpha = fixture.state().find_site(kAlpha);
  DCF_REQUIRE(alpha != nullptr);

  SiteReport report;
  report.site = kAlpha;
  report.accepted = AcceptedGeneration{fixture.state().generation().value()};
  report.accepted_history = Digest{};
  report.membership_generation = alpha->generation;
  DCF_REQUIRE(fixture.submit(RejoinCommand{report, false}, {}, kAlpha).code == ErrorCode::Conflict);

  DCF_REQUIRE(fixture.state().reconciliations().size() == 1);
  const ReconciliationId id = fixture.state().reconciliations().begin()->first;

  ConflictResolution resolution;
  resolution.reconciliation = id;
  resolution.choice = ConflictResolution::Choice::FederationAuthoritative;
  resolution.authority_reference = "change-ticket-4711";

  // A resolution that does not name the authority it is made under is refused.
  ConflictResolution anonymous = resolution;
  anonymous.authority_reference.clear();
  DCF_CHECK_EQ(fixture.submit(ResolveConflictCommand{anonymous}).code, ErrorCode::InvalidArgument);
  DCF_CHECK_EQ(fixture.submit(ResolveConflictCommand{resolution}).code, ErrorCode::None);
  DCF_CHECK(fixture.state().find_reconciliation(id)->resolved);
  DCF_CHECK_EQ(fixture.submit(ResolveConflictCommand{resolution}).code, ErrorCode::Conflict);

  // An unresolvable identifier is reported as such rather than ignored.
  resolution.reconciliation = ReconciliationId{0xdeadULL, 0xbeefULL};
  DCF_CHECK_EQ(fixture.submit(ResolveConflictCommand{resolution}).code, ErrorCode::NotFound);
}

DCF_TEST(engine, authority_digest_is_stable_and_changes_with_authority) {
  Fixture fixture;
  DCF_REQUIRE(fixture.register_site(kAlpha, "alpha", {"dcf.membership"}).code == ErrorCode::None);
  const Digest first = fixture.state().authority_digest();
  DCF_CHECK_EQ(fixture.state().authority_digest(), first);

  DCF_REQUIRE(fixture.submit(SetConnectivityCommand{kAlpha, LinkState::Degraded}).code ==
              ErrorCode::None);
  DCF_CHECK(fixture.state().authority_digest() != first);
}

DCF_TEST(engine, state_rejects_a_journal_gap) {
  Fixture fixture;
  JournalEntry entry;
  entry.sequence = JournalSequence{9};
  entry.generation = FederationGeneration{2};
  entry.changes.push_back(ConnectivityChangedMutation{kAlpha, LinkState::Connected});
  const auto applied = fixture.engine().replay(entry);
  DCF_REQUIRE(!applied.has_value());
  DCF_CHECK_EQ(applied.error().code, ErrorCode::Conflict);
}

DCF_TEST(engine, state_rejects_a_generation_that_does_not_follow) {
  Fixture fixture;
  JournalEntry entry;
  entry.sequence = JournalSequence{2};
  entry.generation = FederationGeneration{5};
  SiteRegisteredMutation registration;
  registration.record.site = kAlpha;
  registration.record.state = MembershipState::Candidate;
  registration.record.generation = MembershipGeneration{1};
  registration.record.entered_at = FederationGeneration{5};
  entry.changes.push_back(registration);
  const auto applied = fixture.engine().replay(entry);
  DCF_REQUIRE(!applied.has_value());
  DCF_CHECK_EQ(applied.error().code, ErrorCode::Conflict);
}
