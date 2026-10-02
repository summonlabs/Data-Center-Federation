// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Internal record codec. The durable journal, the wire protocol, and the state
// digest all describe the same records, so they all use these functions. Keeping
// one implementation is what makes a digest computed over stored state and a
// digest computed over a message that carries the same state agree by
// construction; two implementations would be two chances to disagree.
#pragma once

#include <cstdint>

#include "dcf/codec.hpp"
#include "dcf/command.hpp"
#include "dcf/state.hpp"

namespace dcf::detail {

template <class Tag>
inline void put(Encoder& encoder, const Identifier<Tag>& id) {
  encoder.u64(id.high);
  encoder.u64(id.low);
}

template <class Tag>
[[nodiscard]] inline Result<Identifier<Tag>> get_identifier(Decoder& decoder) {
  const auto high = decoder.u64();
  if (!high) {
    return high.error();
  }
  const auto low = decoder.u64();
  if (!low) {
    return low.error();
  }
  return Identifier<Tag>{high.value(), low.value()};
}

inline void put(Encoder& encoder, const Digest& digest) {
  encoder.raw(std::span<const std::uint8_t>(digest.bytes.data(), digest.bytes.size()));
}

[[nodiscard]] inline Result<Digest> get_digest(Decoder& decoder) {
  const auto raw = decoder.raw(32);
  if (!raw) {
    return raw.error();
  }
  Digest digest;
  for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
    digest.bytes[index] = raw.value()[index];
  }
  return digest;
}

void put(Encoder& encoder, const VersionRange& range);
[[nodiscard]] Result<VersionRange> get_version_range(Decoder& decoder);

void put(Encoder& encoder, const CompatibilityDeclaration& declaration);
[[nodiscard]] Result<CompatibilityDeclaration> get_declaration(Decoder& decoder);

void put(Encoder& encoder, const CompatibilityWindow& window);
[[nodiscard]] Result<CompatibilityWindow> get_window(Decoder& decoder);

void put(Encoder& encoder, const DelegationGrant& grant);
[[nodiscard]] Result<DelegationGrant> get_grant(Decoder& decoder);

void put(Encoder& encoder, const SiteReport& report);
[[nodiscard]] Result<SiteReport> get_report(Decoder& decoder);

void put(Encoder& encoder, MembershipState state);
[[nodiscard]] Result<MembershipState> get_membership_state(Decoder& decoder);
void put(Encoder& encoder, LinkState state);
[[nodiscard]] Result<LinkState> get_link_state(Decoder& decoder);
void put(Encoder& encoder, ReconciliationOutcome outcome);
[[nodiscard]] Result<ReconciliationOutcome> get_reconciliation_outcome(Decoder& decoder);
void put(Encoder& encoder, ConflictResolution::Choice choice);
[[nodiscard]] Result<ConflictResolution::Choice> get_conflict_choice(Decoder& decoder);
void put(Encoder& encoder, ErrorCode code);
[[nodiscard]] Result<ErrorCode> get_error_code(Decoder& decoder);
void put(Encoder& encoder, CapabilityRequirement requirement);
[[nodiscard]] Result<CapabilityRequirement> get_capability_requirement(Decoder& decoder);
void put(Encoder& encoder, AdmissionDecision decision);
[[nodiscard]] Result<AdmissionDecision> get_admission_decision(Decoder& decoder);
void put(Encoder& encoder, CompatibilityFindingCode code);
[[nodiscard]] Result<CompatibilityFindingCode> get_finding_code(Decoder& decoder);
void put(Encoder& encoder, DivergenceKind kind);
[[nodiscard]] Result<DivergenceKind> get_divergence_kind(Decoder& decoder);

void put(Encoder& encoder, const CompatibilityFinding& finding);
[[nodiscard]] Result<CompatibilityFinding> get_compatibility_finding(Decoder& decoder);
void put(Encoder& encoder, const CompatibilityReport& report);
[[nodiscard]] Result<CompatibilityReport> get_compatibility_report(Decoder& decoder);

void put(Encoder& encoder, const MembershipRecord& record);
[[nodiscard]] Result<MembershipRecord> get_membership_record(Decoder& decoder);

void put(Encoder& encoder, const DelegationRecord& record);
[[nodiscard]] Result<DelegationRecord> get_delegation_record(Decoder& decoder);

void put(Encoder& encoder, const DivergenceRecord& record);
[[nodiscard]] Result<DivergenceRecord> get_divergence_record(Decoder& decoder);

void put(Encoder& encoder, const ReconciliationRecord& record);
[[nodiscard]] Result<ReconciliationRecord> get_reconciliation_record(Decoder& decoder);

void put(Encoder& encoder, const IdempotencyReceipt& receipt);
[[nodiscard]] Result<IdempotencyReceipt> get_receipt(Decoder& decoder);

void put(Encoder& encoder, const MutationPayload& payload);
[[nodiscard]] Result<MutationPayload> get_mutation(Decoder& decoder);

void put(Encoder& encoder, const JournalEntry& entry);
[[nodiscard]] Result<JournalEntry> get_journal_entry(Decoder& decoder);

void put(Encoder& encoder, const Command& command);
[[nodiscard]] Result<Command> get_command(Decoder& decoder);

}  // namespace dcf::detail
