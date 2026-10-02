// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "record_codec.hpp"

namespace dcf::detail {
namespace {

template <class Enum>
[[nodiscard]] Result<Enum> get_enum(Decoder& decoder, std::uint8_t maximum, const char* what) {
  const auto value = decoder.u8();
  if (!value) {
    return value.error();
  }
  if (value.value() > maximum) {
    return make_error(ErrorCode::MalformedInput,
                      std::string(what) + " value " + std::to_string(value.value()) +
                          " is not a known value");
  }
  return static_cast<Enum>(value.value());
}

template <class T, class EncodeItem>
void put_list(Encoder& encoder, const std::vector<T>& items, EncodeItem encode_item) {
  encoder.u32(static_cast<std::uint32_t>(items.size()));
  for (const T& item : items) {
    encode_item(encoder, item);
  }
}

template <class T, class DecodeItem>
[[nodiscard]] Result<std::vector<T>> get_list(Decoder& decoder, DecodeItem decode_item) {
  const auto count = decoder.collection_count();
  if (!count) {
    return count.error();
  }
  std::vector<T> items;
  items.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    auto item = decode_item(decoder);
    if (!item) {
      return item.error();
    }
    items.push_back(std::move(item).value());
  }
  return items;
}

}  // namespace

void put(Encoder& encoder, const VersionRange& range) {
  encoder.u32(range.minimum.major);
  encoder.u32(range.minimum.minor);
  encoder.u32(range.maximum.major);
  encoder.u32(range.maximum.minor);
}

Result<VersionRange> get_version_range(Decoder& decoder) {
  VersionRange range;
  const auto minimum_major = decoder.u32();
  if (!minimum_major) {
    return minimum_major.error();
  }
  const auto minimum_minor = decoder.u32();
  if (!minimum_minor) {
    return minimum_minor.error();
  }
  const auto maximum_major = decoder.u32();
  if (!maximum_major) {
    return maximum_major.error();
  }
  const auto maximum_minor = decoder.u32();
  if (!maximum_minor) {
    return maximum_minor.error();
  }
  range.minimum = Version{minimum_major.value(), minimum_minor.value()};
  range.maximum = Version{maximum_major.value(), maximum_minor.value()};
  return range;
}

void put(Encoder& encoder, CapabilityRequirement requirement) {
  encoder.u8(static_cast<std::uint8_t>(requirement));
}

Result<CapabilityRequirement> get_capability_requirement(Decoder& decoder) {
  return get_enum<CapabilityRequirement>(
      decoder, static_cast<std::uint8_t>(CapabilityRequirement::Optional),
      "capability requirement");
}

void put(Encoder& encoder, const CompatibilityDeclaration& declaration) {
  encoder.text(declaration.implementation);
  encoder.u32(declaration.implementation_version.major);
  encoder.u32(declaration.implementation_version.minor);
  put(encoder, declaration.protocol);
  put_list(encoder, declaration.capabilities,
           [](Encoder& target, const CapabilityDeclaration& entry) {
             target.text(entry.id.value);
             put(target, entry.supported);
             put(target, entry.requirement);
           });
  put_list(encoder, declaration.policies,
           [](Encoder& target, const PolicyCompatibilityRef& policy) {
             target.text(policy.domain);
             put(target, policy.version);
           });
}

Result<CompatibilityDeclaration> get_declaration(Decoder& decoder) {
  CompatibilityDeclaration declaration;
  const auto implementation = decoder.text();
  if (!implementation) {
    return implementation.error();
  }
  declaration.implementation = implementation.value();
  const auto major = decoder.u32();
  if (!major) {
    return major.error();
  }
  const auto minor = decoder.u32();
  if (!minor) {
    return minor.error();
  }
  declaration.implementation_version = Version{major.value(), minor.value()};
  const auto protocol = get_version_range(decoder);
  if (!protocol) {
    return protocol.error();
  }
  declaration.protocol = protocol.value();

  auto capabilities = get_list<CapabilityDeclaration>(
      decoder, [](Decoder& source) -> Result<CapabilityDeclaration> {
        CapabilityDeclaration entry;
        const auto id = source.text();
        if (!id) {
          return id.error();
        }
        entry.id = CapabilityId{id.value()};
        const auto supported = get_version_range(source);
        if (!supported) {
          return supported.error();
        }
        entry.supported = supported.value();
        const auto requirement = get_capability_requirement(source);
        if (!requirement) {
          return requirement.error();
        }
        entry.requirement = requirement.value();
        return entry;
      });
  if (!capabilities) {
    return capabilities.error();
  }
  declaration.capabilities = std::move(capabilities).value();

  auto policies = get_list<PolicyCompatibilityRef>(
      decoder, [](Decoder& source) -> Result<PolicyCompatibilityRef> {
        PolicyCompatibilityRef policy;
        const auto domain = source.text();
        if (!domain) {
          return domain.error();
        }
        policy.domain = domain.value();
        const auto version = get_version_range(source);
        if (!version) {
          return version.error();
        }
        policy.version = version.value();
        return policy;
      });
  if (!policies) {
    return policies.error();
  }
  declaration.policies = std::move(policies).value();
  return declaration;
}

void put(Encoder& encoder, const CompatibilityWindow& window) {
  encoder.text(window.capability.value);
  put(encoder, window.offered);
  encoder.u64(window.effective_from.value());
  encoder.u64(window.soft_deprecate.value());
  encoder.u64(window.hard_remove.value());
}

