// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/capability.hpp"

#include <algorithm>
#include <optional>

#include "dcf/codec.hpp"
#include "dcf/hash.hpp"
#include "dcf/version.hpp"

namespace dcf {
namespace {

[[nodiscard]] bool is_name_character(char character) noexcept {
  const bool lower = character >= 'a' && character <= 'z';
  const bool digit = character >= '0' && character <= '9';
  return lower || digit || character == '.' || character == '_' || character == '-';
}

[[nodiscard]] bool is_separator(char character) noexcept {
  return character == '.' || character == '_' || character == '-';
}

void add_finding(std::vector<CompatibilityFinding>& findings, CompatibilityFindingCode code,
                 const CapabilityId& subject, std::string detail) {
  findings.push_back(CompatibilityFinding{code, subject, std::move(detail)});
}

}  // namespace

bool is_valid_capability_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 128) {
    return false;
  }
  if (is_separator(name.front()) || is_separator(name.back())) {
    return false;
  }
  bool previous_separator = false;
  for (const char character : name) {
    if (!is_name_character(character)) {
      return false;
    }
    const bool separator = is_separator(character);
    if (separator && previous_separator) {
      return false;
    }
    previous_separator = separator;
  }
  return true;
}

std::string_view to_string(CapabilityRequirement requirement) noexcept {
  return requirement == CapabilityRequirement::Required ? "required" : "optional";
}

Result<CompatibilityDeclaration> canonicalize(CompatibilityDeclaration declaration,
                                              const Limits& limits) {
  if (declaration.implementation.empty() || declaration.implementation.size() > 128) {
    return make_error(ErrorCode::InvalidArgument,
                      "implementation name must be 1..128 bytes and was " +
                          std::to_string(declaration.implementation.size()));
  }
  if (!is_valid_utf8(declaration.implementation)) {
    return make_error(ErrorCode::InvalidArgument, "implementation name is not valid UTF-8");
  }
  if (declaration.protocol.is_empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "protocol range is empty: minimum " +
                          declaration.protocol.minimum.to_string() + " is above maximum " +
                          declaration.protocol.maximum.to_string());
  }
  if (static_cast<std::uint64_t>(declaration.capabilities.size()) >
      limits.max_capabilities_per_site) {
    return make_error(ErrorCode::BoundsExceeded,
                      "declaration carries " + std::to_string(declaration.capabilities.size()) +
                          " capabilities which exceeds the limit of " +
                          std::to_string(limits.max_capabilities_per_site));
  }

  for (const CapabilityDeclaration& entry : declaration.capabilities) {
    if (!is_valid_capability_name(entry.id.value)) {
      return make_error(ErrorCode::InvalidArgument,
                        "capability name '" + sanitize_for_terminal(entry.id.value) +
                            "' is not a valid capability name");
    }
    if (entry.supported.is_empty()) {
      return make_error(ErrorCode::InvalidArgument,
                        "capability '" + entry.id.value + "' declares an empty version range");
    }
  }

  for (const PolicyCompatibilityRef& policy : declaration.policies) {
    if (!is_valid_capability_name(policy.domain)) {
      return make_error(ErrorCode::InvalidArgument,
                        "policy domain '" + sanitize_for_terminal(policy.domain) +
                            "' is not a valid domain name");
    }
    if (policy.version.is_empty()) {
      return make_error(ErrorCode::InvalidArgument,
                        "policy domain '" + policy.domain + "' declares an empty version range");
    }
  }

  std::sort(declaration.capabilities.begin(), declaration.capabilities.end(),
            [](const CapabilityDeclaration& lhs, const CapabilityDeclaration& rhs) {
              return lhs.id < rhs.id;
            });
  for (std::size_t index = 1; index < declaration.capabilities.size(); ++index) {
    if (declaration.capabilities[index - 1].id == declaration.capabilities[index].id) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "capability '" + declaration.capabilities[index].id.value +
                            "' is declared more than once");
    }
  }

  std::sort(declaration.policies.begin(), declaration.policies.end(),
            [](const PolicyCompatibilityRef& lhs, const PolicyCompatibilityRef& rhs) {
              return lhs.domain < rhs.domain;
            });
  for (std::size_t index = 1; index < declaration.policies.size(); ++index) {
    if (declaration.policies[index - 1].domain == declaration.policies[index].domain) {
      return make_error(ErrorCode::DuplicateIdentity,
                        "policy domain '" + declaration.policies[index].domain +
                            "' is declared more than once");
    }
  }

  return declaration;
}

