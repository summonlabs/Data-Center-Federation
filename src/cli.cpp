// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/cli.hpp"

#include <algorithm>

#include "dcf/hash.hpp"

namespace dcf::cli {
namespace {

[[nodiscard]] Result<std::uint16_t> parse_scopes(const std::string& text) {
  const auto mask = parse_scope_mask(text);
  if (!mask.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      "scope list '" + sanitize_for_terminal(text) +
                          "' contains a name this boundary does not define; the names are " +
                          to_string(all_delegation_scopes()));
  }
  return mask.value();
}

[[nodiscard]] Result<SiteId> require_site(const OptionSet& options) {
  const auto text = options.require("site");
  if (!text) {
    return text.error();
  }
  const auto site = parse_identifier<SiteTag>(text.value());
  if (!site.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      "'" + sanitize_for_terminal(text.value()) + "' is not a site identifier");
  }
  return site.value();
}

[[nodiscard]] Result<DelegationId> require_delegation(const OptionSet& options,
                                                      const char* key) {
  const auto text = options.require(key);
  if (!text) {
    return text.error();
  }
  const auto id = parse_identifier<DelegationTag>(text.value());
  if (!id.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      "'" + sanitize_for_terminal(text.value()) +
                          "' is not a delegation identifier");
  }
  return id.value();
}

}  // namespace

bool OptionSet::has_flag(std::string_view name) const {
  return std::find(flags.begin(), flags.end(), name) != flags.end();
}

bool OptionSet::has(std::string_view name) const {
  return std::any_of(options.begin(), options.end(),
                     [name](const auto& entry) { return entry.first == name; });
}

std::optional<std::string> OptionSet::get(std::string_view name) const {
  for (const auto& entry : options) {
    if (entry.first == name) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::string OptionSet::get_or(std::string_view name, std::string fallback) const {
  const auto value = get(name);
  return value.has_value() ? value.value() : std::move(fallback);
}

Result<std::string> OptionSet::require(std::string_view name) const {
  const auto value = get(name);
  if (!value.has_value()) {
    return make_error(ErrorCode::InvalidArgument,
                      "option --" + std::string(name) + " is required for this command");
  }
  return value.value();
}

OptionSet parse_options(int argc, char** argv, int first) {
  OptionSet set;
  int index = first;
  while (index < argc) {
    const std::string argument(argv[index]);
    if (argument.rfind("--", 0) != 0) {
      set.positional.push_back(argument);
      ++index;
      continue;
    }
    const std::string name = argument.substr(2);
    if (index + 1 < argc) {
      const std::string next(argv[index + 1]);
      if (next.rfind("--", 0) != 0) {
        set.options.emplace_back(name, next);
        index += 2;
        continue;
      }
    }
    set.flags.push_back(name);
    ++index;
  }
  return set;
}

Result<std::uint64_t> parse_u64(std::string_view text, const char* what) {
  if (text.empty()) {
    return make_error(ErrorCode::InvalidArgument, std::string(what) + " is empty");
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return make_error(ErrorCode::InvalidArgument,
                        std::string(what) + " '" + sanitize_for_terminal(std::string(text)) +
                            "' is not a decimal number");
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return make_error(ErrorCode::ArithmeticOverflow, std::string(what) + " overflows");
    }
    value = value * 10U + digit;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view text, const char* what) {
  if (!text.empty() && text.front() == '-') {
    const auto magnitude = parse_u64(text.substr(1), what);
    if (!magnitude) {
      return magnitude.error();
    }
    if (magnitude.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return make_error(ErrorCode::ArithmeticOverflow, std::string(what) + " underflows");
    }
    return -static_cast<std::int64_t>(magnitude.value());
  }
  const auto magnitude = parse_u64(text, what);
  if (!magnitude) {
    return magnitude.error();
  }
  if (magnitude.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return make_error(ErrorCode::ArithmeticOverflow, std::string(what) + " overflows");
  }
  return static_cast<std::int64_t>(magnitude.value());
}

Result<Version> parse_version(std::string_view text) {
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos) {
    const auto major = parse_u64(text, "version");
    if (!major) {
      return major.error();
    }
    if (major.value() > 0xffffffffULL) {
      return make_error(ErrorCode::BoundsExceeded, "version component exceeds 32 bits");
    }
    return Version{static_cast<std::uint32_t>(major.value()), 0};
  }
  const auto major = parse_u64(text.substr(0, dot), "version major");
  if (!major) {
    return major.error();
  }
  const auto minor = parse_u64(text.substr(dot + 1), "version minor");
  if (!minor) {
    return minor.error();
  }
  if (major.value() > 0xffffffffULL || minor.value() > 0xffffffffULL) {
    return make_error(ErrorCode::BoundsExceeded, "version component exceeds 32 bits");
  }
  return Version{static_cast<std::uint32_t>(major.value()), static_cast<std::uint32_t>(minor.value())};
}

Result<VersionRange> parse_range(std::string_view text) {
  const std::size_t separator = text.find("..");
  if (separator == std::string_view::npos) {
    const auto single = parse_version(text);
    if (!single) {
      return single.error();
    }
    return VersionRange{single.value(), single.value()};
  }
  const std::string_view lower = text.substr(0, separator);
  const std::string_view upper = text.substr(separator + 2);
  if (lower.empty() || upper.empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a version range must name both ends, as in 1.0..2.5");
  }
  const auto minimum = parse_version(lower);
  if (!minimum) {
    return minimum.error();
  }
  const auto maximum = parse_version(upper);
  if (!maximum) {
    return maximum.error();
  }
  VersionRange range{minimum.value(), maximum.value()};
  if (range.is_empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "version range '" + sanitize_for_terminal(std::string(text)) + "' is empty");
  }
  return range;
}