Result<CompatibilityWindow> get_window(Decoder& decoder) {
  CompatibilityWindow window;
  const auto capability = decoder.text();
  if (!capability) {
    return capability.error();
  }
  window.capability = CapabilityId{capability.value()};
  const auto offered = get_version_range(decoder);
  if (!offered) {
    return offered.error();
  }
  window.offered = offered.value();
  const auto effective = decoder.u64();
  if (!effective) {
    return effective.error();
  }
  const auto soft = decoder.u64();
  if (!soft) {
    return soft.error();
  }
  const auto hard = decoder.u64();
  if (!hard) {
    return hard.error();
  }
  window.effective_from = FederationGeneration{effective.value()};
  window.soft_deprecate = FederationGeneration{soft.value()};
  window.hard_remove = FederationGeneration{hard.value()};
  return window;
}

void put(Encoder& encoder, const DelegationGrant& grant) {
  put(encoder, grant.id);
  put(encoder, grant.grantee);
  encoder.u16(grant.scope_mask);
  put_list(encoder, grant.selectors,
           [](Encoder& target, const std::string& selector) { target.text(selector); });
  encoder.u64(grant.granted_at.value());
  encoder.u64(grant.grantee_membership_generation.value());
  encoder.u64(grant.expires_at.value());
  encoder.i64(grant.expires_at_wall.value);
  encoder.boolean(grant.survives_partition);
  encoder.boolean(grant.exclusive);
  put(encoder, grant.policy_reference);
}

Result<DelegationGrant> get_grant(Decoder& decoder) {
  DelegationGrant grant;
  const auto id = get_identifier<DelegationTag>(decoder);
  if (!id) {
    return id.error();
  }
  grant.id = id.value();
  const auto grantee = get_identifier<SiteTag>(decoder);
  if (!grantee) {
    return grantee.error();
  }
  grant.grantee = grantee.value();
  const auto scope_mask = decoder.u16();
  if (!scope_mask) {
    return scope_mask.error();
  }
  grant.scope_mask = scope_mask.value();
  auto selectors = get_list<std::string>(decoder, [](Decoder& source) { return source.text(); });
  if (!selectors) {
    return selectors.error();
  }
  grant.selectors = std::move(selectors).value();
  const auto granted_at = decoder.u64();
  if (!granted_at) {
    return granted_at.error();
  }
  const auto membership = decoder.u64();
  if (!membership) {
    return membership.error();
  }
  const auto expires = decoder.u64();
  if (!expires) {
    return expires.error();
  }
  const auto expires_wall = decoder.i64();
  if (!expires_wall) {
    return expires_wall.error();
  }
  const auto survives = decoder.boolean();
  if (!survives) {
    return survives.error();
  }
  const auto exclusive = decoder.boolean();
  if (!exclusive) {
    return exclusive.error();
  }
  const auto policy = get_digest(decoder);
  if (!policy) {
    return policy.error();
  }
  grant.granted_at = FederationGeneration{granted_at.value()};
  grant.grantee_membership_generation = MembershipGeneration{membership.value()};
  grant.expires_at = FederationGeneration{expires.value()};
  grant.expires_at_wall = UnixMillis{expires_wall.value()};
  grant.survives_partition = survives.value();
  grant.exclusive = exclusive.value();
  grant.policy_reference = policy.value();
  return grant;
}

void put(Encoder& encoder, const SiteReport& report) {
  put(encoder, report.site);
  encoder.u64(report.accepted.value());
  encoder.u64(report.local_epoch.value());
  put(encoder, report.accepted_history);
  put(encoder, report.local_history);
  encoder.u64(report.membership_generation.value());
  put_list(encoder, report.held_delegations,
           [](Encoder& target, const DelegationId& id) { put(target, id); });
  put_list(encoder, report.observed_generations,
           [](Encoder& target, const FederationGeneration& generation) {
             target.u64(generation.value());
           });
}

Result<SiteReport> get_report(Decoder& decoder) {
  SiteReport report;
  const auto site = get_identifier<SiteTag>(decoder);
  if (!site) {
    return site.error();
  }
  report.site = site.value();
  const auto accepted = decoder.u64();
  if (!accepted) {
    return accepted.error();
  }
  const auto epoch = decoder.u64();
  if (!epoch) {
    return epoch.error();
  }
  const auto accepted_history = get_digest(decoder);
  if (!accepted_history) {
    return accepted_history.error();
  }
  const auto local_history = get_digest(decoder);
  if (!local_history) {
    return local_history.error();
  }
  const auto membership = decoder.u64();
  if (!membership) {
    return membership.error();
  }
  report.accepted = AcceptedGeneration{accepted.value()};
  report.local_epoch = SiteLocalEpoch{epoch.value()};
  report.accepted_history = accepted_history.value();
  report.local_history = local_history.value();
  report.membership_generation = MembershipGeneration{membership.value()};

  auto held = get_list<DelegationId>(
      decoder, [](Decoder& source) { return get_identifier<DelegationTag>(source); });
  if (!held) {
    return held.error();
  }
  report.held_delegations = std::move(held).value();

  auto observed = get_list<FederationGeneration>(
      decoder, [](Decoder& source) -> Result<FederationGeneration> {
        const auto value = source.u64();
        if (!value) {
          return value.error();
        }
        return FederationGeneration{value.value()};
      });
  if (!observed) {
    return observed.error();
  }
  report.observed_generations = std::move(observed).value();
  return report;
}

void put(Encoder& encoder, MembershipState state) {
  encoder.u8(static_cast<std::uint8_t>(state));
}

Result<MembershipState> get_membership_state(Decoder& decoder) {
  return get_enum<MembershipState>(decoder, static_cast<std::uint8_t>(MembershipState::RefusedAuthority),
                                   "membership state");
}

