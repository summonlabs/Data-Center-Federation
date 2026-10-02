// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dcf/engine.hpp"
#include "dcf/store.hpp"
#include "dcf/types.hpp"

namespace dcf {

// The rendezvous a synchronous submitter waits on. Defined in the implementation
// so that it stays out of the public interface.
struct CompletionSlot;

struct RuntimeOptions {
  // The directory holding the durable state. Required.
  std::string store_root{};
  Limits limits{};
  // The command queue is bounded. A submitter that arrives when the bound is
  // reached is refused rather than queued, so a slow peer cannot make the
  // runtime allocate without limit.
  std::size_t command_queue_capacity{4096};
  // Compact after this many durable entries. Zero never compacts automatically.
  std::uint64_t compact_every_entries{0};
};

struct RuntimeStats {
  std::uint64_t commands_accepted{0};
  std::uint64_t commands_refused{0};
  std::uint64_t commands_rejected_full{0};
  std::uint64_t commands_rejected_closed{0};
  std::uint64_t reentrant_submissions{0};
  std::uint64_t durable_commits{0};
  std::uint64_t events_emitted{0};
  std::uint64_t compactions{0};
  std::uint64_t callback_failures{0};
};

// A sink for committed outcomes. Callbacks run on the mutator thread after the
// change is durable and published, and are never invoked while any internal lock
// is held. A callback that submits another command is supported: the submission
// is queued and processed after the current callback returns.
class FederationEventSink {
 public:
  FederationEventSink() = default;
  FederationEventSink(const FederationEventSink&) = delete;
  FederationEventSink& operator=(const FederationEventSink&) = delete;
  virtual ~FederationEventSink();

  virtual void on_outcome(const Command& command, const CommandOutcome& outcome) = 0;
};

// ---------------------------------------------------------------------------
// Concurrency and ownership
// ---------------------------------------------------------------------------
// There is exactly one mutator thread. Every state change in the process happens
// on it, in submission order, so no lock ever protects authoritative state and
// no read-modify-write race is possible by construction.
//
//   * Submitters place a command on a bounded queue and return.
//   * The mutator evaluates the command, makes the entry durable, applies it,
//     and publishes an immutable snapshot.
//   * Readers take a consistent copy of the published snapshot. They never block
//     the mutator and are never blocked by it for longer than the copy takes.
//   * Callbacks run on the mutator with no lock held. A callback that submits
//     work while the mutator is inside a callback does not block: the command is
//     placed on a deferred list that the mutator drains when the callback
//     returns. This is the one re-entrancy path, and it is handled explicitly
//     rather than by hoping it never happens.
//
// Lock order: the queue mutex and the publication mutex are never held at the
// same time, so there is no order to get wrong. No lock is ever held while
// calling out of the runtime.
class FederationRuntime {
 public:
  FederationRuntime() = delete;
  FederationRuntime(const FederationRuntime&) = delete;
  FederationRuntime& operator=(const FederationRuntime&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<FederationRuntime>> open(
      const RuntimeOptions& options, std::shared_ptr<const Clock> clock);

  ~FederationRuntime();

  // Queues a command. Returns CapacityExhausted when the queue is full and
  // Closed once shutdown has begun. Safe to call from any thread, including
  // from inside an event callback.
  [[nodiscard]] Result<Ack> submit(Command command);
  // Queues a command and waits for its outcome. Refused when called from inside
  // an event callback, because the mutator would be waiting for itself; that
  // case returns a policy refusal naming the alternative rather than hanging.
  [[nodiscard]] Result<CommandOutcome> submit_sync(Command command);
  // Blocks until everything queued so far, including work submitted by
  // callbacks, has been processed.
  [[nodiscard]] Result<Ack> drain();
  // Refuses new submissions, drains what is queued, stops the mutator, and
  // releases the store lock.
  [[nodiscard]] Result<Ack> shutdown();

  // A consistent copy of the published state.
  [[nodiscard]] FederationState snapshot() const;
  [[nodiscard]] Digest authority_digest() const;
  [[nodiscard]] FederationGeneration generation() const;
  [[nodiscard]] JournalSequence last_sequence() const;

  [[nodiscard]] Result<Ack> compact();
  [[nodiscard]] RuntimeStats stats() const;
  [[nodiscard]] RecoveryReport recovery() const;

  void set_event_sink(std::shared_ptr<FederationEventSink> sink);

 private:
  FederationRuntime(std::unique_ptr<JournalStore> store, std::shared_ptr<const Clock> clock);

  void run();
  void publish(FederationState state);
  void emit(const Command& command, const CommandOutcome& outcome);

  std::unique_ptr<JournalStore> store_{};
  FederationEngine engine_{};
  std::shared_ptr<const Clock> clock_{};
  RuntimeOptions options_{};

  mutable std::mutex queue_mutex_{};
  std::condition_variable queue_ready_{};
  std::condition_variable queue_empty_{};
  // A command plus the slot its submitter is waiting on, if any.
  struct QueuedCommand {
    Command command{};
    std::shared_ptr<CompletionSlot> completion{};
  };

  std::deque<QueuedCommand> queue_{};
  std::vector<QueuedCommand> deferred_{};
  std::size_t in_flight_{0};
  bool compact_requested_{false};
  bool stopping_{false};
  bool accepting_{true};
  bool running_{false};

  mutable std::mutex publish_mutex_{};
  std::shared_ptr<const FederationState> published_{};

  mutable std::mutex sink_mutex_{};
  std::shared_ptr<FederationEventSink> sink_{};

  std::thread mutator_{};
  std::atomic<std::thread::id> mutator_id_{};
  mutable std::mutex stats_mutex_{};
  RuntimeStats stats_{};
  std::uint64_t entries_since_compaction_{0};
};

}  // namespace dcf