Result<std::vector<std::string>> split_list(std::string_view text, char separator) {
  std::vector<std::string> items;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const std::size_t found = text.find(separator, offset);
    const std::size_t stop = found == std::string_view::npos ? text.size() : found;
    std::string item(text.substr(offset, stop - offset));
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.erase(item.begin());
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
      item.pop_back();
    }
    if (!item.empty()) {
      items.push_back(std::move(item));
    }
    if (found == std::string_view::npos) {
      break;
    }
    offset = found + 1;
  }
  return items;
}

std::string usage_text() {
  return
      "  status                                  show the federation summary\n"
      "  sites                                   list every membership record\n"
      "  site --site HEX                         show one site and its delegations\n"
      "  delegations                             list delegation records\n"
      "  windows                                 list compatibility windows\n"
      "  reconciliations                         list reconciliation records\n"
      "  receipts                                list durable idempotency receipts\n"
      "  recovery                                show what startup recovery did\n"
      "  stats                                   show runtime counters\n"
      "  version                                 show the library and protocol version\n"
      "\n"
      "  validate --site HEX                     evaluate the site compatibility declaration\n"
      "  admit --site HEX --generation N         admit a validated site\n"
      "  constrain --site HEX [--scopes LIST] [--reason TEXT]\n"
      "  drain --site HEX [--reason TEXT]\n"
      "  cancel-drain --site HEX [--reason TEXT]\n"
      "  remove --site HEX [--reason TEXT]\n"
      "  connectivity --site HEX --link STATE    connected|degraded|partitioned|unknown\n"
      "  window --capability NAME --offered RANGE [--from N] [--soft N] [--hard N]\n"
      "  grant --delegation HEX --site HEX --scopes LIST\n"
      "        [--selectors A,B] [--expires N] [--wall-ms MS]\n"
      "        [--survives-partition] [--exclusive] [--policy HEX]\n"
      "  revoke --delegation HEX [--reason TEXT]\n"
      "  resolve --reconciliation HEX --choice federation|local|reissue --authority TEXT\n"
      "\n"
      "  Every command accepts --operation HEX for a caller-supplied idempotency\n"
      "  identifier. A retry with the same identifier and the same request replays\n"
      "  the recorded outcome; a retry with a different request is refused.\n";
}