void put(Encoder& encoder, LinkState state) { encoder.u8(static_cast<std::uint8_t>(state)); }

Result<LinkState> get_link_state(Decoder& decoder) {
  return get_enum<LinkState>(decoder, static_cast<std::uint8_t>(LinkState::Partitioned), "link state");
}

void put(Encoder& encoder, ReconciliationOutcome outcome) {
  encoder.u8(static_cast<std::uint8_t>(outcome));
}

Result<ReconciliationOutcome> get_reconciliation_outcome(Decoder& decoder) {
  return get_enum<ReconciliationOutcome>(
      decoder, static_cast<std::uint8_t>(ReconciliationOutcome::Indeterminate),
      "reconciliation outcome");
}

void put(Encoder& encoder, ConflictResolution::Choice choice) {
  encoder.u8(static_cast<std::uint8_t>(choice));
}

Result<ConflictResolution::Choice> get_conflict_choice(Decoder& decoder) {
  return get_enum<ConflictResolution::Choice>(
      decoder, static_cast<std::uint8_t>(ConflictResolution::Choice::ReissueExclusiveToSingleHolder),
      "conflict resolution choice");
}

void put(Encoder& encoder, ErrorCode code) { encoder.u8(static_cast<std::uint8_t>(code)); }

Result<ErrorCode> get_error_code(Decoder& decoder) {
  return get_enum<ErrorCode>(decoder, static_cast<std::uint8_t>(ErrorCode::Internal), "error code");
}

void put(Encoder& encoder, AdmissionDecision decision) {
  encoder.u8(static_cast<std::uint8_t>(decision));
}

Result<AdmissionDecision> get_admission_decision(Decoder& decoder) {
  return get_enum<AdmissionDecision>(decoder, static_cast<std::uint8_t>(AdmissionDecision::Refused),
                                     "admission decision");
}

void put(Encoder& encoder, CompatibilityFindingCode code) {
  encoder.u8(static_cast<std::uint8_t>(code));
}

Result<CompatibilityFindingCode> get_finding_code(Decoder& decoder) {
  return get_enum<CompatibilityFindingCode>(
      decoder, static_cast<std::uint8_t>(CompatibilityFindingCode::CapabilitySetTooLarge),
      "compatibility finding code");
}

void put(Encoder& encoder, DivergenceKind kind) { encoder.u8(static_cast<std::uint8_t>(kind)); }

Result<DivergenceKind> get_divergence_kind(Decoder& decoder) {
  return get_enum<DivergenceKind>(
      decoder, static_cast<std::uint8_t>(DivergenceKind::ExclusiveGrantDoubleHeld), "divergence kind");
}

void put(Encoder& encoder, const CompatibilityFinding& finding) {
  put(encoder, finding.code);
  encoder.text(finding.subject.value);
  encoder.text(finding.detail);
}

Result<CompatibilityFinding> get_compatibility_finding(Decoder& decoder) {
  CompatibilityFinding finding;
  const auto code = get_finding_code(decoder);
  if (!code) {
    return code.error();
  }
  finding.code = code.value();
  const auto subject = decoder.text();
  if (!subject) {
    return subject.error();
  }
  finding.subject = CapabilityId{subject.value()};
  const auto detail = decoder.text();
  if (!detail) {
    return detail.error();
  }
  finding.detail = detail.value();
  return finding;
}

void put(Encoder& encoder, const CompatibilityReport& report) {
  put(encoder, report.decision);
  put_list(encoder, report.findings, [](Encoder& target, const CompatibilityFinding& finding) {
    put(target, finding);
  });
  put(encoder, report.declaration);
}

Result<CompatibilityReport> get_compatibility_report(Decoder& decoder) {
  CompatibilityReport report;
  const auto decision = get_admission_decision(decoder);
  if (!decision) {
    return decision.error();
  }
  report.decision = decision.value();
  auto findings = get_list<CompatibilityFinding>(decoder, get_compatibility_finding);
  if (!findings) {
    return findings.error();
  }
  report.findings = std::move(findings).value();
  const auto declaration = get_digest(decoder);
  if (!declaration) {
    return declaration.error();
  }
  report.declaration = declaration.value();
  return report;
}

void put(Encoder& encoder, const MembershipRecord& record) {
  put(encoder, record.site);
  encoder.text(record.display_name);
  put(encoder, record.state);
  encoder.u64(record.generation.value());
  encoder.u64(record.entered_at.value());
  put(encoder, record.pre_partition_state);
  encoder.u64(record.accepted.value());
  encoder.u64(record.local_epoch.value());
  put(encoder, record.accepted_history);
  encoder.i64(record.last_contact.value);
  put(encoder, record.declaration);
  put(encoder, record.declaration_digest);
  put(encoder, record.admission);
  encoder.u16(record.constrained_scopes);
  put(encoder, record.link);
  encoder.text(record.transition_reason);
}

