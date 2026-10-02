// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dcf/json.hpp"
#include "dcf/runtime.hpp"
#include "dcf/store.hpp"
#include "test_harness.hpp"

namespace {

using namespace dcf;

const SiteId kAlpha{0x21ULL, 0x22ULL};
const SiteId kBeta{0x23ULL, 0x24ULL};

[[nodiscard]] RuntimeOptions options_for(const std::string& root, std::size_t capacity) {
  RuntimeOptions options;
  options.store_root = root;
  options.command_queue_capacity = capacity;
  return options;
}

// Establishes a federation on an empty store, the way the daemon does.
void establish(const std::string& root, const FederationId& federation) {
  auto opened = JournalStore::open(root, Limits{});
  if (!opened) {
    return;
  }
  JournalStore store = std::move(opened).value();
  if (!store.recovered_state().initialized()) {
    static_cast<void>(store.commit(genesis_entry(federation, UnixMillis{1000})));
  }
  static_cast<void>(store.close());
}

[[nodiscard]] Command register_command(SiteId site, const char* name, const char* capability) {
  Command command;
  CompatibilityDeclaration declaration;
  declaration.implementation = "summon.runtime-test";
  declaration.implementation_version = Version{1, 0};
  declaration.protocol = VersionRange{Version{1, 0}, Version{9, 9}};
  CapabilityDeclaration entry;
  entry.id = CapabilityId{capability};
  entry.supported = VersionRange{Version{1, 0}, Version{2, 0}};
  declaration.capabilities.push_back(entry);
  command.payload = RegisterSiteCommand{site, name, declaration};
  command.origin = site;
  return command;
}

// A callback that submits more work, reads state, and throws on demand. Every
// one of these is a path the ownership audit has to account for.
class ReentrantSink final : public FederationEventSink {
 public:
  explicit ReentrantSink(FederationRuntime* runtime) : runtime_(runtime) {}

  void on_outcome(const Command& command, const CommandOutcome& outcome) override {
    ++calls;
    if (outcome.code == ErrorCode::None) {
      ++accepted;
    }
    // Reading state from a callback must not deadlock.
    const FederationState state = runtime_->snapshot();
    digest = state.authority_digest();
    if (throw_once) {
      throw_once = false;
      throw std::runtime_error("sink failure");
    }
    if (resubmit_budget > 0) {
      --resubmit_budget;
      // A re-entrant submission is parked, never blocked on the mutator.
      const auto submitted = runtime_->submit(Command{*reinterpret_cast<const Command*>(&command)});
      if (submitted) {
        ++reentrant;
      }
    }
  }

  FederationRuntime* runtime_{nullptr};
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> reentrant{0};
  std::atomic<bool> throw_once{false};
  std::atomic<int> resubmit_budget{0};
  Digest digest{};
};

}  // namespace

DCF_TEST(runtime, concurrent_submitters_and_readers_agree) {
  const std::string root = dcf::test::make_scratch_directory("runtime-concurrent");
  establish(root, FederationId{0x31ULL, 0x32ULL});

  auto opened = FederationRuntime::open(options_for(root, 1024),
                                        std::make_shared<ManualClock>(UnixMillis{1000}));
  DCF_REQUIRE(opened.has_value());
  std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();

  constexpr int kSubmitters = 8;
  constexpr int kReaders = 4;
  constexpr int kCommandsPerSubmitter = 40;

  std::atomic<int> submitted{0};
  std::atomic<int> refused{0};
  std::atomic<bool> stop{false};
  std::atomic<int> reader_observations{0};

  std::vector<std::thread> threads;
  for (int index = 0; index < kSubmitters; ++index) {
    threads.emplace_back([&, index] {
      for (int step = 0; step < kCommandsPerSubmitter; ++step) {
        Command command;
        command.payload = RecordContactCommand{
            kAlpha, AcceptedGeneration{}, Digest{}, SiteLocalEpoch{static_cast<std::uint64_t>(index)},
            UnixMillis{2000 + step}};
        const auto result = runtime->submit(std::move(command));
        if (result) {
          ++submitted;
        } else {
          ++refused;
        }
      }
    });
  }
  for (int index = 0; index < kReaders; ++index) {
    threads.emplace_back([&] {
      while (!stop.load()) {
        const FederationState state = runtime->snapshot();
        const Digest digest = state.authority_digest();
        // A snapshot is always a coherent state: the digest of the state that
        // was copied must equal the digest the snapshot reports.
        if (state.initialized()) {
          if (digest != state.authority_digest()) {
            ++refused;
          }
          ++reader_observations;
        }
      }
    });
  }

  for (int index = 0; index < kSubmitters; ++index) {
    threads[static_cast<std::size_t>(index)].join();
  }
  DCF_CHECK(runtime->drain().has_value());
  stop.store(true);
  for (std::size_t index = static_cast<std::size_t>(kSubmitters); index < threads.size(); ++index) {
    threads[index].join();
  }

  DCF_CHECK_EQ(submitted.load(), kSubmitters * kCommandsPerSubmitter);
  DCF_CHECK(refused.load() == 0);
  DCF_CHECK(reader_observations.load() > 0);
  DCF_CHECK(runtime->shutdown().has_value());
}

