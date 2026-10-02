// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/runtime.hpp"

namespace dcf {

FederationEventSink::~FederationEventSink() = default;

// The rendezvous between a submitter waiting for its outcome and the mutator
// that produces it. Completing the slot is the last thing the mutator does for a
// command, so a submitter that wakes up knows the change is durable and
// published.
struct CompletionSlot {
  std::mutex mutex{};
  std::condition_variable ready{};
  bool done{false};
  CommandOutcome outcome{};
};

FederationRuntime::FederationRuntime(std::unique_ptr<JournalStore> store,
                                     std::shared_ptr<const Clock> clock)
    : store_(std::move(store)),
      engine_(store_->limits(), clock),
      clock_(std::move(clock)),
      options_{} {}

Result<std::unique_ptr<FederationRuntime>> FederationRuntime::open(
    const RuntimeOptions& options, std::shared_ptr<const Clock> clock) {
  if (options.store_root.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a store root is required to open the runtime");
  }
  if (!clock) {
    return make_error(ErrorCode::InvalidArgument, "a clock is required to open the runtime");
  }
  if (options.command_queue_capacity == 0) {
    return make_error(ErrorCode::InvalidArgument, "the command queue capacity must be positive");
  }

  auto store = JournalStore::open(options.store_root, options.limits);
  if (!store) {
    return store.error();
  }

  auto runtime = std::unique_ptr<FederationRuntime>(
      new FederationRuntime(std::make_unique<JournalStore>(std::move(store).value()), clock));
  runtime->options_ = options;
  runtime->engine_ = FederationEngine(options.limits, clock);

  // Recovery is the one place state is installed rather than applied, and what
  // is installed is exactly what the snapshot and journal decoded to.
  const auto restored = runtime->engine_.restore(runtime->store_->recovered_state());
  if (!restored) {
    return restored.error();
  }
  runtime->publish(runtime->engine_.state());
  runtime->running_ = true;
  runtime->mutator_ = std::thread([pointer = runtime.get()] { pointer->run(); });
  // Running is only observable after the thread exists, so a submitter never
  // sees a running runtime whose mutator identifier is still unset.
  runtime->mutator_id_.store(runtime->mutator_.get_id());
  runtime->running_ = true;
  return std::unique_ptr<FederationRuntime>(std::move(runtime));
}

FederationRuntime::~FederationRuntime() {
  if (running_) {
    static_cast<void>(shutdown());
  }
}

Result<Ack> FederationRuntime::submit(Command command) {
  const bool reentrant = mutator_id_.load() == std::this_thread::get_id() && running_;
  std::lock_guard<std::mutex> guard(queue_mutex_);
  if (!accepting_) {
    std::lock_guard<std::mutex> stats_guard(stats_mutex_);
    ++stats_.commands_rejected_closed;
    return make_error(ErrorCode::Closed,
                      "the runtime has stopped accepting commands because it is shutting down");
  }
  const std::size_t queued = queue_.size() + deferred_.size() + in_flight_;
  if (queued >= options_.command_queue_capacity) {
    std::lock_guard<std::mutex> stats_guard(stats_mutex_);
    ++stats_.commands_rejected_full;
    return make_error(ErrorCode::CapacityExhausted,
                      "the command queue holds " + std::to_string(queued) +
                          " commands which is the configured capacity of " +
                          std::to_string(options_.command_queue_capacity));
  }
  if (reentrant) {
    // A callback running on the mutator must never block waiting for the mutator.
    // The command is parked and picked up as soon as the callback returns.
    deferred_.push_back(QueuedCommand{std::move(command), nullptr});
    std::lock_guard<std::mutex> stats_guard(stats_mutex_);
    ++stats_.reentrant_submissions;
    return Ack{};
  }
  queue_.push_back(QueuedCommand{std::move(command), nullptr});
  queue_ready_.notify_one();
  return Ack{};
}

Result<CommandOutcome> FederationRuntime::submit_sync(Command command) {
  if (mutator_id_.load() == std::this_thread::get_id() && running_) {
    return make_error(
        ErrorCode::RefusedByPolicy,
        "a synchronous submission from inside an event callback would wait for the mutator to "
        "finish the very command it is running; use submit() instead");
  }
  auto slot = std::make_shared<CompletionSlot>();
  {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    if (!accepting_) {
      std::lock_guard<std::mutex> stats_guard(stats_mutex_);
      ++stats_.commands_rejected_closed;
      return make_error(ErrorCode::Closed,
                        "the runtime has stopped accepting commands because it is shutting down");
    }
    const std::size_t queued = queue_.size() + deferred_.size() + in_flight_;
    if (queued >= options_.command_queue_capacity) {
      std::lock_guard<std::mutex> stats_guard(stats_mutex_);
      ++stats_.commands_rejected_full;
      return make_error(ErrorCode::CapacityExhausted,
                        "the command queue holds " + std::to_string(queued) +
                            " commands which is the configured capacity of " +
                            std::to_string(options_.command_queue_capacity));
    }
    queue_.push_back(QueuedCommand{std::move(command), slot});
    queue_ready_.notify_one();
  }
  std::unique_lock<std::mutex> guard(slot->mutex);
  slot->ready.wait(guard, [&slot] { return slot->done; });
  return slot->outcome;
}

Result<Ack> FederationRuntime::drain() {
  std::unique_lock<std::mutex> guard(queue_mutex_);
  queue_empty_.wait(guard, [this] {
    return queue_.empty() && deferred_.empty() && in_flight_ == 0 && !compact_requested_;
  });
  return Ack{};
}

void FederationRuntime::publish(FederationState state) {
  auto snapshot = std::make_shared<const FederationState>(std::move(state));
  std::lock_guard<std::mutex> guard(publish_mutex_);
  published_ = std::move(snapshot);
}

FederationState FederationRuntime::snapshot() const {
  std::lock_guard<std::mutex> guard(publish_mutex_);
  if (published_ == nullptr) {
    return FederationState{};
  }
  return *published_;
}

Digest FederationRuntime::authority_digest() const {
  std::lock_guard<std::mutex> guard(publish_mutex_);
  return published_ == nullptr ? Digest{} : published_->authority_digest();
}

FederationGeneration FederationRuntime::generation() const {
  std::lock_guard<std::mutex> guard(publish_mutex_);
  return published_ == nullptr ? FederationGeneration{} : published_->generation();
}

JournalSequence FederationRuntime::last_sequence() const {
  std::lock_guard<std::mutex> guard(publish_mutex_);
  return published_ == nullptr ? JournalSequence{} : published_->sequence();
}

Result<Ack> FederationRuntime::compact() {
  // Compaction must observe exactly the committed state, so it is expressed as a
  // request for the mutator rather than performed here against a copy.
  std::lock_guard<std::mutex> guard(queue_mutex_);
  if (!accepting_) {
    return make_error(ErrorCode::Closed, "the runtime is shutting down");
  }
  compact_requested_ = true;
  queue_ready_.notify_one();
  return Ack{};
}

RuntimeStats FederationRuntime::stats() const {
  std::lock_guard<std::mutex> guard(stats_mutex_);
  return stats_;
}

RecoveryReport FederationRuntime::recovery() const { return store_->recovery(); }

void FederationRuntime::set_event_sink(std::shared_ptr<FederationEventSink> sink) {
  std::lock_guard<std::mutex> guard(sink_mutex_);
  sink_ = std::move(sink);
}

void FederationRuntime::emit(const Command& command, const CommandOutcome& outcome) {
  std::shared_ptr<FederationEventSink> sink;
  {
    std::lock_guard<std::mutex> guard(sink_mutex_);
    sink = sink_;
  }
  if (sink == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> guard(stats_mutex_);
    ++stats_.events_emitted;
  }
  // No lock is held here. A callback is free to read state, submit commands, or
  // throw; a throwing callback is contained so that one bad observer cannot take
  // the runtime down.
  try {
    sink->on_outcome(command, outcome);
  } catch (...) {
    std::lock_guard<std::mutex> guard(stats_mutex_);
    ++stats_.callback_failures;
  }
}

void FederationRuntime::run() {
  for (;;) {
    Command command;
    std::shared_ptr<CompletionSlot> completion;
    {
      std::unique_lock<std::mutex> guard(queue_mutex_);
      queue_ready_.wait(guard,
                        [this] { return stopping_ || !queue_.empty() || compact_requested_; });
      if (queue_.empty()) {
        if (compact_requested_) {
          // The request is cleared only once the work is finished. Clearing it
          // first would let a concurrent drain observe an idle runtime while the
          // compaction was still in flight, and report a completion that had not
          // happened yet.
          guard.unlock();
          const auto compacted = store_->compact(engine_.state());
          {
            std::lock_guard<std::mutex> stats_guard(stats_mutex_);
            if (compacted) {
              ++stats_.compactions;
            }
            entries_since_compaction_ = 0;
          }
          std::lock_guard<std::mutex> empty_guard(queue_mutex_);
          compact_requested_ = false;
          if (queue_.empty() && in_flight_ == 0) {
            queue_empty_.notify_all();
          }
          continue;
        }
        queue_empty_.notify_all();
        return;
      }
      command = std::move(queue_.front().command);
      completion = std::move(queue_.front().completion);
      queue_.pop_front();
      ++in_flight_;
    }

    CommandOutcome outcome;
    const auto plan = engine_.evaluate(command);
    if (!plan) {
      outcome.code = plan.error().code;
      outcome.detail = plan.error().detail;
      outcome.generation = engine_.generation();
    } else {
      const auto& decided = plan.value();
      if (decided.persists) {
        const auto durable = store_->commit(decided.entry);
        if (!durable) {
          outcome.code = durable.error().code;
          outcome.detail = durable.error().detail;
          outcome.generation = engine_.generation();
        } else {
          const auto applied = engine_.commit(decided);
          if (!applied) {
            outcome.code = applied.error().code;
            outcome.detail = applied.error().detail;
            outcome.generation = engine_.generation();
          } else {
            outcome = decided.outcome;
            std::lock_guard<std::mutex> guard(stats_mutex_);
            ++stats_.durable_commits;
          }
        }
      } else {
        outcome = decided.outcome;
      }
      if (outcome.code == ErrorCode::None) {
        publish(engine_.state());
        {
          std::lock_guard<std::mutex> guard(stats_mutex_);
          ++stats_.commands_accepted;
        }
      } else {
        std::lock_guard<std::mutex> guard(stats_mutex_);
        ++stats_.commands_refused;
      }
    }

    // A submitter waiting for this outcome is released before observers run, so
    // a slow observer cannot delay a synchronous caller.
    if (completion != nullptr) {
      {
        std::lock_guard<std::mutex> slot_guard(completion->mutex);
        completion->outcome = outcome;
        completion->done = true;
      }
      completion->ready.notify_all();
    }

    // The outcome is announced with no lock held. The command stays in flight
    // until the announcement has returned, so a caller that observes an idle
    // runtime also knows that no callback is still running.
    emit(command, outcome);

    {
      std::lock_guard<std::mutex> guard(queue_mutex_);
      --in_flight_;
      // Work submitted by a callback is moved onto the front of the queue in
      // submission order.
      if (!deferred_.empty()) {
        for (auto iterator = deferred_.rbegin(); iterator != deferred_.rend(); ++iterator) {
          queue_.push_front(std::move(*iterator));
        }
        deferred_.clear();
        queue_ready_.notify_one();
      }
      if (queue_.empty() && in_flight_ == 0 && !compact_requested_) {
        queue_empty_.notify_all();
      }
    }

    if (options_.compact_every_entries > 0) {
      ++entries_since_compaction_;
      if (entries_since_compaction_ >= options_.compact_every_entries) {
        entries_since_compaction_ = 0;
        const auto compacted = store_->compact(engine_.state());
        if (compacted) {
          std::lock_guard<std::mutex> guard(stats_mutex_);
          ++stats_.compactions;
        }
      }
    }
  }
}

Result<Ack> FederationRuntime::shutdown() {
  {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    if (!running_) {
      return Ack{};
    }
    accepting_ = false;
  }
  // Everything already queued is still processed, so an accepted command is
  // never silently dropped by a shutdown.
  const auto drained = drain();

  {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    stopping_ = true;
  }
  queue_ready_.notify_all();
  if (mutator_.joinable()) {
    mutator_.join();
  }
  running_ = false;
  const auto closed = store_->close();
  if (!drained) {
    return drained.error();
  }
  if (!closed) {
    return closed.error();
  }
  return Ack{};
}

}  // namespace dcf