Result<MembershipRecord> get_membership_record(Decoder& decoder) {
  MembershipRecord record;
  const auto site = get_identifier<SiteTag>(decoder);
  if (!site) {
    return site.error();
  }
  record.site = site.value();
  const auto name = decoder.text();
  if (!name) {
    return name.error();
  }
  record.display_name = name.value();
  const auto state = get_membership_state(decoder);
  if (!state) {
    return state.error();
  }
  record.state = state.value();
  const auto generation = decoder.u64();
  if (!generation) {
    return generation.error();
  }
  const auto entered = decoder.u64();
  if (!entered) {
    return entered.error();
  }
  const auto pre = get_membership_state(decoder);
  if (!pre) {
    return pre.error();
  }
  const auto accepted = decoder.u64();
  if (!accepted) {
    return accepted.error();
  }
  const auto epoch = decoder.u64();
  if (!epoch) {
    return epoch.error();
  }
  const auto accepted_history = get_digest(decoder);
  if (!accepted_history) {
    return accepted_history.error();
  }
  const auto contact = decoder.i64();
  if (!contact) {
    return contact.error();
  }
  const auto declaration = get_declaration(decoder);
  if (!declaration) {
    return declaration.error();
  }
  const auto declaration_digest_value = get_digest(decoder);
  if (!declaration_digest_value) {
    return declaration_digest_value.error();
  }
  const auto admission = get_compatibility_report(decoder);
  if (!admission) {
    return admission.error();
  }
  const auto scopes = decoder.u16();
  if (!scopes) {
    return scopes.error();
  }
  const auto link = get_link_state(decoder);
  if (!link) {
    return link.error();
  }
  const auto reason = decoder.text();
  if (!reason) {
    return reason.error();
  }
  record.generation = MembershipGeneration{generation.value()};
  record.entered_at = FederationGeneration{entered.value()};
  record.pre_partition_state = pre.value();
  record.accepted = AcceptedGeneration{accepted.value()};
  record.local_epoch = SiteLocalEpoch{epoch.value()};
  record.accepted_history = accepted_history.value();
  record.last_contact = UnixMillis{contact.value()};
  record.declaration = declaration.value();
  record.declaration_digest = declaration_digest_value.value();
  record.admission = admission.value();
  record.constrained_scopes = scopes.value();
  record.link = link.value();
  record.transition_reason = reason.value();
  return record;
}

void put(Encoder& encoder, const DelegationRecord& record) {
  put(encoder, record.grant);
  encoder.boolean(record.revoked);
  encoder.u64(record.revoked_at.value());
  encoder.text(record.revocation_reason);
}

Result<DelegationRecord> get_delegation_record(Decoder& decoder) {
  DelegationRecord record;
  const auto grant = get_grant(decoder);
  if (!grant) {
    return grant.error();
  }
  record.grant = grant.value();
  const auto revoked = decoder.boolean();
  if (!revoked) {
    return revoked.error();
  }
  const auto revoked_at = decoder.u64();
  if (!revoked_at) {
    return revoked_at.error();
  }
  const auto reason = decoder.text();
  if (!reason) {
    return reason.error();
  }
  record.revoked = revoked.value();
  record.revoked_at = FederationGeneration{revoked_at.value()};
  record.revocation_reason = reason.value();
  return record;
}

void put(Encoder& encoder, const DivergenceRecord& record) {
  put(encoder, record.kind);
  put(encoder, record.delegation);
  put(encoder, record.subject);
  encoder.u64(record.federation_generation.value());
  encoder.u64(record.membership_generation.value());
  encoder.text(record.detail);
}

Result<DivergenceRecord> get_divergence_record(Decoder& decoder) {
  DivergenceRecord record;
  const auto kind = get_divergence_kind(decoder);
  if (!kind) {
    return kind.error();
  }
  record.kind = kind.value();
  const auto delegation = get_identifier<DelegationTag>(decoder);
  if (!delegation) {
    return delegation.error();
  }
  record.delegation = delegation.value();
  const auto subject = get_identifier<SiteTag>(decoder);
  if (!subject) {
    return subject.error();
  }
  record.subject = subject.value();
  const auto federation_generation = decoder.u64();
  if (!federation_generation) {
    return federation_generation.error();
  }
  const auto membership_generation = decoder.u64();
  if (!membership_generation) {
    return membership_generation.error();
  }
  const auto detail = decoder.text();
  if (!detail) {
    return detail.error();
  }
  record.federation_generation = FederationGeneration{federation_generation.value()};
  record.membership_generation = MembershipGeneration{membership_generation.value()};
  record.detail = detail.value();
  return record;
}

void put(Encoder& encoder, const ReconciliationRecord& record) {
  put(encoder, record.id);
  put(encoder, record.site);
  encoder.u64(record.federation_generation.value());
  encoder.u64(record.site_reported_generation.value());
  put(encoder, record.federation_history);
  put(encoder, record.site_history);
  put(encoder, record.outcome);
  put_list(encoder, record.divergences,
           [](Encoder& target, const DivergenceRecord& divergence) { put(target, divergence); });
  encoder.boolean(record.resolved);
  encoder.text(record.resolution);
  encoder.u64(record.committed.value());
  encoder.i64(record.observed_at.value);
}