DCF_TEST(runtime, a_callback_may_submit_and_read_without_deadlock) {
  const std::string root = dcf::test::make_scratch_directory("runtime-reentrant");
  establish(root, FederationId{0x33ULL, 0x34ULL});

  auto opened = FederationRuntime::open(options_for(root, 64),
                                        std::make_shared<ManualClock>(UnixMillis{1000}));
  DCF_REQUIRE(opened.has_value());
  std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();

  auto sink = std::make_shared<ReentrantSink>(runtime.get());
  sink->resubmit_budget.store(5);
  runtime->set_event_sink(sink);

  DCF_REQUIRE(runtime->submit(register_command(kAlpha, "alpha", "dcf.membership")).has_value());
  DCF_REQUIRE(runtime->drain().has_value());

  DCF_CHECK(sink->calls.load() >= 6);
  DCF_CHECK_EQ(sink->reentrant.load(), std::uint64_t{5});
  DCF_CHECK(runtime->stats().reentrant_submissions >= 5);
  DCF_CHECK(runtime->shutdown().has_value());
}

DCF_TEST(runtime, a_throwing_callback_is_contained) {
  const std::string root = dcf::test::make_scratch_directory("runtime-throwing");
  establish(root, FederationId{0x35ULL, 0x36ULL});

  auto opened = FederationRuntime::open(options_for(root, 64),
                                        std::make_shared<ManualClock>(UnixMillis{1000}));
  DCF_REQUIRE(opened.has_value());
  std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();

  auto sink = std::make_shared<ReentrantSink>(runtime.get());
  sink->throw_once.store(true);
  runtime->set_event_sink(sink);

  DCF_REQUIRE(runtime->submit(register_command(kAlpha, "alpha", "dcf.membership")).has_value());
  DCF_REQUIRE(runtime->drain().has_value());
  DCF_CHECK_EQ(runtime->stats().callback_failures, std::uint64_t{1});

  // The runtime is still fully usable afterwards.
  DCF_REQUIRE(runtime->submit(register_command(kBeta, "beta", "dcf.membership")).has_value());
  DCF_REQUIRE(runtime->drain().has_value());
  DCF_CHECK_EQ(runtime->snapshot().site_count(), std::size_t{2});
  DCF_CHECK(runtime->shutdown().has_value());
}

DCF_TEST(runtime, synchronous_submission_from_a_callback_is_refused_not_deadlocked) {
  const std::string root = dcf::test::make_scratch_directory("runtime-sync-from-callback");
  establish(root, FederationId{0x37ULL, 0x38ULL});

  auto opened = FederationRuntime::open(options_for(root, 64),
                                        std::make_shared<ManualClock>(UnixMillis{1000}));
  DCF_REQUIRE(opened.has_value());
  std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();

  class SyncSink final : public FederationEventSink {
   public:
    explicit SyncSink(FederationRuntime* runtime) : runtime_(runtime) {}
    void on_outcome(const Command& command, const CommandOutcome&) override {
      if (attempted) {
        return;
      }
      attempted = true;
      outcome = runtime_->submit_sync(command);
    }
    FederationRuntime* runtime_{nullptr};
    bool attempted{false};
    Result<CommandOutcome> outcome{make_error(ErrorCode::Internal, "not attempted")};
  };

  auto sink = std::make_shared<SyncSink>(runtime.get());
  runtime->set_event_sink(sink);
  DCF_REQUIRE(runtime->submit(register_command(kAlpha, "alpha", "dcf.membership")).has_value());
  DCF_REQUIRE(runtime->drain().has_value());
  DCF_CHECK(!sink->outcome.has_value());
  DCF_CHECK_EQ(sink->outcome.error().code, ErrorCode::RefusedByPolicy);
  DCF_CHECK(runtime->shutdown().has_value());
}