Digest declaration_digest(const CompatibilityDeclaration& declaration) {
  Encoder encoder;
  encoder.text(declaration.implementation);
  encoder.u32(declaration.implementation_version.major);
  encoder.u32(declaration.implementation_version.minor);
  encoder.u32(declaration.protocol.minimum.major);
  encoder.u32(declaration.protocol.minimum.minor);
  encoder.u32(declaration.protocol.maximum.major);
  encoder.u32(declaration.protocol.maximum.minor);
  encoder.u32(static_cast<std::uint32_t>(declaration.capabilities.size()));
  for (const CapabilityDeclaration& entry : declaration.capabilities) {
    encoder.text(entry.id.value);
    encoder.u32(entry.supported.minimum.major);
    encoder.u32(entry.supported.minimum.minor);
    encoder.u32(entry.supported.maximum.major);
    encoder.u32(entry.supported.maximum.minor);
    encoder.u8(static_cast<std::uint8_t>(entry.requirement));
  }
  encoder.u32(static_cast<std::uint32_t>(declaration.policies.size()));
  for (const PolicyCompatibilityRef& policy : declaration.policies) {
    encoder.text(policy.domain);
    encoder.u32(policy.version.minimum.major);
    encoder.u32(policy.version.minimum.minor);
    encoder.u32(policy.version.maximum.major);
    encoder.u32(policy.version.maximum.minor);
  }
  return digest_of(to_string(encoder.bytes()));
}

std::string_view to_string(CompatibilityFindingCode code) noexcept {
  switch (code) {
    case CompatibilityFindingCode::DeclarationEmpty:
      return "declaration_empty";
    case CompatibilityFindingCode::ProtocolRangeDisjoint:
      return "protocol_range_disjoint";
    case CompatibilityFindingCode::ProtocolVersionUnsupported:
      return "protocol_version_unsupported";
    case CompatibilityFindingCode::RequiredCapabilityMissing:
      return "required_capability_missing";
    case CompatibilityFindingCode::RequiredCapabilityVersionUnsatisfied:
      return "required_capability_version_unsatisfied";
    case CompatibilityFindingCode::CapabilityUnsupportedByFederation:
      return "capability_unsupported_by_federation";
    case CompatibilityFindingCode::CapabilityDeprecated:
      return "capability_deprecated";
    case CompatibilityFindingCode::CapabilityRemoved:
      return "capability_removed";
    case CompatibilityFindingCode::OptionalCapabilityMissing:
      return "optional_capability_missing";
    case CompatibilityFindingCode::PolicyDomainUnknown:
      return "policy_domain_unknown";
    case CompatibilityFindingCode::PolicyVersionUnsatisfied:
      return "policy_version_unsatisfied";
    case CompatibilityFindingCode::CapabilitySetTooLarge:
      return "capability_set_too_large";
  }
  return "unknown";
}

std::string_view to_string(AdmissionDecision decision) noexcept {
  switch (decision) {
    case AdmissionDecision::Admitted:
      return "admitted";
    case AdmissionDecision::AdmittedConstrained:
      return "admitted_constrained";
    case AdmissionDecision::Refused:
      return "refused";
  }
  return "unknown";
}

