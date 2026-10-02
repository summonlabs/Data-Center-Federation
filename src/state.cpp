// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/state.hpp"

#include <algorithm>
#include <type_traits>

#include "dcf/hash.hpp"
#include "record_codec.hpp"

namespace dcf {
namespace {

// The fields of a membership record that can change the federation generation.
// Observation fields are deliberately excluded: a site reporting what it has
// accepted is evidence, not authority, and it does not advance the generation.
// The authority digest therefore covers exactly the state a generation names.
void put_authority_fields(Encoder& encoder, const MembershipRecord& record) {
  detail::put(encoder, record.site);
  encoder.text(record.display_name);
  detail::put(encoder, record.state);
  encoder.u64(record.generation.value());
  encoder.u64(record.entered_at.value());
  detail::put(encoder, record.pre_partition_state);
  encoder.u16(record.constrained_scopes);
  detail::put(encoder, record.link);
  detail::put(encoder, record.declaration_digest);
  detail::put(encoder, record.admission);
}

void put_observation_fields(Encoder& encoder, const MembershipRecord& record) {
  encoder.u64(record.accepted.value());
  encoder.u64(record.local_epoch.value());
  detail::put(encoder, record.accepted_history);
  encoder.i64(record.last_contact.value);
  detail::put(encoder, record.declaration);
  encoder.text(record.transition_reason);
  encoder.text(record.display_name);
}

void put_authority_delegation(Encoder& encoder, const DelegationRecord& record) {
  detail::put(encoder, record.grant);
  encoder.boolean(record.revoked);
  encoder.u64(record.revoked_at.value());
}

[[nodiscard]] Result<Ack> require_site(const std::map<SiteId, MembershipRecord>& sites,
                                       const SiteId& site) {
  if (sites.find(site) == sites.end()) {
    return make_error(ErrorCode::NotFound,
                      "site " + to_string(site) + " has no membership record in this federation");
  }
  return Ack{};
}

}  // namespace

std::string_view mutation_name(const MutationPayload& payload) noexcept {
  return std::visit(
      [](const auto& mutation) -> std::string_view {
        using T = std::decay_t<decltype(mutation)>;
        if constexpr (std::is_same_v<T, GenesisMutation>) {
          return "genesis";
        } else if constexpr (std::is_same_v<T, SiteRegisteredMutation>) {
          return "site_registered";
        } else if constexpr (std::is_same_v<T, CompatibilityEvaluatedMutation>) {
          return "compatibility_evaluated";
        } else if constexpr (std::is_same_v<T, MembershipTransitionMutation>) {
          return "membership_transition";
        } else if constexpr (std::is_same_v<T, DelegationGrantedMutation>) {
          return "delegation_granted";
        } else if constexpr (std::is_same_v<T, DelegationRevokedMutation>) {
          return "delegation_revoked";
        } else if constexpr (std::is_same_v<T, CompatibilityWindowDeclaredMutation>) {
          return "compatibility_window_declared";
        } else if constexpr (std::is_same_v<T, ConnectivityChangedMutation>) {
          return "connectivity_changed";
        } else if constexpr (std::is_same_v<T, SiteReportedStateMutation>) {
          return "site_reported_state";
        } else if constexpr (std::is_same_v<T, ReconciliationRecordedMutation>) {
          return "reconciliation_recorded";
        } else {
          return "conflict_resolved";
        }
      },
      payload);
}

bool mutation_changes_authority(const MutationPayload& payload) noexcept {
  // Two mutations are evidence rather than authority. Recording what a site
  // reported, and recording the outcome of a reconciliation, do not change who
  // may do what, so they do not advance the generation. If they did, a site that
  // had just synchronised would be behind again the moment its own report was
  // written, and a reconnecting site could never converge.
  return !std::holds_alternative<SiteReportedStateMutation>(payload) &&
         !std::holds_alternative<ReconciliationRecordedMutation>(payload);
}

void encode(Encoder& encoder, const MutationPayload& payload) { detail::put(encoder, payload); }

Result<MutationPayload> decode_mutation(Decoder& decoder) { return detail::get_mutation(decoder); }

void encode(Encoder& encoder, const JournalEntry& entry) { detail::put(encoder, entry); }

Result<JournalEntry> decode_journal_entry(Decoder& decoder) {
  return detail::get_journal_entry(decoder);
}

FederationState FederationState::genesis(FederationId federation, UnixMillis created) {
  FederationState state;
  state.federation_ = federation;
  state.created_ = created;
  return state;
}

const MembershipRecord* FederationState::find_site(const SiteId& site) const noexcept {
  const auto iterator = sites_.find(site);
  return iterator == sites_.end() ? nullptr : &iterator->second;
}

const DelegationRecord* FederationState::find_delegation(const DelegationId& id) const noexcept {
  const auto iterator = delegations_.find(id);
  return iterator == delegations_.end() ? nullptr : &iterator->second;
}

const CompatibilityWindow* FederationState::find_window(const CapabilityId& id) const noexcept {
  const auto iterator = windows_.find(id);
  return iterator == windows_.end() ? nullptr : &iterator->second;
}

const ReconciliationRecord* FederationState::find_reconciliation(
    const ReconciliationId& id) const noexcept {
  const auto iterator = reconciliations_.find(id);
  return iterator == reconciliations_.end() ? nullptr : &iterator->second;
}

const IdempotencyReceipt* FederationState::find_receipt(const IdempotencyKey& key) const noexcept {
  const auto iterator = receipts_.find(key);
  return iterator == receipts_.end() ? nullptr : &iterator->second;
}

std::vector<DelegationRecord> FederationState::delegations_for(const SiteId& site) const {
  std::vector<DelegationRecord> out;
  for (const auto& [id, record] : delegations_) {
    if (record.grant.grantee == site) {
      out.push_back(record);
    }
  }
  return out;
}

std::vector<CompatibilityWindow> FederationState::window_list() const {
  std::vector<CompatibilityWindow> out;
  out.reserve(windows_.size());
  for (const auto& [id, window] : windows_) {
    out.push_back(window);
  }
  return out;
}

std::vector<DelegationRecord> FederationState::delegations_with_scope(
    std::uint16_t scope_mask) const {
  std::vector<DelegationRecord> out;
  for (const auto& [id, record] : delegations_) {
    if ((record.grant.scope_mask & scope_mask) != 0U) {
      out.push_back(record);
    }
  }
  return out;
}

Digest FederationState::authority_digest() const {
  Encoder encoder;
  encoder.text("dcf.authority.v1");
  detail::put(encoder, federation_);
  encoder.u64(generation_.value());

  encoder.u32(static_cast<std::uint32_t>(sites_.size()));
  for (const auto& [id, record] : sites_) {
    put_authority_fields(encoder, record);
  }

  encoder.u32(static_cast<std::uint32_t>(delegations_.size()));
  for (const auto& [id, record] : delegations_) {
    put_authority_delegation(encoder, record);
  }

  encoder.u32(static_cast<std::uint32_t>(windows_.size()));
  for (const auto& [id, window] : windows_) {
    detail::put(encoder, window);
  }

  return digest_of(to_string(encoder.bytes()));
}

Digest FederationState::site_authority_digest(const SiteId& site) const {
  Encoder encoder;
  encoder.text("dcf.site.v1");
  detail::put(encoder, federation_);
  encoder.u64(generation_.value());
  detail::put(encoder, site);

  const MembershipRecord* record = find_site(site);
  encoder.boolean(record != nullptr);
  if (record != nullptr) {
    put_authority_fields(encoder, *record);
  }

  std::size_t count = 0;
  for (const auto& [id, delegation] : delegations_) {
    if (delegation.grant.grantee == site) {
      ++count;
    }
  }
  encoder.u32(static_cast<std::uint32_t>(count));
  for (const auto& [id, delegation] : delegations_) {
    if (delegation.grant.grantee == site) {
      put_authority_delegation(encoder, delegation);
    }
  }

  return digest_of(to_string(encoder.bytes()));
}

Digest FederationState::canonical_digest() const {
  Encoder encoder;
  encoder.text("dcf.canonical.v1");
  detail::put(encoder, federation_);
  encoder.u64(generation_.value());
  encoder.u64(sequence_.value());
  encoder.i64(created_.value);

  encoder.u32(static_cast<std::uint32_t>(sites_.size()));
  for (const auto& [id, record] : sites_) {
    put_authority_fields(encoder, record);
    put_observation_fields(encoder, record);
  }

  encoder.u32(static_cast<std::uint32_t>(delegations_.size()));
  for (const auto& [id, record] : delegations_) {
    put_authority_delegation(encoder, record);
    encoder.text(record.revocation_reason);
  }

  encoder.u32(static_cast<std::uint32_t>(windows_.size()));
  for (const auto& [id, window] : windows_) {
    detail::put(encoder, window);
  }

  encoder.u32(static_cast<std::uint32_t>(reconciliations_.size()));
  for (const auto& [id, record] : reconciliations_) {
    detail::put(encoder, record);
  }

  encoder.u32(static_cast<std::uint32_t>(receipts_.size()));
  for (const auto& [key, receipt] : receipts_) {
    detail::put(encoder, receipt);
  }

  return digest_of(to_string(encoder.bytes()));
}

void FederationState::encode_into(Encoder& encoder) const {
  detail::put(encoder, federation_);
  encoder.u64(generation_.value());
  encoder.u64(sequence_.value());
  encoder.i64(created_.value);

  encoder.u32(static_cast<std::uint32_t>(sites_.size()));
  for (const auto& [id, record] : sites_) {
    detail::put(encoder, record);
  }
  encoder.u32(static_cast<std::uint32_t>(delegations_.size()));
  for (const auto& [id, record] : delegations_) {
    detail::put(encoder, record);
  }
  encoder.u32(static_cast<std::uint32_t>(windows_.size()));
  for (const auto& [id, window] : windows_) {
    detail::put(encoder, window);
  }
  encoder.u32(static_cast<std::uint32_t>(reconciliations_.size()));
  for (const auto& [id, record] : reconciliations_) {
    detail::put(encoder, record);
  }
  encoder.u32(static_cast<std::uint32_t>(receipts_.size()));
  for (const auto& [key, receipt] : receipts_) {
    detail::put(encoder, receipt);
  }
}

Result<FederationState> FederationState::decode_from(Decoder& decoder, const Limits& limits) {
  FederationState state;
  const auto federation = detail::get_identifier<FederationTag>(decoder);
  if (!federation) {
    return federation.error();
  }
  state.federation_ = federation.value();
  const auto generation = decoder.u64();
  if (!generation) {
    return generation.error();
  }
  const auto sequence = decoder.u64();
  if (!sequence) {
    return sequence.error();
  }
  const auto created = decoder.i64();
  if (!created) {
    return created.error();
  }
  state.generation_ = FederationGeneration{generation.value()};
  state.sequence_ = JournalSequence{sequence.value()};
  state.created_ = UnixMillis{created.value()};

  const auto site_count = decoder.collection_count();
  if (!site_count) {
    return site_count.error();
  }
  if (static_cast<std::uint64_t>(site_count.value()) > limits.max_sites) {
    return make_error(ErrorCode::BoundsExceeded,
                      "snapshot carries " + std::to_string(site_count.value()) +
                          " sites which exceeds the configured maximum of " +
                          std::to_string(limits.max_sites));
  }
  for (std::uint32_t index = 0; index < site_count.value(); ++index) {
    const auto record = detail::get_membership_record(decoder);
    if (!record) {
      return record.error();
    }
    state.sites_.emplace(record.value().site, record.value());
  }

  const auto delegation_count = decoder.collection_count();
  if (!delegation_count) {
    return delegation_count.error();
  }
  if (static_cast<std::uint64_t>(delegation_count.value()) > limits.max_delegations) {
    return make_error(ErrorCode::BoundsExceeded,
                      "snapshot carries " + std::to_string(delegation_count.value()) +
                          " delegations which exceeds the configured maximum of " +
                          std::to_string(limits.max_delegations));
  }
  for (std::uint32_t index = 0; index < delegation_count.value(); ++index) {
    const auto record = detail::get_delegation_record(decoder);
    if (!record) {
      return record.error();
    }
    state.delegations_.emplace(record.value().grant.id, record.value());
  }

  const auto window_count = decoder.collection_count();
  if (!window_count) {
    return window_count.error();
  }
  for (std::uint32_t index = 0; index < window_count.value(); ++index) {
    const auto window = detail::get_window(decoder);
    if (!window) {
      return window.error();
    }
    state.windows_.emplace(window.value().capability, window.value());
  }

  const auto reconciliation_count = decoder.collection_count();
  if (!reconciliation_count) {
    return reconciliation_count.error();
  }
  if (static_cast<std::uint64_t>(reconciliation_count.value()) > limits.max_reconciliations) {
    return make_error(ErrorCode::BoundsExceeded,
                      "snapshot carries " + std::to_string(reconciliation_count.value()) +
                          " reconciliation records which exceeds the configured maximum of " +
                          std::to_string(limits.max_reconciliations));
  }
  for (std::uint32_t index = 0; index < reconciliation_count.value(); ++index) {
    const auto record = detail::get_reconciliation_record(decoder);
    if (!record) {
      return record.error();
    }
    state.reconciliations_.emplace(record.value().id, record.value());
  }

  const auto receipt_count = decoder.collection_count();
  if (!receipt_count) {
    return receipt_count.error();
  }
  if (static_cast<std::uint64_t>(receipt_count.value()) > limits.max_receipts) {
    return make_error(ErrorCode::BoundsExceeded,
                      "snapshot carries " + std::to_string(receipt_count.value()) +
                          " receipts which exceeds the configured maximum of " +
                          std::to_string(limits.max_receipts));
  }
  for (std::uint32_t index = 0; index < receipt_count.value(); ++index) {
    const auto receipt = detail::get_receipt(decoder);
    if (!receipt) {
      return receipt.error();
    }
    state.receipts_.emplace(receipt.value().key, receipt.value());
  }

  return state;
}

Result<Ack> FederationState::apply(const JournalEntry& entry, const Limits& limits) {
  if (entry.changes.empty() && !entry.receipt.has_value()) {
    return make_error(ErrorCode::MalformedInput,
                      "journal entry carries neither a change nor a receipt");
  }

  if (sequence_.exhausted()) {
    return make_error(ErrorCode::ArithmeticOverflow,
                      "journal sequence has reached its maximum and cannot advance");
  }
  if (entry.sequence.value() != sequence_.value() + 1) {
    return make_error(ErrorCode::Conflict,
                      "journal sequence " + std::to_string(entry.sequence.value()) +
                          " does not follow the current sequence " +
                          std::to_string(sequence_.value()));
  }

  const FederationGeneration before = generation_;
  bool changes_authority = false;
  for (const MutationPayload& payload : entry.changes) {
    changes_authority = changes_authority || mutation_changes_authority(payload);
    const auto applied = apply_change(payload, entry, limits);
    if (!applied) {
      return applied.error();
    }
  }

  if (changes_authority) {
    if (before.exhausted()) {
      return make_error(ErrorCode::ArithmeticOverflow,
                        "federation generation has reached its maximum and cannot advance");
    }
    const std::uint64_t expected = before.value() + 1;
    if (entry.generation.value() != expected) {
      return make_error(ErrorCode::Conflict,
                        "journal entry declares federation generation " +
                            std::to_string(entry.generation.value()) +
                            " but the state advances from " + std::to_string(before.value()) +
                            " to " + std::to_string(expected));
    }
  } else if (entry.generation.value() != before.value()) {
    return make_error(ErrorCode::Conflict,
                      "journal entry declares federation generation " +
                          std::to_string(entry.generation.value()) +
                          " but it carries no authority change, so the generation stays at " +
                          std::to_string(before.value()));
  }

  if (entry.receipt.has_value()) {
    const IdempotencyKey& key = entry.receipt->key;
    const bool is_new = receipts_.find(key) == receipts_.end();
    if (is_new && static_cast<std::uint64_t>(receipts_.size()) >= limits.max_receipts) {
      return make_error(ErrorCode::CapacityExhausted,
                        "the receipt table holds " + std::to_string(receipts_.size()) +
                            " entries which is the configured maximum of " +
                            std::to_string(limits.max_receipts));
    }
    receipts_[key] = *entry.receipt;
  }

  generation_ = entry.generation;
  sequence_ = entry.sequence;
  return Ack{};
}

Result<Ack> FederationState::apply_change(const MutationPayload& payload, const JournalEntry& entry,
                                          const Limits& limits) {
  if (const auto* mutation = std::get_if<GenesisMutation>(&payload)) {
    if (initialized()) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "genesis was applied to a federation that already exists");
    }
    if (mutation->federation.is_zero()) {
      return make_error(ErrorCode::InvalidArgument, "genesis carries a zero federation identifier");
    }
    federation_ = mutation->federation;
    created_ = mutation->created;
    return Ack{};
  }

  if (!initialized()) {
    return make_error(ErrorCode::Indeterminate,
                      "a mutation other than genesis was applied before the federation existed");
  }

  if (const auto* mutation = std::get_if<SiteRegisteredMutation>(&payload)) {
    if (mutation->record.site.is_zero()) {
      return make_error(ErrorCode::InvalidArgument, "a site record carries a zero identifier");
    }
    if (sites_.find(mutation->record.site) != sites_.end()) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "site " + to_string(mutation->record.site) +
                            " already has a membership record");
    }
    if (static_cast<std::uint64_t>(sites_.size()) >= limits.max_sites) {
      return make_error(ErrorCode::CapacityExhausted,
                        "the federation already holds " + std::to_string(sites_.size()) +
                            " sites which is the configured maximum of " +
                            std::to_string(limits.max_sites));
    }
    sites_.emplace(mutation->record.site, mutation->record);
    return Ack{};
  }

  if (const auto* mutation = std::get_if<CompatibilityEvaluatedMutation>(&payload)) {
    const auto exists = require_site(sites_, mutation->site);
    if (!exists) {
      return exists.error();
    }
    MembershipRecord& record = sites_.at(mutation->site);
    if (!is_legal_transition(record.state, mutation->resulting_state)) {
      return make_error(ErrorCode::Conflict,
                        "compatibility evaluation would move site " + to_string(mutation->site) +
                            " from " + std::string(to_string(record.state)) + " to " +
                            std::string(to_string(mutation->resulting_state)) +
                            " which the lifecycle does not allow");
    }
    record.state = mutation->resulting_state;
    record.entered_at = entry.generation;
    record.constrained_scopes = mutation->constrained_scopes;
    record.admission = mutation->report;
    record.generation = MembershipGeneration{record.generation.value() + 1};
    return Ack{};
  }

  if (const auto* mutation = std::get_if<MembershipTransitionMutation>(&payload)) {
    const auto exists = require_site(sites_, mutation->site);
    if (!exists) {
      return exists.error();
    }
    MembershipRecord& record = sites_.at(mutation->site);
    if (!is_legal_transition(record.state, mutation->to)) {
      return make_error(ErrorCode::Conflict,
                        "site " + to_string(mutation->site) + " cannot move from " +
                            std::string(to_string(record.state)) + " to " +
                            std::string(to_string(mutation->to)));
    }
    if (mutation->to == MembershipState::Partitioned) {
      record.pre_partition_state = mutation->pre_partition_state;
    }
    if (record.generation.exhausted()) {
      return make_error(ErrorCode::ArithmeticOverflow,
                        "membership generation for site " + to_string(mutation->site) +
                            " has reached its maximum and cannot advance");
    }
    record.state = mutation->to;
    record.entered_at = entry.generation;
    record.constrained_scopes = mutation->constrained_scopes;
    record.transition_reason = mutation->reason;
    record.generation = MembershipGeneration{record.generation.value() + 1};
    return Ack{};
  }

  if (const auto* mutation = std::get_if<DelegationGrantedMutation>(&payload)) {
    if (mutation->grant.id.is_zero()) {
      return make_error(ErrorCode::InvalidArgument, "a delegation grant carries a zero identifier");
    }
    if (delegations_.find(mutation->grant.id) != delegations_.end()) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "delegation " + to_string(mutation->grant.id) + " is already recorded");
    }
    if (static_cast<std::uint64_t>(delegations_.size()) >= limits.max_delegations) {
      return make_error(ErrorCode::CapacityExhausted,
                        "the federation already holds " + std::to_string(delegations_.size()) +
                            " delegations which is the configured maximum of " +
                            std::to_string(limits.max_delegations));
    }
    DelegationRecord record;
    record.grant = mutation->grant;
    delegations_.emplace(mutation->grant.id, std::move(record));
    return Ack{};
  }

  if (const auto* mutation = std::get_if<DelegationRevokedMutation>(&payload)) {
    const auto iterator = delegations_.find(mutation->revocation.id);
    if (iterator == delegations_.end()) {
      return make_error(ErrorCode::NotFound,
                        "delegation " + to_string(mutation->revocation.id) +
                            " has no record and cannot be revoked");
    }
    if (iterator->second.revoked) {
      // Revocation is idempotent by identity: re-revoking keeps the earliest
      // revocation, so a replayed revocation cannot move the boundary later.
      return Ack{};
    }
    iterator->second.revoked = true;
    iterator->second.revoked_at = mutation->revocation.revoked_at;
    iterator->second.revocation_reason = mutation->revocation.reason;
    return Ack{};
  }

  if (const auto* mutation = std::get_if<CompatibilityWindowDeclaredMutation>(&payload)) {
    if (!is_valid_capability_name(mutation->window.capability.value)) {
      return make_error(ErrorCode::InvalidArgument,
                        "window names '" + sanitize_for_terminal(mutation->window.capability.value) +
                            "' which is not a valid capability name");
    }
    if (mutation->window.offered.is_empty()) {
      return make_error(ErrorCode::InvalidArgument,
                        "window for '" + mutation->window.capability.value +
                            "' offers an empty version range");
    }
    const bool is_new = windows_.find(mutation->window.capability) == windows_.end();
    if (is_new && static_cast<std::uint64_t>(windows_.size()) >= limits.max_collection_items) {
      return make_error(ErrorCode::CapacityExhausted,
                        "the window table holds " + std::to_string(windows_.size()) +
                            " entries which is the configured maximum of " +
                            std::to_string(limits.max_collection_items));
    }
    windows_[mutation->window.capability] = mutation->window;
    return Ack{};
  }

  if (const auto* mutation = std::get_if<ConnectivityChangedMutation>(&payload)) {
    const auto exists = require_site(sites_, mutation->site);
    if (!exists) {
      return exists.error();
    }
    sites_.at(mutation->site).link = mutation->link;
    return Ack{};
  }

  if (const auto* mutation = std::get_if<SiteReportedStateMutation>(&payload)) {
    const auto exists = require_site(sites_, mutation->site);
    if (!exists) {
      return exists.error();
    }
    MembershipRecord& record = sites_.at(mutation->site);
    record.accepted = mutation->accepted;
    record.local_epoch = mutation->local_epoch;
    record.accepted_history = mutation->accepted_history;
    record.last_contact = mutation->contact;
    return Ack{};
  }

  if (const auto* mutation = std::get_if<ReconciliationRecordedMutation>(&payload)) {
    if (mutation->record.id.is_zero()) {
      return make_error(ErrorCode::InvalidArgument,
                        "a reconciliation record carries a zero identifier");
    }
    if (reconciliations_.find(mutation->record.id) != reconciliations_.end()) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "reconciliation " + to_string(mutation->record.id) + " is already recorded");
    }
    if (static_cast<std::uint64_t>(reconciliations_.size()) >= limits.max_reconciliations) {
      return make_error(ErrorCode::CapacityExhausted,
                        "the reconciliation table holds " + std::to_string(reconciliations_.size()) +
                            " entries which is the configured maximum of " +
                            std::to_string(limits.max_reconciliations));
    }
    reconciliations_.emplace(mutation->record.id, mutation->record);
    return Ack{};
  }

  const auto* mutation = std::get_if<ConflictResolvedMutation>(&payload);
  if (mutation == nullptr) {
    return make_error(ErrorCode::Internal, "unrecognised mutation payload");
  }
  const auto iterator = reconciliations_.find(mutation->reconciliation);
  if (iterator == reconciliations_.end()) {
    return make_error(ErrorCode::NotFound,
                      "reconciliation " + to_string(mutation->reconciliation) +
                          " has no record and cannot be resolved");
  }
  if (iterator->second.resolved) {
    return make_error(ErrorCode::Conflict,
                      "reconciliation " + to_string(mutation->reconciliation) +
                          " has already been resolved");
  }
  iterator->second.resolved = true;
  iterator->second.resolution = std::string(to_string(mutation->choice)) +
                                " under authority '" +
                                sanitize_for_terminal(mutation->authority_reference) + "'";
  return Ack{};
}

}  // namespace dcf
