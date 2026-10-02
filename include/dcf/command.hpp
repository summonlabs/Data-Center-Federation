// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "dcf/capability.hpp"
#include "dcf/codec.hpp"
#include "dcf/delegation.hpp"
#include "dcf/membership.hpp"
#include "dcf/receipt.hpp"
#include "dcf/reconciliation.hpp"
#include "dcf/types.hpp"

namespace dcf {

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
// A command is a request, not a fact. Nothing in this header changes state; the
// engine decides what a command means and whether it is allowed.
struct RegisterSiteCommand {
  SiteId site{};
  std::string display_name{};
  CompatibilityDeclaration declaration{};

  friend bool operator==(const RegisterSiteCommand&, const RegisterSiteCommand&) = default;
};

struct ValidateSiteCommand {
  SiteId site{};

  friend bool operator==(const ValidateSiteCommand&, const ValidateSiteCommand&) = default;
};

struct AdmitSiteCommand {
  SiteId site{};
  // The membership generation the caller believes the site is at. A mismatch is
  // refused as stale rather than applied to whatever the site has become since.
  MembershipGeneration expected_generation{};

  friend bool operator==(const AdmitSiteCommand&, const AdmitSiteCommand&) = default;
};

struct ActivateSiteCommand {
  SiteId site{};
  AcceptedGeneration accepted{};
  Digest accepted_history{};
  SiteLocalEpoch local_epoch{};

  friend bool operator==(const ActivateSiteCommand&, const ActivateSiteCommand&) = default;
};

struct ConstrainSiteCommand {
  SiteId site{};
  std::uint16_t scopes{0};
  std::string reason{};

  friend bool operator==(const ConstrainSiteCommand&, const ConstrainSiteCommand&) = default;
};

struct BeginDrainCommand {
  SiteId site{};
  std::string reason{};

  friend bool operator==(const BeginDrainCommand&, const BeginDrainCommand&) = default;
};

struct CancelDrainCommand {
  SiteId site{};
  std::string reason{};

  friend bool operator==(const CancelDrainCommand&, const CancelDrainCommand&) = default;
};

struct RemoveSiteCommand {
  SiteId site{};
  std::string reason{};

  friend bool operator==(const RemoveSiteCommand&, const RemoveSiteCommand&) = default;
};

// Connectivity is observed by the transport and reported to the engine. It is
// never inferred from a lack of traffic alone.
struct SetConnectivityCommand {
  SiteId site{};
  LinkState link{LinkState::Unknown};

  friend bool operator==(const SetConnectivityCommand&, const SetConnectivityCommand&) = default;
};

struct DeclareWindowCommand {
  CompatibilityWindow window{};

  friend bool operator==(const DeclareWindowCommand&, const DeclareWindowCommand&) = default;
};

struct GrantDelegationCommand {
  DelegationGrant grant{};

  friend bool operator==(const GrantDelegationCommand&, const GrantDelegationCommand&) = default;
};

struct RevokeDelegationCommand {
  DelegationId delegation{};
  std::string reason{};

  friend bool operator==(const RevokeDelegationCommand&, const RevokeDelegationCommand&) = default;
};

// A site reconnecting after a partition states what it believes.
struct RejoinCommand {
  SiteReport report{};
  // The reconnecting site asserts that some other site also holds an exclusive
  // grant overlapping its own. The federation checks the claim against its own
  // records; the assertion alone never changes anything.
  bool claims_exclusive_conflict{false};

  friend bool operator==(const RejoinCommand&, const RejoinCommand&) = default;
};

struct ResolveConflictCommand {
  ConflictResolution resolution{};

  friend bool operator==(const ResolveConflictCommand&, const ResolveConflictCommand&) = default;
};

// A heartbeat that also carries what the site believes it has accepted. It is
// observation only: recording it never changes authority and never advances the
// federation generation, which is what lets a site converge instead of being
// permanently one generation behind its own report.
struct RecordContactCommand {
  SiteId site{};
  AcceptedGeneration accepted{};
  Digest accepted_history{};
  SiteLocalEpoch local_epoch{};
  UnixMillis at{};

  friend bool operator==(const RecordContactCommand&, const RecordContactCommand&) = default;
};

using CommandPayload =
    std::variant<RegisterSiteCommand, ValidateSiteCommand, AdmitSiteCommand, ActivateSiteCommand,
                 ConstrainSiteCommand, BeginDrainCommand, CancelDrainCommand, RemoveSiteCommand,
                 SetConnectivityCommand, DeclareWindowCommand, GrantDelegationCommand,
                 RevokeDelegationCommand, RejoinCommand, ResolveConflictCommand,
                 RecordContactCommand>;

struct Command {
  CommandPayload payload{RegisterSiteCommand{}};
  // The caller's operation identifier, used to make retries safe. A zero
  // identifier means the caller has not asked for idempotency and the command is
  // applied every time it is submitted.
  OperationId operation{};
  // The site that submitted the command. A zero identifier means the federation
  // operator itself, which holds the federation's own authority and needs no
  // delegation.
  SiteId origin{};
  // The federation generation the origin believes is current. A command that
  // would change authoritative state is refused when this is stale, so a message
  // that was composed before a partition cannot take effect after it.
  FederationGeneration origin_generation{};
};

// The name of a command, used in diagnostics and receipts.
[[nodiscard]] std::string_view command_name(const CommandPayload& payload) noexcept;
// True when applying the command can change authoritative federation state.
[[nodiscard]] bool command_changes_authority(const CommandPayload& payload) noexcept;
// True when the command is an act of a site about itself. A self-scoped command
// needs no delegation, but only the site it names may submit it.
[[nodiscard]] bool is_self_scoped(const CommandPayload& payload) noexcept;
// The delegation scope the origin must hold. std::nullopt means the command is
// not covered by a delegation: it is either self-scoped, or it is reserved to
// the federation operator.
[[nodiscard]] std::optional<DelegationScope> required_scope(const CommandPayload& payload) noexcept;
// The site the command is about, if it is about exactly one site.
[[nodiscard]] std::optional<SiteId> subject_site(const CommandPayload& payload) noexcept;

void encode(Encoder& encoder, const Command& command);
[[nodiscard]] Result<Command> decode_command(Decoder& decoder);
// The digest of the canonical encoding of a command. Two commands with the same
// digest are the same request, which is what makes an idempotent retry checkable.
[[nodiscard]] Digest command_digest(const Command& command);

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------
// What the engine decided. An outcome is returned whether the command succeeded
// or was refused; a refusal is a decision with a reason, not an exception. The
// outcome is also what a durable receipt replays.
struct CommandOutcome {
  // None means the command was accepted.
  ErrorCode code{ErrorCode::None};
  std::string detail{};
  // The federation generation after the command. For a refusal this is the
  // unchanged current generation.
  FederationGeneration generation{};
  MembershipGeneration membership_generation{};
  // Populated for commands that produce an explainable evaluation.
  std::optional<CompatibilityReport> compatibility{};
  std::optional<ReconciliationEvaluation> reconciliation{};

  friend bool operator==(const CommandOutcome&, const CommandOutcome&) = default;
};

[[nodiscard]] inline bool succeeded(const CommandOutcome& outcome) noexcept {
  return outcome.code == ErrorCode::None;
}

}  // namespace dcf