Result<ReconciliationRecord> get_reconciliation_record(Decoder& decoder) {
  ReconciliationRecord record;
  const auto id = get_identifier<ReconciliationTag>(decoder);
  if (!id) {
    return id.error();
  }
  record.id = id.value();
  const auto site = get_identifier<SiteTag>(decoder);
  if (!site) {
    return site.error();
  }
  record.site = site.value();
  const auto federation_generation = decoder.u64();
  if (!federation_generation) {
    return federation_generation.error();
  }
  const auto reported = decoder.u64();
  if (!reported) {
    return reported.error();
  }
  const auto federation_history = get_digest(decoder);
  if (!federation_history) {
    return federation_history.error();
  }
  const auto site_history = get_digest(decoder);
  if (!site_history) {
    return site_history.error();
  }
  const auto outcome = get_reconciliation_outcome(decoder);
  if (!outcome) {
    return outcome.error();
  }
  auto divergences = get_list<DivergenceRecord>(decoder, get_divergence_record);
  if (!divergences) {
    return divergences.error();
  }
  const auto resolved = decoder.boolean();
  if (!resolved) {
    return resolved.error();
  }
  const auto resolution = decoder.text();
  if (!resolution) {
    return resolution.error();
  }
  const auto committed = decoder.u64();
  if (!committed) {
    return committed.error();
  }
  const auto observed = decoder.i64();
  if (!observed) {
    return observed.error();
  }
  record.federation_generation = FederationGeneration{federation_generation.value()};
  record.site_reported_generation = AcceptedGeneration{reported.value()};
  record.federation_history = federation_history.value();
  record.site_history = site_history.value();
  record.outcome = outcome.value();
  record.divergences = std::move(divergences).value();
  record.resolved = resolved.value();
  record.resolution = resolution.value();
  record.committed = JournalSequence{committed.value()};
  record.observed_at = UnixMillis{observed.value()};
  return record;
}

void put(Encoder& encoder, const IdempotencyReceipt& receipt) {
  put(encoder, receipt.key.federation);
  put(encoder, receipt.key.site);
  put(encoder, receipt.key.operation);
  put(encoder, receipt.request);
  put(encoder, receipt.outcome);
  encoder.text(receipt.outcome_detail);
  encoder.u64(receipt.generation.value());
  encoder.u64(receipt.membership_generation.value());
  encoder.u64(receipt.committed.value());
  encoder.i64(receipt.recorded_at.value);
}

Result<IdempotencyReceipt> get_receipt(Decoder& decoder) {
  IdempotencyReceipt receipt;
  const auto federation = get_identifier<FederationTag>(decoder);
  if (!federation) {
    return federation.error();
  }
  const auto site = get_identifier<SiteTag>(decoder);
  if (!site) {
    return site.error();
  }
  const auto operation = get_identifier<OperationTag>(decoder);
  if (!operation) {
    return operation.error();
  }
  const auto request = get_digest(decoder);
  if (!request) {
    return request.error();
  }
  const auto outcome = get_error_code(decoder);
  if (!outcome) {
    return outcome.error();
  }
  const auto detail = decoder.text();
  if (!detail) {
    return detail.error();
  }
  const auto generation = decoder.u64();
  if (!generation) {
    return generation.error();
  }
  const auto membership = decoder.u64();
  if (!membership) {
    return membership.error();
  }
  const auto committed = decoder.u64();
  if (!committed) {
    return committed.error();
  }
  const auto recorded = decoder.i64();
  if (!recorded) {
    return recorded.error();
  }
  receipt.key = IdempotencyKey{federation.value(), site.value(), operation.value()};
  receipt.request = request.value();
  receipt.outcome = outcome.value();
  receipt.outcome_detail = detail.value();
  receipt.generation = FederationGeneration{generation.value()};
  receipt.membership_generation = MembershipGeneration{membership.value()};
  receipt.committed = JournalSequence{committed.value()};
  receipt.recorded_at = UnixMillis{recorded.value()};
  return receipt;
}

namespace {

constexpr std::uint8_t kMutationGenesis = 0;
constexpr std::uint8_t kMutationSiteRegistered = 1;
constexpr std::uint8_t kMutationCompatibilityEvaluated = 2;
constexpr std::uint8_t kMutationMembershipTransition = 3;
constexpr std::uint8_t kMutationDelegationGranted = 4;
constexpr std::uint8_t kMutationDelegationRevoked = 5;
constexpr std::uint8_t kMutationWindowDeclared = 6;
constexpr std::uint8_t kMutationConnectivityChanged = 7;
constexpr std::uint8_t kMutationSiteReportedState = 8;
constexpr std::uint8_t kMutationReconciliationRecorded = 9;
constexpr std::uint8_t kMutationConflictResolved = 10;

}  // namespace

void put(Encoder& encoder, const MutationPayload& payload) {
  std::visit(
      [&encoder](const auto& mutation) {
        using T = std::decay_t<decltype(mutation)>;
        if constexpr (std::is_same_v<T, GenesisMutation>) {
          encoder.u8(kMutationGenesis);
          put(encoder, mutation.federation);
          encoder.i64(mutation.created.value);
        } else if constexpr (std::is_same_v<T, SiteRegisteredMutation>) {
          encoder.u8(kMutationSiteRegistered);
          put(encoder, mutation.record);
        } else if constexpr (std::is_same_v<T, CompatibilityEvaluatedMutation>) {
          encoder.u8(kMutationCompatibilityEvaluated);
          put(encoder, mutation.site);
          put(encoder, mutation.resulting_state);
          encoder.u16(mutation.constrained_scopes);
          put(encoder, mutation.report);
        } else if constexpr (std::is_same_v<T, MembershipTransitionMutation>) {
          encoder.u8(kMutationMembershipTransition);
          put(encoder, mutation.site);
          put(encoder, mutation.to);
          put(encoder, mutation.pre_partition_state);
          encoder.u16(mutation.constrained_scopes);
          encoder.text(mutation.reason);
        } else if constexpr (std::is_same_v<T, DelegationGrantedMutation>) {
          encoder.u8(kMutationDelegationGranted);
          put(encoder, mutation.grant);
        } else if constexpr (std::is_same_v<T, DelegationRevokedMutation>) {
          encoder.u8(kMutationDelegationRevoked);
          put(encoder, mutation.revocation.id);
          encoder.u64(mutation.revocation.revoked_at.value());
          encoder.text(mutation.revocation.reason);
        } else if constexpr (std::is_same_v<T, CompatibilityWindowDeclaredMutation>) {
          encoder.u8(kMutationWindowDeclared);
          put(encoder, mutation.window);
        } else if constexpr (std::is_same_v<T, ConnectivityChangedMutation>) {
          encoder.u8(kMutationConnectivityChanged);
          put(encoder, mutation.site);
          put(encoder, mutation.link);
        } else if constexpr (std::is_same_v<T, SiteReportedStateMutation>) {
          encoder.u8(kMutationSiteReportedState);
          put(encoder, mutation.site);
          encoder.u64(mutation.accepted.value());
          encoder.u64(mutation.local_epoch.value());
          put(encoder, mutation.accepted_history);
          encoder.i64(mutation.contact.value);
        } else if constexpr (std::is_same_v<T, ReconciliationRecordedMutation>) {
          encoder.u8(kMutationReconciliationRecorded);
          put(encoder, mutation.record);
        } else {
          encoder.u8(kMutationConflictResolved);
          put(encoder, mutation.reconciliation);
          put(encoder, mutation.choice);
          encoder.text(mutation.authority_reference);
        }
      },
      payload);
}

