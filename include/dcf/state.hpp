// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dcf/codec.hpp"
#include "dcf/command.hpp"
#include "dcf/delegation.hpp"
#include "dcf/membership.hpp"
#include "dcf/receipt.hpp"
#include "dcf/reconciliation.hpp"
#include "dcf/types.hpp"

namespace dcf {

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------
// A mutation is an authoritative fact that has already been decided. Mutations
// are the only thing that changes state, they are serialized into the journal
// verbatim, and replaying them reproduces the state that produced them.
struct GenesisMutation {
  FederationId federation{};
  UnixMillis created{};

  friend bool operator==(const GenesisMutation&, const GenesisMutation&) = default;
};

struct SiteRegisteredMutation {
  MembershipRecord record{};

  friend bool operator==(const SiteRegisteredMutation&, const SiteRegisteredMutation&) = default;
};

struct CompatibilityEvaluatedMutation {
  SiteId site{};
  MembershipState resulting_state{MembershipState::Candidate};
  std::uint16_t constrained_scopes{0};
  CompatibilityReport report{};

  friend bool operator==(const CompatibilityEvaluatedMutation&,
                         const CompatibilityEvaluatedMutation&) = default;
};

struct MembershipTransitionMutation {
  SiteId site{};
  MembershipState to{MembershipState::Candidate};
  // Only meaningful when "to" is Partitioned: the state to return to.
  MembershipState pre_partition_state{MembershipState::Active};
  std::uint16_t constrained_scopes{0};
  std::string reason{};

  friend bool operator==(const MembershipTransitionMutation&,
                         const MembershipTransitionMutation&) = default;
};

struct DelegationGrantedMutation {
  DelegationGrant grant{};

  friend bool operator==(const DelegationGrantedMutation&,
                         const DelegationGrantedMutation&) = default;
};

struct DelegationRevokedMutation {
  DelegationRevocation revocation{};

  friend bool operator==(const DelegationRevokedMutation&,
                         const DelegationRevokedMutation&) = default;
};

struct CompatibilityWindowDeclaredMutation {
  CompatibilityWindow window{};

  friend bool operator==(const CompatibilityWindowDeclaredMutation&,
                         const CompatibilityWindowDeclaredMutation&) = default;
};

struct ConnectivityChangedMutation {
  SiteId site{};
  LinkState link{LinkState::Unknown};

  friend bool operator==(const ConnectivityChangedMutation&,
                         const ConnectivityChangedMutation&) = default;
};

// Observation only: it records what a site reported about itself and never
// changes authority, so it does not advance the federation generation.
struct SiteReportedStateMutation {
  SiteId site{};
  AcceptedGeneration accepted{};
  SiteLocalEpoch local_epoch{};
  Digest accepted_history{};
  UnixMillis contact{};

  friend bool operator==(const SiteReportedStateMutation&,
                         const SiteReportedStateMutation&) = default;
};

struct ReconciliationRecordedMutation {
  ReconciliationRecord record{};

  friend bool operator==(const ReconciliationRecordedMutation&,
                         const ReconciliationRecordedMutation&) = default;
};

struct ConflictResolvedMutation {
  ReconciliationId reconciliation{};
  ConflictResolution::Choice choice{ConflictResolution::Choice::FederationAuthoritative};
  std::string authority_reference{};

  friend bool operator==(const ConflictResolvedMutation&,
                         const ConflictResolvedMutation&) = default;
};

using MutationPayload =
    std::variant<GenesisMutation, SiteRegisteredMutation, CompatibilityEvaluatedMutation,
                 MembershipTransitionMutation, DelegationGrantedMutation, DelegationRevokedMutation,
                 CompatibilityWindowDeclaredMutation, ConnectivityChangedMutation,
                 SiteReportedStateMutation, ReconciliationRecordedMutation,
                 ConflictResolvedMutation>;

[[nodiscard]] std::string_view mutation_name(const MutationPayload& payload) noexcept;
// True when applying the mutation can change authority, in which case the entry
// that carries it advances the federation generation.
[[nodiscard]] bool mutation_changes_authority(const MutationPayload& payload) noexcept;

// ---------------------------------------------------------------------------
// Journal entries
// ---------------------------------------------------------------------------
// One durable record. The changes and the receipt commit together or not at all,
// so a receipt can never exist for a change that was not committed and a change
// can never be committed without the receipt that makes a retry safe.
struct JournalEntry {
  JournalSequence sequence{};
  FederationGeneration generation{};
  UnixMillis recorded_at{};
  std::vector<MutationPayload> changes{};
  std::optional<IdempotencyReceipt> receipt{};

