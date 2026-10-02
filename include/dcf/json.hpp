// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dcf {

// A minimal, first-party JSON writer for operator output. It exists so that the
// command line tools can report state without a third-party dependency, and it
// is deliberately strict: every string is escaped, invalid input is replaced
// rather than emitted raw, and the result is always well-formed.
class Json {
 public:
  Json& begin_object();
  Json& end_object();
  Json& begin_array();
  Json& end_array();

  Json& key(std::string_view name);
  Json& value(std::string_view text);
  Json& value(const char* text);
  Json& value(std::uint64_t number);
  Json& value(std::uint32_t number);
  Json& value(std::int64_t number);
  Json& value(int number);
  Json& value(bool flag);
  Json& null_value();
  // Inserts an already-encoded fragment. Used only with fragments this file
  // produced.
  Json& raw(std::string_view fragment);

  [[nodiscard]] const std::string& str() const noexcept { return out_; }
  [[nodiscard]] bool balanced() const noexcept { return stack_.empty(); }

 private:
  void separate();

  std::string out_{};
  std::vector<bool> stack_{};
  bool after_key_{false};
};

[[nodiscard]] std::string escape_json_string(std::string_view text);

}  // namespace dcf
