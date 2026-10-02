// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dcf/command.hpp"
#include "dcf/state.hpp"
#include "dcf/types.hpp"

namespace dcf {

// The first durable record of a federation. It is a journal entry like every
// other, so recovery has exactly one code path.
[[nodiscard]] JournalEntry genesis_entry(FederationId federation, UnixMillis created);

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------
// A pure, single-threaded state machine. It has no threads, no locks, and no
// I/O: it decides what a command means and what durable record that decision
// would require. The runtime owns the durability and the concurrency; keeping
// those out of the engine is what makes the engine exhaustively testable and
// makes recovery and the live path provably the same code.
//
// The split is deliberate and mirrors the commit protocol:
//   evaluate -> decide, without changing anything
//   (the caller makes the decision durable)
//   commit   -> apply the decision to in-memory state
// A decision that is not durable is never visible.
class FederationEngine {
 public:
  struct Plan {
    // What the caller should report back. A refusal is a decision with a reason.
    CommandOutcome outcome{};
    // The record that must be durable before the decision becomes visible.
    JournalEntry entry{};
    // True when the entry carries something worth writing.
    bool persists{false};
  };

  FederationEngine(Limits limits, std::shared_ptr<const Clock> clock);
  FederationEngine();

  // Decide a command. The engine is not modified.
  [[nodiscard]] Result<Plan> evaluate(const Command& command) const;
  // Make a decided plan visible. Call only after the entry is durable.
  [[nodiscard]] Result<Ack> commit(const Plan& plan);
  // Apply a durable entry. Used by recovery and by commit, identically.
  [[nodiscard]] Result<Ack> replay(const JournalEntry& entry);
  // Installs state that was itself decoded from durable records during startup
  // recovery. This is the only way state changes without applying an entry, it
  // is used exactly once, and it refuses to move the state backwards.
  [[nodiscard]] Result<Ack> restore(FederationState state);
  // Evaluate and commit in one step. Used by the single-process command line and
  // by tests; the runtime uses evaluate, writes, then commits.
  [[nodiscard]] Result<CommandOutcome> submit(const Command& command);

  [[nodiscard]] const FederationState& state() const noexcept { return state_; }
  [[nodiscard]] FederationState snapshot() const { return state_; }
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
  [[nodiscard]] FederationId federation() const noexcept { return state_.federation(); }
  [[nodiscard]] FederationGeneration generation() const noexcept { return state_.generation(); }
  [[nodiscard]] JournalSequence sequence() const noexcept { return state_.sequence(); }
  [[nodiscard]] Digest authority_digest() const { return state_.authority_digest(); }
  [[nodiscard]] UnixMillis now() const noexcept;

 private:
  [[nodiscard]] CommandOutcome dispatch(const Command& command,
                                        std::vector<MutationPayload>& changes,
                                        FederationGeneration next, bool& keep_on_refusal) const;
  [[nodiscard]] CommandOutcome check_authority(const Command& command) const;

  FederationState state_{};
  Limits limits_{};
  std::shared_ptr<const Clock> clock_{};
};

}  // namespace dcf
