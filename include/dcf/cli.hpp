// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dcf/command.hpp"
#include "dcf/types.hpp"

namespace dcf::cli {

// A parsed command line. Options are plain "--name value" pairs and "--flag"
// switches; positional arguments follow. Nothing here interprets the meaning of
// an option, so every program in the repository reads its arguments the same
// way.
struct OptionSet {
  std::vector<std::pair<std::string, std::string>> options{};
  std::vector<std::string> flags{};
  std::vector<std::string> positional{};

  [[nodiscard]] bool has_flag(std::string_view name) const;
  [[nodiscard]] bool has(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> get(std::string_view name) const;
  [[nodiscard]] std::string get_or(std::string_view name, std::string fallback) const;
  [[nodiscard]] Result<std::string> require(std::string_view name) const;
};

// Parses argv starting at "first". Unknown options are kept, not rejected, so a
// program can decide which ones it understands.
[[nodiscard]] OptionSet parse_options(int argc, char** argv, int first);

[[nodiscard]] Result<std::uint64_t> parse_u64(std::string_view text, const char* what);
[[nodiscard]] Result<std::int64_t> parse_i64(std::string_view text, const char* what);
[[nodiscard]] Result<Version> parse_version(std::string_view text);
// Accepts "1.0..2.5", "1.0" (a single version), or ".." for an unbounded range.
[[nodiscard]] Result<VersionRange> parse_range(std::string_view text);
[[nodiscard]] Result<std::vector<std::string>> split_list(std::string_view text, char separator);

struct CommandRequest {
  Command command{};
  // A verb that is a query rather than a command.
  std::optional<std::string> query{};
  // The argument a query needs, if it needs one.
  std::string query_argument{};
};

// Builds a command from a verb and its options. Every verb this boundary
// implements is listed here; an unknown verb is refused by name rather than
// guessed at.
[[nodiscard]] Result<CommandRequest> build_request(const std::string& verb,
                                                   const OptionSet& options);

[[nodiscard]] std::string usage_text();

}  // namespace dcf::cli
