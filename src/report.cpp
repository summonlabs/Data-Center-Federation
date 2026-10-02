// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/report.hpp"

#include "dcf/hash.hpp"
#include "dcf/json.hpp"
#include "dcf/version.hpp"

namespace dcf {
namespace {

void render_window(Json& json, const CompatibilityWindow& window) {
  json.begin_object();
  json.key("capability").value(window.capability.value);
  json.key("offered_min").value(window.offered.minimum.to_string());
  json.key("offered_max").value(window.offered.maximum.to_string());
  json.key("effective_from").value(window.effective_from.value());
  json.key("soft_deprecate").value(window.soft_deprecate.value());
  json.key("hard_remove").value(window.hard_remove.value());
  json.end_object();
}

void render_finding(Json& json, const CompatibilityFinding& finding) {
  json.begin_object();
  json.key("code").value(to_string(finding.code));
  json.key("subject").value(finding.subject.value);
  json.key("detail").value(finding.detail);
  json.end_object();
}

void render_delegation(Json& json, const DelegationRecord& record, FederationGeneration at,
                       UnixMillis now, MembershipGeneration membership, bool partitioned) {
  json.begin_object();
  json.key("id").value(to_string(record.grant.id));
  json.key("grantee").value(to_string(record.grant.grantee));
  json.key("scopes").value(to_string(record.grant.scope_mask));
  json.key("granted_at_generation").value(record.grant.granted_at.value());
  json.key("grantee_membership_generation")
      .value(record.grant.grantee_membership_generation.value());
  json.key("expires_at_generation").value(record.grant.expires_at.value());
  json.key("expires_at_wall_ms").value(record.grant.expires_at_wall.value);
  json.key("survives_partition").value(record.grant.survives_partition);
  json.key("exclusive").value(record.grant.exclusive);
  json.key("selectors").begin_array();
  for (const std::string& selector : record.grant.selectors) {
    json.value(selector);
  }
  json.end_array();
  json.key("revoked").value(record.revoked);
  json.key("revoked_at_generation").value(record.revoked_at.value());
  if (!record.revocation_reason.empty()) {
    json.key("revocation_reason").value(record.revocation_reason);
  }
  json.key("status").value(to_string(evaluate_delegation(record, at, now, membership, partitioned)));
  json.end_object();
}

void render_divergence(Json& json, const DivergenceRecord& divergence) {
  json.begin_object();
  json.key("kind").value(to_string(divergence.kind));
  if (!divergence.delegation.is_zero()) {
    json.key("delegation").value(to_string(divergence.delegation));
  }
  json.key("federation_generation").value(divergence.federation_generation.value());
  json.key("membership_generation").value(divergence.membership_generation.value());
  json.key("detail").value(divergence.detail);
  json.end_object();
}

void render_reconciliation(Json& json, const ReconciliationRecord& record) {
  json.begin_object();
  json.key("id").value(to_string(record.id));
  json.key("site").value(to_string(record.site));
  json.key("federation_generation").value(record.federation_generation.value());
  json.key("site_reported_generation").value(record.site_reported_generation.value());
  json.key("federation_history").value(record.federation_history.to_hex());
  json.key("site_history").value(record.site_history.to_hex());
  json.key("outcome").value(to_string(record.outcome));
  json.key("resolved").value(record.resolved);
  if (!record.resolution.empty()) {
    json.key("resolution").value(record.resolution);
  }
  json.key("journal_sequence").value(record.committed.value());
  json.key("observed_at_ms").value(record.observed_at.value);
  json.key("divergences").begin_array();
  for (const DivergenceRecord& divergence : record.divergences) {
    render_divergence(json, divergence);
  }
  json.end_array();
  json.end_object();
}

void render_membership(Json& json, const MembershipRecord& record) {
  json.begin_object();
  json.key("site").value(to_string(record.site));
  json.key("display_name").value(record.display_name);
  json.key("state").value(to_string(record.state));
  json.key("membership_generation").value(record.generation.value());
  json.key("entered_at_generation").value(record.entered_at.value());
  json.key("pre_partition_state").value(to_string(record.pre_partition_state));
  json.key("link").value(to_string(record.link));
  json.key("constrained_scopes").value(to_string(record.constrained_scopes));
  json.key("accepted_generation").value(record.accepted.value());
  json.key("local_epoch").value(record.local_epoch.value());
  json.key("accepted_history").value(record.accepted_history.to_hex());
  json.key("last_contact_ms").value(record.last_contact.value);
  json.key("implementation").value(record.declaration.implementation);
  json.key("declaration_digest").value(record.declaration_digest.to_hex());
  json.key("admission").value(to_string(record.admission.decision));
  json.key("findings").begin_array();
  for (const CompatibilityFinding& finding : record.admission.findings) {
    render_finding(json, finding);
  }
  json.end_array();
  if (!record.transition_reason.empty()) {
    json.key("transition_reason").value(record.transition_reason);
  }
  json.end_object();
}

void render_receipt(Json& json, const IdempotencyReceipt& receipt) {
  json.begin_object();
  json.key("origin").value(to_string(receipt.key.site));
  json.key("operation").value(to_string(receipt.key.operation));
  json.key("request_digest").value(receipt.request.to_hex());
  json.key("outcome").value(to_string(receipt.outcome));
  json.key("generation").value(receipt.generation.value());
  json.key("membership_generation").value(receipt.membership_generation.value());
  json.key("journal_sequence").value(receipt.committed.value());
  json.key("recorded_at_ms").value(receipt.recorded_at.value);
  if (!receipt.outcome_detail.empty()) {
    json.key("detail").value(receipt.outcome_detail);
  }
  json.end_object();
}

[[nodiscard]] Json summary_of(const FederationState& state) {
  Json json;
  json.begin_object();
  json.key("federation").value(to_string(state.federation()));
  json.key("initialized").value(state.initialized());
  json.key("generation").value(state.generation().value());
  json.key("journal_sequence").value(state.sequence().value());
  json.key("created_at_ms").value(state.created().value);
  json.key("sites").value(static_cast<std::uint64_t>(state.sites().size()));
  json.key("delegations").value(static_cast<std::uint64_t>(state.delegations().size()));
  json.key("windows").value(static_cast<std::uint64_t>(state.windows().size()));
  json.key("reconciliations").value(static_cast<std::uint64_t>(state.reconciliations().size()));
  json.key("receipts").value(static_cast<std::uint64_t>(state.receipts().size()));
  json.key("authority_digest").value(state.authority_digest().to_hex());
  json.end_object();
  return json;
}

}  // namespace

std::string render_scope_mask(std::uint16_t mask) { return to_string(mask); }

Result<std::string> render_query(wire::QueryKind kind, const std::string& argument,
                                 const QueryContext& context) {
  if (context.state == nullptr) {
    return make_error(ErrorCode::Indeterminate, "no state is published yet");
  }
  const FederationState& state = *context.state;

  switch (kind) {
    case wire::QueryKind::Version: {
      Json json;
      json.begin_object();
      json.key("name").value("data-center-federation");
      json.key("version").value(version_string());
      json.key("protocol_version").value(protocol_version_string());
      json.key("store_format_version").value(kStoreFormatVersion);
      json.end_object();
      return json.str();
    }
    case wire::QueryKind::Summary:
      return summary_of(state).str();
    case wire::QueryKind::Sites: {
      Json json;
      json.begin_array();
      for (const auto& entry : state.sites()) {
        render_membership(json, entry.second);
      }
      json.end_array();
      return json.str();
    }
    case wire::QueryKind::Site: {
      const auto site = parse_identifier<SiteTag>(argument);
      if (!site.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          "'" + sanitize_for_terminal(argument) + "' is not a site identifier");
      }
      const MembershipRecord* record = state.find_site(site.value());
      if (record == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "site " + argument + " has no membership record");
      }
      Json json;
      json.begin_object();
      json.key("membership");
      render_membership(json, *record);
      json.key("delegations").begin_array();
      for (const auto& entry : state.delegations()) {
        if (entry.second.grant.grantee == site.value()) {
          render_delegation(json, entry.second, state.generation(), UnixMillis{},
                            record->generation, is_partitioned(*record));
        }
      }
      json.end_array();
      json.key("site_authority_digest").value(state.site_authority_digest(site.value()).to_hex());
      json.end_object();
      return json.str();
    }
    case wire::QueryKind::Delegations: {
      Json json;
      json.begin_object();
      json.key("total").value(static_cast<std::uint64_t>(state.delegations().size()));
      json.key("delegations").begin_array();
      std::size_t emitted = 0;
      for (const auto& entry : state.delegations()) {
        if (emitted++ >= kListingLimit) {
          break;
        }
        const MembershipRecord* record = state.find_site(entry.second.grant.grantee);
        const MembershipGeneration membership =
            record == nullptr ? MembershipGeneration{} : record->generation;
        const bool partitioned = record != nullptr && is_partitioned(*record);
        render_delegation(json, entry.second, state.generation(), UnixMillis{}, membership,
                          partitioned);
      }
      json.end_array();
      json.key("truncated").value(emitted > kListingLimit);
      json.end_object();
      return json.str();
    }
    case wire::QueryKind::Windows: {
      Json json;
      json.begin_array();
      for (const auto& entry : state.windows()) {
        render_window(json, entry.second);
      }
      json.end_array();
      return json.str();
    }
    case wire::QueryKind::Reconciliations: {
      Json json;
      json.begin_object();
      json.key("total").value(static_cast<std::uint64_t>(state.reconciliations().size()));
      json.key("reconciliations").begin_array();
      std::size_t emitted = 0;
      for (const auto& entry : state.reconciliations()) {
        if (emitted++ >= kListingLimit) {
          break;
        }
        render_reconciliation(json, entry.second);
      }
      json.end_array();
      json.key("truncated").value(emitted > kListingLimit);
      json.end_object();
      return json.str();
    }
    case wire::QueryKind::Receipts: {
      Json json;
      json.begin_object();
      json.key("total").value(static_cast<std::uint64_t>(state.receipts().size()));
      json.key("receipts").begin_array();
      std::size_t emitted = 0;
      for (const auto& entry : state.receipts()) {
        if (emitted++ >= kListingLimit) {
          break;
        }
        render_receipt(json, entry.second);
      }
      json.end_array();
      json.key("truncated").value(emitted > kListingLimit);
      json.end_object();
      return json.str();
    }
    case wire::QueryKind::Recovery: {
      Json json;
      json.begin_object();
      if (context.recovery != nullptr) {
        const RecoveryReport& report = *context.recovery;
        json.key("reopened").value(report.reopened);
        json.key("torn_tail_bytes_removed").value(report.torn_tail_bytes_removed);
        json.key("journal_segments_read").value(report.journal_segments_read);
        json.key("entries_replayed").value(report.entries_replayed);
        json.key("snapshot_sequence").value(report.snapshot_sequence);
        json.key("snapshot_generation").value(report.snapshot_generation);
        json.key("orphaned_files_removed").begin_array();
        for (const std::string& name : report.orphaned_files_removed) {
          json.value(name);
        }
        json.end_array();
        json.key("state_digest").value(report.state_digest.to_hex());
      } else {
        json.key("available").value(false);
      }
      json.end_object();
      return json.str();
    }
    case wire::QueryKind::Stats: {
      Json json;
      json.begin_object();
      if (context.stats != nullptr) {
        const RuntimeStats& stats = *context.stats;
        json.key("commands_accepted").value(stats.commands_accepted);
        json.key("commands_refused").value(stats.commands_refused);
        json.key("commands_rejected_full").value(stats.commands_rejected_full);
        json.key("commands_rejected_closed").value(stats.commands_rejected_closed);
        json.key("reentrant_submissions").value(stats.reentrant_submissions);
        json.key("durable_commits").value(stats.durable_commits);
        json.key("events_emitted").value(stats.events_emitted);
        json.key("compactions").value(stats.compactions);
        json.key("callback_failures").value(stats.callback_failures);
      } else {
        json.key("available").value(false);
      }
      json.end_object();
      return json.str();
    }
  }
  return make_error(ErrorCode::Unsupported, "that query is not implemented");
}

}  // namespace dcf