Result<MutationPayload> get_mutation(Decoder& decoder) {
  const auto tag = decoder.u8();
  if (!tag) {
    return tag.error();
  }
  switch (tag.value()) {
    case kMutationGenesis: {
      GenesisMutation mutation;
      const auto federation = get_identifier<FederationTag>(decoder);
      if (!federation) {
        return federation.error();
      }
      const auto created = decoder.i64();
      if (!created) {
        return created.error();
      }
      mutation.federation = federation.value();
      mutation.created = UnixMillis{created.value()};
      return MutationPayload{mutation};
    }
    case kMutationSiteRegistered: {
      const auto record = get_membership_record(decoder);
      if (!record) {
        return record.error();
      }
      return MutationPayload{SiteRegisteredMutation{record.value()}};
    }
    case kMutationCompatibilityEvaluated: {
      CompatibilityEvaluatedMutation mutation;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      mutation.site = site.value();
      const auto state = get_membership_state(decoder);
      if (!state) {
        return state.error();
      }
      mutation.resulting_state = state.value();
      const auto scopes = decoder.u16();
      if (!scopes) {
        return scopes.error();
      }
      mutation.constrained_scopes = scopes.value();
      const auto report = get_compatibility_report(decoder);
      if (!report) {
        return report.error();
      }
      mutation.report = report.value();
      return MutationPayload{mutation};
    }
    case kMutationMembershipTransition: {
      MembershipTransitionMutation mutation;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      mutation.site = site.value();
      const auto to = get_membership_state(decoder);
      if (!to) {
        return to.error();
      }
      mutation.to = to.value();
      const auto pre = get_membership_state(decoder);
      if (!pre) {
        return pre.error();
      }
      mutation.pre_partition_state = pre.value();
      const auto scopes = decoder.u16();
      if (!scopes) {
        return scopes.error();
      }
      mutation.constrained_scopes = scopes.value();
      const auto reason = decoder.text();
      if (!reason) {
        return reason.error();
      }
      mutation.reason = reason.value();
      return MutationPayload{mutation};
    }
    case kMutationDelegationGranted: {
      const auto grant = get_grant(decoder);
      if (!grant) {
        return grant.error();
      }
      return MutationPayload{DelegationGrantedMutation{grant.value()}};
    }
    case kMutationDelegationRevoked: {
      DelegationRevokedMutation mutation;
      const auto id = get_identifier<DelegationTag>(decoder);
      if (!id) {
        return id.error();
      }
      const auto revoked_at = decoder.u64();
      if (!revoked_at) {
        return revoked_at.error();
      }
      const auto reason = decoder.text();
      if (!reason) {
        return reason.error();
      }
      mutation.revocation.id = id.value();
      mutation.revocation.revoked_at = FederationGeneration{revoked_at.value()};
      mutation.revocation.reason = reason.value();
      return MutationPayload{mutation};
    }
    case kMutationWindowDeclared: {
      const auto window = get_window(decoder);
      if (!window) {
        return window.error();
      }
      return MutationPayload{CompatibilityWindowDeclaredMutation{window.value()}};
    }
    case kMutationConnectivityChanged: {
      ConnectivityChangedMutation mutation;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      mutation.site = site.value();
      const auto link = get_link_state(decoder);
      if (!link) {
        return link.error();
      }
      mutation.link = link.value();
      return MutationPayload{mutation};
    }
    case kMutationSiteReportedState: {
      SiteReportedStateMutation mutation;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      mutation.site = site.value();
      const auto accepted = decoder.u64();
      if (!accepted) {
        return accepted.error();
      }
      const auto epoch = decoder.u64();
      if (!epoch) {
        return epoch.error();
      }
      const auto history = get_digest(decoder);
      if (!history) {
        return history.error();
      }
      const auto contact = decoder.i64();
      if (!contact) {
        return contact.error();
      }
      mutation.accepted = AcceptedGeneration{accepted.value()};
      mutation.local_epoch = SiteLocalEpoch{epoch.value()};
      mutation.accepted_history = history.value();
      mutation.contact = UnixMillis{contact.value()};
      return MutationPayload{mutation};
    }
    case kMutationReconciliationRecorded: {
      const auto record = get_reconciliation_record(decoder);
      if (!record) {
        return record.error();
      }
      return MutationPayload{ReconciliationRecordedMutation{record.value()}};
    }
    case kMutationConflictResolved: {
      ConflictResolvedMutation mutation;
      const auto reconciliation = get_identifier<ReconciliationTag>(decoder);
      if (!reconciliation) {
        return reconciliation.error();
      }
      mutation.reconciliation = reconciliation.value();
      const auto choice = get_conflict_choice(decoder);
      if (!choice) {
        return choice.error();
      }
      mutation.choice = choice.value();
      const auto reference = decoder.text();
      if (!reference) {
        return reference.error();
      }
      mutation.authority_reference = reference.value();
      return MutationPayload{mutation};
    }
    default:
      return make_error(ErrorCode::MalformedInput,
                        "mutation tag " + std::to_string(tag.value()) + " is not a known mutation");
  }
}

