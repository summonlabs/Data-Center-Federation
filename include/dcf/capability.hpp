// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dcf/types.hpp"

namespace dcf {

// ---------------------------------------------------------------------------
// Capability names
// ---------------------------------------------------------------------------
// A capability is named by a stable, machine-readable name rather than by a
// version number of the whole product. Two sites are compatible when the
// capabilities each of them needs are capabilities the other offers, which is a
// different question from "are these two builds the same build".
//
// Names are lower-case ASCII with '.', '_' and '-' separators, at least one
// character, at most 128 bytes, and never start or end with a separator and
// never contain two separators in a row. Names are compared bytewise, so the
// canonical order is the bytewise order.
struct CapabilityId {
  std::string value{};

  friend bool operator==(const CapabilityId&, const CapabilityId&) = default;
  friend auto operator<=>(const CapabilityId&, const CapabilityId&) = default;

  [[nodiscard]] bool empty() const noexcept { return value.empty(); }
};

[[nodiscard]] bool is_valid_capability_name(std::string_view name) noexcept;

enum class CapabilityRequirement : std::uint8_t {
  // The declaring site cannot participate without this capability.
  Required = 0,
  // The declaring site participates with reduced function without it.
  Optional = 1,
};

[[nodiscard]] std::string_view to_string(CapabilityRequirement requirement) noexcept;

// One capability as declared by a site: the name, the inclusive range of
// capability versions the site implements, and whether the site requires the
// federation to offer it.
struct CapabilityDeclaration {
  CapabilityId id{};
  VersionRange supported{};
  CapabilityRequirement requirement{CapabilityRequirement::Required};

  friend bool operator==(const CapabilityDeclaration&, const CapabilityDeclaration&) = default;
  friend auto operator<=>(const CapabilityDeclaration&, const CapabilityDeclaration&) = default;
};

// A typed reference to a policy domain the site's local policy conforms to.
// The federation records the reference; it never interprets the policy, because
// global policy semantics belong to another boundary.
struct PolicyCompatibilityRef {
  std::string domain{};
  VersionRange version{};

  friend bool operator==(const PolicyCompatibilityRef&, const PolicyCompatibilityRef&) = default;
  friend auto operator<=>(const PolicyCompatibilityRef&, const PolicyCompatibilityRef&) = default;
};

// Everything a site tells the federation about itself when it asks to join.
struct CompatibilityDeclaration {
  std::string implementation{};
  Version implementation_version{};
  VersionRange protocol{};
  std::vector<CapabilityDeclaration> capabilities{};
  std::vector<PolicyCompatibilityRef> policies{};

  friend bool operator==(const CompatibilityDeclaration&, const CompatibilityDeclaration&) = default;
};

// Orders capabilities by name and removes duplicates. A duplicate name with two
// different declarations is refused rather than resolved: the federation will
// not guess which of a site's two answers about itself is the real one.
[[nodiscard]] Result<CompatibilityDeclaration> canonicalize(CompatibilityDeclaration declaration,
                                                           const Limits& limits);

[[nodiscard]] Digest declaration_digest(const CompatibilityDeclaration& declaration);

// ---------------------------------------------------------------------------
// Staged compatibility windows
// ---------------------------------------------------------------------------
// The federation publishes, per capability, the range of versions it offers and
// the federation generations at which that offer changes. The window makes
// compatibility a function of time rather than a single global version, so a
// fleet can be migrated in stages without a flag day.
struct CompatibilityWindow {
  CapabilityId capability{};
  VersionRange offered{};
  // The window is in force from this federation generation, inclusive.
  FederationGeneration effective_from{};
  // From this federation generation, inclusive, the capability is deprecated:
  // sites that already hold it keep working with narrowed scope, and sites that
  // are not yet admitted are refused.
  FederationGeneration soft_deprecate{};
  // From this federation generation, inclusive, the capability is gone.
  FederationGeneration hard_remove{};

  friend bool operator==(const CompatibilityWindow&, const CompatibilityWindow&) = default;
  friend auto operator<=>(const CompatibilityWindow&, const CompatibilityWindow&) = default;

  // A window with a zero effective_from is in force from the first generation.
  [[nodiscard]] bool in_force_at(FederationGeneration generation) const noexcept {
    return effective_from <= generation;
  }
  // A zero soft_deprecate or hard_remove stage means the stage was not scheduled.
  // Zero is not generation zero: treating it as a generation would make every
  // window that never scheduled a removal look as though it had already removed
  // the capability.
  [[nodiscard]] bool deprecated_at(FederationGeneration generation) const noexcept {
    return in_force_at(generation) && !soft_deprecate.is_zero() && soft_deprecate <= generation;
  }
  [[nodiscard]] bool removed_at(FederationGeneration generation) const noexcept {
    return in_force_at(generation) && !hard_remove.is_zero() && hard_remove <= generation;
  }
};

// ---------------------------------------------------------------------------
// Admission evaluation
// ---------------------------------------------------------------------------
enum class CompatibilityFindingCode : std::uint8_t {
  DeclarationEmpty,
  ProtocolRangeDisjoint,
  ProtocolVersionUnsupported,
  RequiredCapabilityMissing,
  RequiredCapabilityVersionUnsatisfied,
  CapabilityUnsupportedByFederation,
  CapabilityDeprecated,
  CapabilityRemoved,
  OptionalCapabilityMissing,
  PolicyDomainUnknown,
  PolicyVersionUnsatisfied,
  CapabilitySetTooLarge,
};

[[nodiscard]] std::string_view to_string(CompatibilityFindingCode code) noexcept;

// One reason the evaluation reached its decision. Findings carry enough detail
// to be explained to an operator without re-running the evaluation.
struct CompatibilityFinding {
  CompatibilityFindingCode code{CompatibilityFindingCode::DeclarationEmpty};
  CapabilityId subject{};
  std::string detail{};

  friend bool operator==(const CompatibilityFinding&, const CompatibilityFinding&) = default;
  friend auto operator<=>(const CompatibilityFinding&, const CompatibilityFinding&) = default;
};

enum class AdmissionDecision : std::uint8_t {
  // The site may be admitted and may take part in every capability it asked for.
  Admitted = 0,
  // The site may be admitted, but at least one capability is narrower than the
  // site asked for. The caller must record the narrowed scope.
  AdmittedConstrained = 1,
  // The site may not be admitted. The findings say exactly why.
  Refused = 2,
};

[[nodiscard]] std::string_view to_string(AdmissionDecision decision) noexcept;

struct CompatibilityReport {
  AdmissionDecision decision{AdmissionDecision::Refused};
  std::vector<CompatibilityFinding> findings{};
  Digest declaration{};

  friend bool operator==(const CompatibilityReport&, const CompatibilityReport&) = default;
};

// Evaluates a declaration against the windows in force. The evaluation is a
// pure function of its arguments: the same declaration, the same windows, and
// the same generation always produce the same report, including the order of
// the findings, so a refusal can be reproduced exactly from the journal.
[[nodiscard]] CompatibilityReport evaluate_compatibility(
    const CompatibilityDeclaration& declaration,
    const std::vector<CompatibilityWindow>& windows,
    FederationGeneration at,
    const Limits& limits);

}  // namespace dcf
