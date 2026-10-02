// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <string>

#include "dcf/runtime.hpp"
#include "dcf/state.hpp"
#include "dcf/wire.hpp"

namespace dcf {

// What a query is answered from. A query never observes a partially applied
// change: the state pointer is one coherent published snapshot.
struct QueryContext {
  const FederationState* state{nullptr};
  const RuntimeStats* stats{nullptr};
  const RecoveryReport* recovery{nullptr};
};

// Renders a query result as a JSON document. The rendering is a pure function of
// the snapshot and the counters, so the same state always renders identical
// bytes and a report can be compared between runs.
[[nodiscard]] Result<std::string> render_query(wire::QueryKind kind, const std::string& argument,
                                               const QueryContext& context);

// The most receipts or reconciliations a single listing will include, so that a
// report stays bounded even when the state is not.
inline constexpr std::size_t kListingLimit = 512;

[[nodiscard]] std::string render_scope_mask(std::uint16_t mask);

}  // namespace dcf
