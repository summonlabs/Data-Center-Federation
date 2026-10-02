// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>

namespace dcf {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// The version of the federation wire protocol carried between the federation
// runtime and site runtimes. It is versioned separately from the library so
// that a protocol change is visible in compatibility declarations.
inline constexpr std::uint32_t kProtocolVersionMajor = 1;
inline constexpr std::uint32_t kProtocolVersionMinor = 0;

[[nodiscard]] std::string version_string();
[[nodiscard]] std::string protocol_version_string();

}  // namespace dcf
