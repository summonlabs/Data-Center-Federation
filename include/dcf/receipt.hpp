// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <string>

#include "dcf/types.hpp"

namespace dcf {

// The identity of one externally submitted operation. A site retries with the
// same key when it does not know whether its previous attempt was committed.
struct IdempotencyKey {
  FederationId federation{};
  SiteId site{};
  OperationId operation{};

  friend bool operator==(const IdempotencyKey&, const IdempotencyKey&) = default;
  friend auto operator<=>(const IdempotencyKey&, const IdempotencyKey&) = default;
};

// The durable record of an operation that was actually committed. A receipt is
// written in the same journal record as the state change it describes, so a
// receipt can never exist for a change that was not committed, and a change can
// never be committed without the receipt that makes a retry safe.
struct IdempotencyReceipt {
  IdempotencyKey key{};
  // The digest of the decoded request. A retry that carries the same key but a
  // different request is refused rather than answered with the stored outcome.
  Digest request{};
  // The outcome code the original attempt returned. It is replayed verbatim.
  ErrorCode outcome{ErrorCode::None};
  std::string outcome_detail{};
  FederationGeneration generation{};
  MembershipGeneration membership_generation{};
  JournalSequence committed{};
  UnixMillis recorded_at{};

  friend bool operator==(const IdempotencyReceipt&, const IdempotencyReceipt&) = default;
};

}  // namespace dcf