DCF_TEST(runtime, the_queue_is_bounded_and_says_so) {
  const std::string root = dcf::test::make_scratch_directory("runtime-bounded");
  establish(root, FederationId{0x39ULL, 0x3aULL});

  RuntimeOptions options = options_for(root, 4);
  auto opened = FederationRuntime::open(options, std::make_shared<ManualClock>(UnixMillis{1000}));
  DCF_REQUIRE(opened.has_value());
  std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();

  // A callback that slows the mutator down gives the test a window in which the
  // queue really does fill.
  class BlockingSink final : public FederationEventSink {
   public:
    void on_outcome(const Command&, const CommandOutcome&) override {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };
  runtime->set_event_sink(std::make_shared<BlockingSink>());

  int refused = 0;
  for (int index = 0; index < 200; ++index) {
    Command command;
    command.payload = RecordContactCommand{kAlpha, AcceptedGeneration{}, Digest{},
                                           SiteLocalEpoch{}, UnixMillis{1}};
    if (!runtime->submit(std::move(command))) {
      ++refused;
    }
  }
  DCF_CHECK(refused > 0);
  DCF_CHECK(runtime->stats().commands_rejected_full > 0);
  DCF_REQUIRE(runtime->drain().has_value());
  DCF_CHECK(runtime->shutdown().has_value());
}

DCF_TEST(runtime, shutdown_does_not_drop_accepted_work) {
  const std::string root = dcf::test::make_scratch_directory("runtime-shutdown");
  establish(root, FederationId{0x3bULL, 0x3cULL});

  auto opened = FederationRuntime::open(options_for(root, 256),
                                        std::make_shared<ManualClock>(UnixMillis{1000}));
  DCF_REQUIRE(opened.has_value());
  std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();

  for (int index = 0; index < 50; ++index) {
    DCF_REQUIRE(runtime->submit(register_command(index == 0 ? kAlpha : kBeta, "site",
                                                 "dcf.membership"))
                    .has_value());
  }
  DCF_REQUIRE(runtime->shutdown().has_value());
  // Everything that was accepted is durable and present.
  DCF_CHECK(runtime->stats().durable_commits >= 1);
  DCF_CHECK(runtime->last_sequence().value() >= 1);

  const auto refused = runtime->submit(register_command(kAlpha, "alpha", "dcf.membership"));
  DCF_CHECK(!refused.has_value());
  DCF_CHECK_EQ(refused.error().code, ErrorCode::Closed);
}

DCF_TEST(runtime, state_survives_a_real_close_and_reopen) {
  const std::string root = dcf::test::make_scratch_directory("runtime-reopen");
  establish(root, FederationId{0x3dULL, 0x3eULL});
  Digest before;
  {
    auto opened = FederationRuntime::open(options_for(root, 64),
                                          std::make_shared<ManualClock>(UnixMillis{1000}));
    DCF_REQUIRE(opened.has_value());
    std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();
    DCF_REQUIRE(runtime->submit(register_command(kAlpha, "alpha", "dcf.membership")).has_value());
    DCF_REQUIRE(runtime->submit(register_command(kBeta, "beta", "dcf.membership")).has_value());
    DCF_REQUIRE(runtime->drain().has_value());
    before = runtime->authority_digest();
    DCF_REQUIRE(runtime->shutdown().has_value());
  }
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    DCF_CHECK(store.recovered_state().initialized());
    DCF_CHECK_EQ(store.recovered_state().site_count(), std::size_t{2});
    DCF_REQUIRE(store.close().has_value());
  }
  {
    auto opened = FederationRuntime::open(options_for(root, 64),
                                          std::make_shared<ManualClock>(UnixMillis{1000}));
    DCF_REQUIRE(opened.has_value());
    std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();
    DCF_CHECK_EQ(runtime->authority_digest(), before);
    DCF_REQUIRE(runtime->shutdown().has_value());
  }
}

DCF_TEST(runtime, compaction_keeps_the_state_and_moves_the_journal) {
  const std::string root = dcf::test::make_scratch_directory("runtime-compact");
  establish(root, FederationId{0x3fULL, 0x40ULL});

  auto opened = FederationRuntime::open(options_for(root, 64),
                                        std::make_shared<ManualClock>(UnixMillis{1000}));
  DCF_REQUIRE(opened.has_value());
  std::unique_ptr<FederationRuntime> runtime = std::move(opened).value();
  DCF_REQUIRE(runtime->submit(register_command(kAlpha, "alpha", "dcf.membership")).has_value());
  DCF_REQUIRE(runtime->drain().has_value());
  const Digest before = runtime->authority_digest();
  const std::uint64_t sequence = runtime->last_sequence().value();

  DCF_REQUIRE(runtime->compact().has_value());
  DCF_REQUIRE(runtime->drain().has_value());
  DCF_CHECK_EQ(runtime->stats().compactions, std::uint64_t{1});
  DCF_CHECK_EQ(runtime->authority_digest(), before);

  // A compaction never loses a committed entry: the sequence does not go back.
  DCF_CHECK_EQ(runtime->last_sequence().value(), sequence);
  DCF_REQUIRE(runtime->shutdown().has_value());

  auto reopened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(reopened.has_value());
  JournalStore store = std::move(reopened).value();
  DCF_CHECK(store.recovery().snapshot_sequence > 0);
  DCF_CHECK_EQ(store.recovered_state().authority_digest(), before);
  DCF_REQUIRE(store.close().has_value());
}
