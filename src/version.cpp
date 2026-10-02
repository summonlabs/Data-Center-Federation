// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/version.hpp"

namespace dcf {

std::string version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
         std::to_string(kVersionPatch);
}

std::string protocol_version_string() {
  return std::to_string(kProtocolVersionMajor) + "." + std::to_string(kProtocolVersionMinor);
}

}  // namespace dcf
