// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/json.hpp"

#include <cstdio>

#include "dcf/hash.hpp"

namespace dcf {
namespace {

void append_hex4(std::string& out, std::uint32_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  out.append("\\u");
  out.push_back(kDigits[(value >> 12) & 0xFU]);
  out.push_back(kDigits[(value >> 8) & 0xFU]);
  out.push_back(kDigits[(value >> 4) & 0xFU]);
  out.push_back(kDigits[value & 0xFU]);
}

}  // namespace

std::string escape_json_string(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte == '"') {
      out.append("\\\"");
      ++index;
      continue;
    }
    if (byte == '\\') {
      out.append("\\\\");
      ++index;
      continue;
    }
    if (byte == '\n') {
      out.append("\\n");
      ++index;
      continue;
    }
    if (byte == '\r') {
      out.append("\\r");
      ++index;
      continue;
    }
    if (byte == '\t') {
      out.append("\\t");
      ++index;
      continue;
    }
    if (byte < 0x20U || byte == 0x7fU) {
      append_hex4(out, byte);
      ++index;
      continue;
    }
    if (byte < 0x80U) {
      out.push_back(static_cast<char>(byte));
      ++index;
      continue;
    }
    // Multi-byte sequences are validated before they are copied, so the writer
    // can never emit a byte string that is not well-formed UTF-8.
    std::size_t length = 0;
    if ((byte & 0xe0U) == 0xc0U) {
      length = 2;
    } else if ((byte & 0xf0U) == 0xe0U) {
      length = 3;
    } else if ((byte & 0xf8U) == 0xf0U) {
      length = 4;
    }
    if (length == 0 || index + length > text.size() ||
        !is_valid_utf8(text.substr(index, length))) {
      out.append("\\ufffd");
      ++index;
      continue;
    }
    out.append(text.substr(index, length));
    index += length;
  }
  out.push_back('"');
  return out;
}

void Json::separate() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!stack_.empty()) {
    if (stack_.back()) {
      stack_.back() = false;
    } else {
      out_.push_back(',');
    }
  }
}

Json& Json::begin_object() {
  separate();
  out_.push_back('{');
  stack_.push_back(true);
  return *this;
}

Json& Json::end_object() {
  if (!stack_.empty()) {
    stack_.pop_back();
  }
  out_.push_back('}');
  return *this;
}

Json& Json::begin_array() {
  separate();
  out_.push_back('[');
  stack_.push_back(true);
  return *this;
}

Json& Json::end_array() {
  if (!stack_.empty()) {
    stack_.pop_back();
  }
  out_.push_back(']');
  return *this;
}

Json& Json::key(std::string_view name) {
  separate();
  out_.append(escape_json_string(name));
  out_.push_back(':');
  after_key_ = true;
  return *this;
}

Json& Json::value(std::string_view text) {
  separate();
  out_.append(escape_json_string(text));
  return *this;
}

Json& Json::value(const char* text) {
  return value(text == nullptr ? std::string_view{} : std::string_view(text));
}

Json& Json::value(std::uint64_t number) {
  separate();
  out_.append(std::to_string(number));
  return *this;
}

Json& Json::value(std::uint32_t number) {
  separate();
  out_.append(std::to_string(number));
  return *this;
}

Json& Json::value(std::int64_t number) {
  separate();
  out_.append(std::to_string(number));
  return *this;
}

Json& Json::value(int number) {
  separate();
  out_.append(std::to_string(number));
  return *this;
}

Json& Json::value(bool flag) {
  separate();
  out_.append(flag ? "true" : "false");
  return *this;
}

Json& Json::null_value() {
  separate();
  out_.append("null");
  return *this;
}

Json& Json::raw(std::string_view fragment) {
  separate();
  out_.append(fragment);
  return *this;
}

}  // namespace dcf