CompatibilityReport evaluate_compatibility(const CompatibilityDeclaration& declaration,
                                           const std::vector<CompatibilityWindow>& windows,
                                           FederationGeneration at, const Limits& limits) {
  CompatibilityReport report;
  report.declaration = declaration_digest(declaration);

  if (declaration.capabilities.empty()) {
    add_finding(report.findings, CompatibilityFindingCode::DeclarationEmpty, CapabilityId{},
                "the site declares no capabilities at all");
  }
  if (static_cast<std::uint64_t>(declaration.capabilities.size()) >
      limits.max_capabilities_per_site) {
    add_finding(report.findings, CompatibilityFindingCode::CapabilitySetTooLarge, CapabilityId{},
                "the site declares " + std::to_string(declaration.capabilities.size()) +
                    " capabilities which exceeds the per-site limit of " +
                    std::to_string(limits.max_capabilities_per_site));
  }

  // A window is addressed by capability name. Windows are supplied in canonical
  // order by the caller; the search below relies on that order only for
  // efficiency, not for correctness of the result.
  std::vector<const CompatibilityWindow*> matching;
  for (const CompatibilityWindow& window : windows) {
    if (window.in_force_at(at)) {
      matching.push_back(&window);
    }
  }
  std::sort(matching.begin(), matching.end(),
            [](const CompatibilityWindow* lhs, const CompatibilityWindow* rhs) {
              return lhs->capability < rhs->capability;
            });

  const auto find_window = [&matching](const CapabilityId& id) -> const CompatibilityWindow* {
    const auto iterator =
        std::lower_bound(matching.begin(), matching.end(), id,
                         [](const CompatibilityWindow* candidate, const CapabilityId& key) {
                           return candidate->capability < key;
                         });
    if (iterator == matching.end() || !((*iterator)->capability == id)) {
      return nullptr;
    }
    return *iterator;
  };

  // The protocol range is the one capability every site has and which cannot be
  // optional: a site that cannot speak the federation protocol cannot join.
  const auto supported_protocol = Version{kProtocolVersionMajor, kProtocolVersionMinor};
  if (!declaration.protocol.contains(supported_protocol)) {
    add_finding(report.findings,
                declaration.protocol.intersects(VersionRange{supported_protocol, supported_protocol})
                    ? CompatibilityFindingCode::ProtocolVersionUnsupported
                    : CompatibilityFindingCode::ProtocolRangeDisjoint,
                CapabilityId{},
                "the site supports federation protocol versions " +
                    declaration.protocol.minimum.to_string() + ".." +
                    declaration.protocol.maximum.to_string() + " and this federation speaks " +
                    std::to_string(kProtocolVersionMajor) + "." +
                    std::to_string(kProtocolVersionMinor));
  }

  bool constrained = false;

  for (const CapabilityDeclaration& entry : declaration.capabilities) {
    const CompatibilityWindow* window = find_window(entry.id);

    if (window == nullptr) {
      if (entry.requirement == CapabilityRequirement::Required) {
        add_finding(report.findings, CompatibilityFindingCode::RequiredCapabilityMissing, entry.id,
                    "the federation publishes no window for required capability '" + entry.id.value +
                        "'");
      } else {
        add_finding(report.findings, CompatibilityFindingCode::OptionalCapabilityMissing, entry.id,
                    "the federation publishes no window for optional capability '" + entry.id.value +
                        "'");
        constrained = true;
      }
      continue;
    }

    if (window->removed_at(at)) {
      add_finding(report.findings, CompatibilityFindingCode::CapabilityRemoved, entry.id,
                  "capability '" + entry.id.value + "' was removed at federation generation " +
                      std::to_string(window->hard_remove.value()) + " and this evaluation is at " +
                      std::to_string(at.value()));
      continue;
    }

    if (window->deprecated_at(at)) {
      add_finding(report.findings, CompatibilityFindingCode::CapabilityDeprecated, entry.id,
                  "capability '" + entry.id.value +
                      "' is deprecated from federation generation " +
                      std::to_string(window->soft_deprecate.value()));
      constrained = true;
    }

    if (!window->offered.intersects(entry.supported)) {
      add_finding(report.findings,
                  entry.requirement == CapabilityRequirement::Required
                      ? CompatibilityFindingCode::RequiredCapabilityVersionUnsatisfied
                      : CompatibilityFindingCode::CapabilityUnsupportedByFederation,
                  entry.id,
                  "capability '" + entry.id.value + "' is offered as " +
                      window->offered.minimum.to_string() + ".." +
                      window->offered.maximum.to_string() + " and declared as " +
                      entry.supported.minimum.to_string() + ".." +
                      entry.supported.maximum.to_string());
      if (entry.requirement == CapabilityRequirement::Optional) {
        constrained = true;
      }
      continue;
    }

    // The window may name a capability that no site asked for. That is not a
    // finding: the federation offers capabilities, it does not require sites to
    // use them.
  }

  // Policies are references, not semantics. A policy domain the federation has
  // never published a window for is reported, because silently accepting an
  // unknown policy reference would be claiming a compatibility the federation
  // cannot vouch for.
  for (const PolicyCompatibilityRef& policy : declaration.policies) {
    const CompatibilityWindow* window =
        find_window(CapabilityId{"policy." + policy.domain});
    if (window == nullptr) {
      add_finding(report.findings, CompatibilityFindingCode::PolicyDomainUnknown,
                  CapabilityId{policy.domain},
                  "the site references policy domain '" + policy.domain +
                      "' and the federation publishes no window for it");
      constrained = true;
      continue;
    }
    if (!window->offered.intersects(policy.version)) {
      add_finding(report.findings, CompatibilityFindingCode::PolicyVersionUnsatisfied,
                  CapabilityId{policy.domain},
                  "policy domain '" + policy.domain + "' is offered as " +
                      window->offered.minimum.to_string() + ".." +
                      window->offered.maximum.to_string() + " and referenced as " +
                      policy.version.minimum.to_string() + ".." +
                      policy.version.maximum.to_string());
    }
  }

  std::sort(report.findings.begin(), report.findings.end());

  const bool refused = std::any_of(
      report.findings.begin(), report.findings.end(), [](const CompatibilityFinding& finding) {
        switch (finding.code) {
          case CompatibilityFindingCode::DeclarationEmpty:
          case CompatibilityFindingCode::ProtocolRangeDisjoint:
          case CompatibilityFindingCode::ProtocolVersionUnsupported:
          case CompatibilityFindingCode::RequiredCapabilityMissing:
          case CompatibilityFindingCode::RequiredCapabilityVersionUnsatisfied:
          case CompatibilityFindingCode::CapabilityRemoved:
          case CompatibilityFindingCode::PolicyVersionUnsatisfied:
          case CompatibilityFindingCode::CapabilitySetTooLarge:
            return true;
          default:
            return false;
        }
      });

  if (refused) {
    report.decision = AdmissionDecision::Refused;
  } else if (constrained || !report.findings.empty()) {
    report.decision = AdmissionDecision::AdmittedConstrained;
  } else {
    report.decision = AdmissionDecision::Admitted;
  }
  return report;
}

}  // namespace dcf