void put(Encoder& encoder, const JournalEntry& entry) {
  encoder.u64(entry.sequence.value());
  encoder.u64(entry.generation.value());
  encoder.i64(entry.recorded_at.value);
  put_list(encoder, entry.changes,
           [](Encoder& target, const MutationPayload& payload) { put(target, payload); });
  encoder.boolean(entry.receipt.has_value());
  if (entry.receipt.has_value()) {
    put(encoder, *entry.receipt);
  }
}

Result<JournalEntry> get_journal_entry(Decoder& decoder) {
  JournalEntry entry;
  const auto sequence = decoder.u64();
  if (!sequence) {
    return sequence.error();
  }
  const auto generation = decoder.u64();
  if (!generation) {
    return generation.error();
  }
  const auto recorded = decoder.i64();
  if (!recorded) {
    return recorded.error();
  }
  entry.sequence = JournalSequence{sequence.value()};
  entry.generation = FederationGeneration{generation.value()};
  entry.recorded_at = UnixMillis{recorded.value()};

  auto changes = get_list<MutationPayload>(decoder, get_mutation);
  if (!changes) {
    return changes.error();
  }
  entry.changes = std::move(changes).value();

  const auto has_receipt = decoder.boolean();
  if (!has_receipt) {
    return has_receipt.error();
  }
  if (has_receipt.value()) {
    const auto receipt = get_receipt(decoder);
    if (!receipt) {
      return receipt.error();
    }
    entry.receipt = receipt.value();
  }

  if (entry.changes.empty() && !entry.receipt.has_value()) {
    return make_error(ErrorCode::MalformedInput,
                      "journal entry carries neither a change nor a receipt");
  }
  return entry;
}

void put(Encoder& encoder, const Command& command) {
  std::visit(
      [&encoder](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, RegisterSiteCommand>) {
          encoder.u8(0);
          put(encoder, payload.site);
          encoder.text(payload.display_name);
          put(encoder, payload.declaration);
        } else if constexpr (std::is_same_v<T, ValidateSiteCommand>) {
          encoder.u8(1);
          put(encoder, payload.site);
        } else if constexpr (std::is_same_v<T, AdmitSiteCommand>) {
          encoder.u8(2);
          put(encoder, payload.site);
          encoder.u64(payload.expected_generation.value());
        } else if constexpr (std::is_same_v<T, ActivateSiteCommand>) {
          encoder.u8(3);
          put(encoder, payload.site);
          encoder.u64(payload.accepted.value());
          put(encoder, payload.accepted_history);
          encoder.u64(payload.local_epoch.value());
        } else if constexpr (std::is_same_v<T, ConstrainSiteCommand>) {
          encoder.u8(4);
          put(encoder, payload.site);
          encoder.u16(payload.scopes);
          encoder.text(payload.reason);
        } else if constexpr (std::is_same_v<T, BeginDrainCommand>) {
          encoder.u8(5);
          put(encoder, payload.site);
          encoder.text(payload.reason);
        } else if constexpr (std::is_same_v<T, CancelDrainCommand>) {
          encoder.u8(6);
          put(encoder, payload.site);
          encoder.text(payload.reason);
        } else if constexpr (std::is_same_v<T, RemoveSiteCommand>) {
          encoder.u8(7);
          put(encoder, payload.site);
          encoder.text(payload.reason);
        } else if constexpr (std::is_same_v<T, SetConnectivityCommand>) {
          encoder.u8(8);
          put(encoder, payload.site);
          put(encoder, payload.link);
        } else if constexpr (std::is_same_v<T, DeclareWindowCommand>) {
          encoder.u8(9);
          put(encoder, payload.window);
        } else if constexpr (std::is_same_v<T, GrantDelegationCommand>) {
          encoder.u8(10);
          put(encoder, payload.grant);
        } else if constexpr (std::is_same_v<T, RevokeDelegationCommand>) {
          encoder.u8(11);
          put(encoder, payload.delegation);
          encoder.text(payload.reason);
        } else if constexpr (std::is_same_v<T, RejoinCommand>) {
          encoder.u8(12);
          put(encoder, payload.report);
          encoder.boolean(payload.claims_exclusive_conflict);
        } else if constexpr (std::is_same_v<T, ResolveConflictCommand>) {
          encoder.u8(13);
          put(encoder, payload.resolution.reconciliation);
          put(encoder, payload.resolution.choice);
          encoder.text(payload.resolution.authority_reference);
        } else {
          encoder.u8(14);
          put(encoder, payload.site);
          encoder.u64(payload.accepted.value());
          put(encoder, payload.accepted_history);
          encoder.u64(payload.local_epoch.value());
          encoder.i64(payload.at.value);
        }
      },
      command.payload);

  put(encoder, command.operation);
  put(encoder, command.origin);
  encoder.u64(command.origin_generation.value());
}