Result<CommandRequest> build_request(const std::string& verb, const OptionSet& options) {
  CommandRequest request;
  Command& command = request.command;

  const auto query = [&request, &verb]() {
    request.query = verb;
    return request;
  };

  if (verb == "version" || verb == "status" || verb == "sites" || verb == "delegations" ||
      verb == "windows" || verb == "reconciliations" || verb == "receipts" ||
      verb == "recovery" || verb == "stats") {
    return query();
  }

  if (verb == "site") {
    const auto text = options.require("site");
    if (!text) {
      return text.error();
    }
    if (!parse_identifier<SiteTag>(text.value()).has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        "'" + sanitize_for_terminal(text.value()) + "' is not a site identifier");
    }
    request.query = "site";
    request.query_argument = text.value();
    return request;
  }

  if (verb == "validate") {
    const auto site = require_site(options);
    if (!site) {
      return site.error();
    }
    command.payload = ValidateSiteCommand{site.value()};
    return request;
  }

  if (verb == "admit") {
    const auto site = require_site(options);
    if (!site) {
      return site.error();
    }
    const auto generation = options.require("generation");
    if (!generation) {
      return generation.error();
    }
    const auto value = parse_u64(generation.value(), "membership generation");
    if (!value) {
      return value.error();
    }
    command.payload = AdmitSiteCommand{site.value(), MembershipGeneration{value.value()}};
    return request;
  }

  if (verb == "constrain") {
    const auto site = require_site(options);
    if (!site) {
      return site.error();
    }
    std::uint16_t scopes = 0;
    if (const auto text = options.get("scopes"); text.has_value()) {
      const auto parsed = parse_scopes(text.value());
      if (!parsed) {
        return parsed.error();
      }
      scopes = parsed.value();
    }
    command.payload = ConstrainSiteCommand{site.value(), scopes, options.get_or("reason", "")};
    return request;
  }

  if (verb == "drain" || verb == "cancel-drain" || verb == "remove") {
    const auto site = require_site(options);
    if (!site) {
      return site.error();
    }
    const std::string reason = options.get_or("reason", "");
    if (verb == "drain") {
      command.payload = BeginDrainCommand{site.value(), reason};
    } else if (verb == "cancel-drain") {
      command.payload = CancelDrainCommand{site.value(), reason};
    } else {
      command.payload = RemoveSiteCommand{site.value(), reason};
    }
    return request;
  }

  if (verb == "connectivity") {
    const auto site = require_site(options);
    if (!site) {
      return site.error();
    }
    const auto link = options.require("link");
    if (!link) {
      return link.error();
    }
    LinkState state = LinkState::Unknown;
    if (link.value() == "connected") {
      state = LinkState::Connected;
    } else if (link.value() == "degraded") {
      state = LinkState::Degraded;
    } else if (link.value() == "partitioned") {
      state = LinkState::Partitioned;
    } else if (link.value() == "unknown") {
      state = LinkState::Unknown;
    } else {
      return make_error(ErrorCode::InvalidArgument,
                        "link state '" + sanitize_for_terminal(link.value()) +
                            "' is not connected, degraded, partitioned or unknown");
    }
    command.payload = SetConnectivityCommand{site.value(), state};
    return request;
  }

  if (verb == "window") {
    const auto capability = options.require("capability");
    if (!capability) {
      return capability.error();
    }
    const auto offered = options.require("offered");
    if (!offered) {
      return offered.error();
    }
    const auto range = parse_range(offered.value());
    if (!range) {
      return range.error();
    }
    CompatibilityWindow window;
    window.capability = CapabilityId{capability.value()};
    window.offered = range.value();
    const auto read_generation = [&options](const char* key, const char* what)
        -> Result<FederationGeneration> {
      const auto text = options.get(key);
      if (!text.has_value()) {
        return FederationGeneration{};
      }
      const auto value = parse_u64(text.value(), what);
      if (!value) {
        return value.error();
      }
      return FederationGeneration{value.value()};
    };
    const auto effective = read_generation("from", "effective generation");
    if (!effective) {
      return effective.error();
    }
    const auto soft = read_generation("soft", "soft deprecation generation");
    if (!soft) {
      return soft.error();
    }
    const auto hard = read_generation("hard", "hard removal generation");
    if (!hard) {
      return hard.error();
    }
    window.effective_from = effective.value();
    window.soft_deprecate = soft.value();
    window.hard_remove = hard.value();
    command.payload = DeclareWindowCommand{window};
    return request;
  }

  if (verb == "grant") {
    const auto id = require_delegation(options, "delegation");
    if (!id) {
      return id.error();
    }
    const auto site = require_site(options);
    if (!site) {
      return site.error();
    }
    const auto scopes = options.require("scopes");
    if (!scopes) {
      return scopes.error();
    }
    const auto mask = parse_scopes(scopes.value());
    if (!mask) {
      return mask.error();
    }
    DelegationGrant grant;
    grant.id = id.value();
    grant.grantee = site.value();
    grant.scope_mask = mask.value();
    if (const auto selectors = options.get("selectors"); selectors.has_value()) {
      const auto items = split_list(selectors.value(), ',');
      if (!items) {
        return items.error();
      }
      grant.selectors = items.value();
    }
    if (const auto expires = options.get("expires"); expires.has_value()) {
      const auto value = parse_u64(expires.value(), "expiry generation");
      if (!value) {
        return value.error();
      }
      grant.expires_at = FederationGeneration{value.value()};
    }
    if (const auto wall = options.get("wall-ms"); wall.has_value()) {
      const auto value = parse_i64(wall.value(), "wall-clock expiry");
      if (!value) {
        return value.error();
      }
      grant.expires_at_wall = UnixMillis{value.value()};
    }
    if (const auto membership = options.get("membership-generation"); membership.has_value()) {
      const auto value = parse_u64(membership.value(), "membership generation");
      if (!value) {
        return value.error();
      }
      grant.grantee_membership_generation = MembershipGeneration{value.value()};
    }
    grant.survives_partition = options.has_flag("survives-partition");
    grant.exclusive = options.has_flag("exclusive");
    if (const auto policy = options.get("policy"); policy.has_value()) {
      const auto digest = Digest::from_hex(policy.value());
      if (!digest.has_value()) {
        return make_error(ErrorCode::InvalidArgument,
                          "'" + sanitize_for_terminal(policy.value()) +
                              "' is not a policy reference digest");
      }
      grant.policy_reference = digest.value();
    }
    command.payload = GrantDelegationCommand{grant};
    return request;
  }

  if (verb == "revoke") {
    const auto id = require_delegation(options, "delegation");
    if (!id) {
      return id.error();
    }
    command.payload = RevokeDelegationCommand{id.value(), options.get_or("reason", "")};
    return request;
  }

  if (verb == "resolve") {
    const auto text = options.require("reconciliation");
    if (!text) {
      return text.error();
    }
    const auto id = parse_identifier<ReconciliationTag>(text.value());
    if (!id.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        "'" + sanitize_for_terminal(text.value()) +
                            "' is not a reconciliation identifier");
    }
    const auto choice = options.require("choice");
    if (!choice) {
      return choice.error();
    }
    ConflictResolution resolution;
    resolution.reconciliation = id.value();
    if (choice.value() == "federation") {
      resolution.choice = ConflictResolution::Choice::FederationAuthoritative;
    } else if (choice.value() == "local") {
      resolution.choice = ConflictResolution::Choice::LocalScopeAuthoritative;
    } else if (choice.value() == "reissue") {
      resolution.choice = ConflictResolution::Choice::ReissueExclusiveToSingleHolder;
    } else {
      return make_error(ErrorCode::InvalidArgument,
                        "resolution choice '" + sanitize_for_terminal(choice.value()) +
                            "' is not federation, local or reissue");
    }
    const auto authority = options.require("authority");
    if (!authority) {
      return authority.error();
    }
    resolution.authority_reference = authority.value();
    command.payload = ResolveConflictCommand{resolution};
    return request;
  }

  return make_error(ErrorCode::InvalidArgument,
                    "verb '" + sanitize_for_terminal(verb) + "' is not a command this tool has");
}

}  // namespace dcf::cli