  friend bool operator==(const JournalEntry&, const JournalEntry&) = default;
};

void encode(Encoder& encoder, const MutationPayload& payload);
[[nodiscard]] Result<MutationPayload> decode_mutation(Decoder& decoder);
void encode(Encoder& encoder, const JournalEntry& entry);
[[nodiscard]] Result<JournalEntry> decode_journal_entry(Decoder& decoder);

// ---------------------------------------------------------------------------
// Authoritative state
// ---------------------------------------------------------------------------
// Every table is ordered by its key, and every key has a total, canonical order,
// so iteration, serialization, and hashing are the same operation.
class FederationState {
 public:
  FederationState() = default;

  [[nodiscard]] static FederationState genesis(FederationId federation, UnixMillis created);

  [[nodiscard]] bool initialized() const noexcept { return !federation_.is_zero(); }
  [[nodiscard]] FederationId federation() const noexcept { return federation_; }
  [[nodiscard]] FederationGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] JournalSequence sequence() const noexcept { return sequence_; }
  [[nodiscard]] UnixMillis created() const noexcept { return created_; }

  [[nodiscard]] const std::map<SiteId, MembershipRecord>& sites() const noexcept { return sites_; }
  [[nodiscard]] const std::map<DelegationId, DelegationRecord>& delegations() const noexcept {
    return delegations_;
  }
  [[nodiscard]] const std::map<CapabilityId, CompatibilityWindow>& windows() const noexcept {
    return windows_;
  }
  [[nodiscard]] const std::map<ReconciliationId, ReconciliationRecord>& reconciliations()
      const noexcept {
    return reconciliations_;
  }
  [[nodiscard]] const std::map<IdempotencyKey, IdempotencyReceipt>& receipts() const noexcept {
    return receipts_;
  }

  [[nodiscard]] const MembershipRecord* find_site(const SiteId& site) const noexcept;
  [[nodiscard]] const DelegationRecord* find_delegation(const DelegationId& id) const noexcept;
  [[nodiscard]] const CompatibilityWindow* find_window(const CapabilityId& id) const noexcept;
  [[nodiscard]] const ReconciliationRecord* find_reconciliation(const ReconciliationId& id)
      const noexcept;
  [[nodiscard]] const IdempotencyReceipt* find_receipt(const IdempotencyKey& key) const noexcept;

  // Every delegation record whose grantee is the given site, ordered by
  // delegation identifier.
  [[nodiscard]] std::vector<DelegationRecord> delegations_for(const SiteId& site) const;
  [[nodiscard]] std::vector<CompatibilityWindow> window_list() const;
  // Delegations whose scope mask intersects the given mask, ordered by id.
  [[nodiscard]] std::vector<DelegationRecord> delegations_with_scope(std::uint16_t scope_mask) const;

  // The digest of everything that can change the federation generation. Two
  // states that share a generation and this digest are the same authority.
  [[nodiscard]] Digest authority_digest() const;
  // The digest of one site's own authoritative record together with its
  // delegations. This is what a site accepts and what it reports back.
  [[nodiscard]] Digest site_authority_digest(const SiteId& site) const;
  // The digest of the whole state including reconciliations, receipts, and
  // observation. Used to verify a snapshot, not to fence a site.
  [[nodiscard]] Digest canonical_digest() const;

  // Canonical encoding of the whole state, used by snapshots. The encoding is
  // deterministic and independent of the order anything was inserted in.
  void encode_into(Encoder& encoder) const;
  [[nodiscard]] static Result<FederationState> decode_from(Decoder& decoder,
                                                           const Limits& limits);

  // Applies one durable entry. This is the only way state changes, and it is
  // used identically by the live path and by crash recovery, so a recovered
  // state cannot differ from the state that was live.
  [[nodiscard]] Result<Ack> apply(const JournalEntry& entry, const Limits& limits);

  [[nodiscard]] std::size_t site_count() const noexcept { return sites_.size(); }
  [[nodiscard]] std::size_t delegation_count() const noexcept { return delegations_.size(); }

 private:
  [[nodiscard]] Result<Ack> apply_change(const MutationPayload& payload, const JournalEntry& entry,
                                         const Limits& limits);

  FederationId federation_{};
  FederationGeneration generation_{};
  JournalSequence sequence_{};
  UnixMillis created_{};
  std::map<SiteId, MembershipRecord> sites_{};
  std::map<DelegationId, DelegationRecord> delegations_{};
  std::map<CapabilityId, CompatibilityWindow> windows_{};
  std::map<ReconciliationId, ReconciliationRecord> reconciliations_{};
  std::map<IdempotencyKey, IdempotencyReceipt> receipts_{};
};

}  // namespace dcf
