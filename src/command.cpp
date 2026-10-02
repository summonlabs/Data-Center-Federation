// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/command.hpp"

#include <type_traits>

#include "record_codec.hpp"

namespace dcf {

std::string_view command_name(const CommandPayload& payload) noexcept {
  return std::visit(
      [](const auto& command) -> std::string_view {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, RegisterSiteCommand>) {
          return "register_site";
        } else if constexpr (std::is_same_v<T, ValidateSiteCommand>) {
          return "validate_site";
        } else if constexpr (std::is_same_v<T, AdmitSiteCommand>) {
          return "admit_site";
        } else if constexpr (std::is_same_v<T, ActivateSiteCommand>) {
          return "activate_site";
        } else if constexpr (std::is_same_v<T, ConstrainSiteCommand>) {
          return "constrain_site";
        } else if constexpr (std::is_same_v<T, BeginDrainCommand>) {
          return "begin_drain";
        } else if constexpr (std::is_same_v<T, CancelDrainCommand>) {
          return "cancel_drain";
        } else if constexpr (std::is_same_v<T, RemoveSiteCommand>) {
          return "remove_site";
        } else if constexpr (std::is_same_v<T, SetConnectivityCommand>) {
          return "set_connectivity";
        } else if constexpr (std::is_same_v<T, DeclareWindowCommand>) {
          return "declare_window";
        } else if constexpr (std::is_same_v<T, GrantDelegationCommand>) {
          return "grant_delegation";
        } else if constexpr (std::is_same_v<T, RevokeDelegationCommand>) {
          return "revoke_delegation";
        } else if constexpr (std::is_same_v<T, RejoinCommand>) {
          return "rejoin";
        } else if constexpr (std::is_same_v<T, ResolveConflictCommand>) {
          return "resolve_conflict";
        } else {
          return "record_contact";
        }
      },
      payload);
}

bool command_changes_authority(const CommandPayload& payload) noexcept {
  // Reporting is not acting: a reconnecting site states what it believes and a
  // heartbeat states that it is alive, and neither changes authority. Everything
  // else does.
  return std::visit(
      [](const auto& command) -> bool {
        using T = std::decay_t<decltype(command)>;
        return !std::is_same_v<T, RecordContactCommand> && !std::is_same_v<T, RejoinCommand>;
      },
      payload);
}

bool is_self_scoped(const CommandPayload& payload) noexcept {
  return std::visit(
      [](const auto& command) -> bool {
        using T = std::decay_t<decltype(command)>;
        return std::is_same_v<T, RegisterSiteCommand> || std::is_same_v<T, ActivateSiteCommand> ||
               std::is_same_v<T, RejoinCommand> || std::is_same_v<T, RecordContactCommand>;
      },
      payload);
}

std::optional<DelegationScope> required_scope(const CommandPayload& payload) noexcept {
  return std::visit(
      [](const auto& command) -> std::optional<DelegationScope> {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, ValidateSiteCommand> || std::is_same_v<T, AdmitSiteCommand>) {
          return DelegationScope::MembershipAdmit;
        } else if constexpr (std::is_same_v<T, DeclareWindowCommand>) {
          return DelegationScope::CompatibilityApprove;
        } else if constexpr (std::is_same_v<T, BeginDrainCommand> ||
                             std::is_same_v<T, CancelDrainCommand>) {
          return DelegationScope::DrainSchedule;
        } else {
          return std::nullopt;
        }
      },
      payload);
}

std::optional<SiteId> subject_site(const CommandPayload& payload) noexcept {
  return std::visit(
      [](const auto& command) -> std::optional<SiteId> {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, RegisterSiteCommand> || std::is_same_v<T, ValidateSiteCommand> ||
                      std::is_same_v<T, AdmitSiteCommand> || std::is_same_v<T, ActivateSiteCommand> ||
                      std::is_same_v<T, ConstrainSiteCommand> || std::is_same_v<T, BeginDrainCommand> ||
                      std::is_same_v<T, CancelDrainCommand> || std::is_same_v<T, RemoveSiteCommand> ||
                      std::is_same_v<T, SetConnectivityCommand>) {
          return command.site;
        } else if constexpr (std::is_same_v<T, GrantDelegationCommand>) {
          return command.grant.grantee;
        } else if constexpr (std::is_same_v<T, RejoinCommand>) {
          return command.report.site;
        } else if constexpr (std::is_same_v<T, RecordContactCommand>) {
          return command.site;
        } else {
          return std::nullopt;
        }
      },
      payload);
}

void encode(Encoder& encoder, const Command& command) { detail::put(encoder, command); }

Result<Command> decode_command(Decoder& decoder) { return detail::get_command(decoder); }

Digest command_digest(const Command& command) {
  Encoder encoder;
  encode(encoder, command);
  return digest_of(to_string(encoder.bytes()));
}

}  // namespace dcf