Result<Command> get_command(Decoder& decoder) {
  const auto tag = decoder.u8();
  if (!tag) {
    return tag.error();
  }
  Command command;
  switch (tag.value()) {
    case 0: {
      RegisterSiteCommand payload;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      payload.site = site.value();
      const auto name = decoder.text();
      if (!name) {
        return name.error();
      }
      payload.display_name = name.value();
      const auto declaration = get_declaration(decoder);
      if (!declaration) {
        return declaration.error();
      }
      payload.declaration = declaration.value();
      command.payload = std::move(payload);
      break;
    }
    case 1: {
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      command.payload = ValidateSiteCommand{site.value()};
      break;
    }
    case 2: {
      AdmitSiteCommand payload;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      payload.site = site.value();
      const auto generation = decoder.u64();
      if (!generation) {
        return generation.error();
      }
      payload.expected_generation = MembershipGeneration{generation.value()};
      command.payload = payload;
      break;
    }
    case 3: {
      ActivateSiteCommand payload;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      payload.site = site.value();
      const auto accepted = decoder.u64();
      if (!accepted) {
        return accepted.error();
      }
      const auto history = get_digest(decoder);
      if (!history) {
        return history.error();
      }
      const auto epoch = decoder.u64();
      if (!epoch) {
        return epoch.error();
      }
      payload.accepted = AcceptedGeneration{accepted.value()};
      payload.accepted_history = history.value();
      payload.local_epoch = SiteLocalEpoch{epoch.value()};
      command.payload = payload;
      break;
    }
    case 4: {
      ConstrainSiteCommand payload;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      payload.site = site.value();
      const auto scopes = decoder.u16();
      if (!scopes) {
        return scopes.error();
      }
      const auto reason = decoder.text();
      if (!reason) {
        return reason.error();
      }
      payload.scopes = scopes.value();
      payload.reason = reason.value();
      command.payload = payload;
      break;
    }
    case 5:
    case 6:
    case 7: {
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      const auto reason = decoder.text();
      if (!reason) {
        return reason.error();
      }
      if (tag.value() == 5) {
        command.payload = BeginDrainCommand{site.value(), reason.value()};
      } else if (tag.value() == 6) {
        command.payload = CancelDrainCommand{site.value(), reason.value()};
      } else {
        command.payload = RemoveSiteCommand{site.value(), reason.value()};
      }
      break;
    }
    case 8: {
      SetConnectivityCommand payload;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      payload.site = site.value();
      const auto link = get_link_state(decoder);
      if (!link) {
        return link.error();
      }
      payload.link = link.value();
      command.payload = payload;
      break;
    }
    case 9: {
      const auto window = get_window(decoder);
      if (!window) {
        return window.error();
      }
      command.payload = DeclareWindowCommand{window.value()};
      break;
    }
    case 10: {
      const auto grant = get_grant(decoder);
      if (!grant) {
        return grant.error();
      }
      command.payload = GrantDelegationCommand{grant.value()};
      break;
    }
    case 11: {
      RevokeDelegationCommand payload;
      const auto id = get_identifier<DelegationTag>(decoder);
      if (!id) {
        return id.error();
      }
      payload.delegation = id.value();
      const auto reason = decoder.text();
      if (!reason) {
        return reason.error();
      }
      payload.reason = reason.value();
      command.payload = payload;
      break;
    }
    case 12: {
      RejoinCommand payload;
      const auto report = get_report(decoder);
      if (!report) {
        return report.error();
      }
      payload.report = report.value();
      const auto claims = decoder.boolean();
      if (!claims) {
        return claims.error();
      }
      payload.claims_exclusive_conflict = claims.value();
      command.payload = std::move(payload);
      break;
    }
    case 13: {
      ResolveConflictCommand payload;
      const auto id = get_identifier<ReconciliationTag>(decoder);
      if (!id) {
        return id.error();
      }
      payload.resolution.reconciliation = id.value();
      const auto choice = get_conflict_choice(decoder);
      if (!choice) {
        return choice.error();
      }
      payload.resolution.choice = choice.value();
      const auto reference = decoder.text();
      if (!reference) {
        return reference.error();
      }
      payload.resolution.authority_reference = reference.value();
      command.payload = std::move(payload);
      break;
    }
    case 14: {
      RecordContactCommand payload;
      const auto site = get_identifier<SiteTag>(decoder);
      if (!site) {
        return site.error();
      }
      payload.site = site.value();
      const auto accepted = decoder.u64();
      if (!accepted) {
        return accepted.error();
      }
      const auto history = get_digest(decoder);
      if (!history) {
        return history.error();
      }
      const auto epoch = decoder.u64();
      if (!epoch) {
        return epoch.error();
      }
      const auto at = decoder.i64();
      if (!at) {
        return at.error();
      }
      payload.accepted = AcceptedGeneration{accepted.value()};
      payload.accepted_history = history.value();
      payload.local_epoch = SiteLocalEpoch{epoch.value()};
      payload.at = UnixMillis{at.value()};
      command.payload = payload;
      break;
    }
    default:
      return make_error(ErrorCode::MalformedInput,
                        "command tag " + std::to_string(tag.value()) + " is not a known command");
  }

  const auto operation = get_identifier<OperationTag>(decoder);
  if (!operation) {
    return operation.error();
  }
  command.operation = operation.value();
  const auto origin = get_identifier<SiteTag>(decoder);
  if (!origin) {
    return origin.error();
  }
  command.origin = origin.value();
  const auto generation = decoder.u64();
  if (!generation) {
    return generation.error();
  }
  command.origin_generation = FederationGeneration{generation.value()};
  return command;
}

}  // namespace dcf::detail
