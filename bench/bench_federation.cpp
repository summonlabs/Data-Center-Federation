// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Benchmarks of completed core operations. Nothing here measures submission
// latency: a submission that has not been committed is not an operation this
// boundary has performed, so the durable path is what is timed, and the durable
// cost is reported separately from the in-memory evaluation cost.
//
// Every number printed is a measurement taken by this run. No number is
// estimated, extrapolated, or carried over from anywhere.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dcf/engine.hpp"
#include "dcf/runtime.hpp"
#include "dcf/store.hpp"

namespace {

using namespace dcf;

class BenchClock final : public Clock {
 public:
  [[nodiscard]] UnixMillis wall_clock() const noexcept override { return UnixMillis{1000}; }
};

[[nodiscard]] SiteId bench_site(std::uint64_t index) {
  return SiteId{0x1000ULL, index + 1};
}

[[nodiscard]] CompatibilityDeclaration bench_declaration() {
  CompatibilityDeclaration declaration;
  declaration.implementation = "summon.bench-site";
  declaration.implementation_version = Version{1, 0};
  declaration.protocol = VersionRange{Version{1, 0}, Version{9, 9}};
  CapabilityDeclaration entry;
  entry.id = CapabilityId{"dcf.membership"};
  entry.supported = VersionRange{Version{1, 0}, Version{2, 0}};
  declaration.capabilities.push_back(entry);
  return declaration;
}

struct Timing {
  std::string label{};
  std::uint64_t operations{0};
  double milliseconds{0};
};

void report(const Timing& timing, const char* evidence) {
  const double per_operation =
      timing.operations == 0 ? 0.0 : (timing.milliseconds * 1000.0) /
                                          static_cast<double>(timing.operations);
  std::printf("%-46s %10llu ops %12.2f ms %12.3f us/op  [%s]\n", timing.label.c_str(),
              static_cast<unsigned long long>(timing.operations), timing.milliseconds,
              per_operation, evidence);
  std::fflush(stdout);
}

// Builds a federation with the requested number of registered sites. The digest
// of that state is the input to every read-side measurement, so the cost of
// building it is not part of what is reported.
[[nodiscard]] FederationState build_state(std::uint64_t sites, const Limits& limits) {
  FederationEngine engine(limits, std::make_shared<BenchClock>());
  static_cast<void>(
      engine.replay(genesis_entry(FederationId{0x2000ULL, 0x2001ULL}, UnixMillis{1})));

  CompatibilityWindow window;
  window.capability = CapabilityId{"dcf.membership"};
  window.offered = VersionRange{Version{1, 0}, Version{3, 0}};
  window.effective_from = FederationGeneration{1};

  Command declare;
  declare.payload = DeclareWindowCommand{window};
  auto plan = engine.evaluate(declare);
  if (plan.has_value() && plan.value().persists) {
    static_cast<void>(engine.commit(plan.value()));
  }

  for (std::uint64_t index = 0; index < sites; ++index) {
    Command command;
    command.payload = RegisterSiteCommand{bench_site(index), "bench", bench_declaration()};
    command.origin = bench_site(index);
    auto registered = engine.evaluate(command);
    if (registered.has_value() && registered.value().persists) {
      static_cast<void>(engine.commit(registered.value()));
    }
  }
  return engine.state();
}

void bench_evaluation(std::uint64_t sites) {
  const Limits limits;
  const FederationState state = build_state(sites, limits);
  const std::vector<CompatibilityWindow> windows = state.window_list();
  const CompatibilityDeclaration declaration = bench_declaration();

  const auto start = std::chrono::steady_clock::now();
  std::uint64_t decisions = 0;
  for (int repeat = 0; repeat < 4; ++repeat) {
    for (std::size_t index = 0; index < state.site_count(); ++index) {
      const CompatibilityReport report =
          evaluate_compatibility(declaration, windows, state.generation(), limits);
      if (report.decision == AdmissionDecision::Admitted) {
        ++decisions;
      }
    }
  }
  const auto stop = std::chrono::steady_clock::now();
  Timing timing;
  timing.label = "compatibility evaluation @" + std::to_string(sites) + " sites";
  timing.operations = decisions;
  timing.milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
  report(timing, "REAL");
}

void bench_reconciliation(std::uint64_t sites) {
  const Limits limits;
  const FederationState state = build_state(sites, limits);

  std::vector<ReconciliationInputs> inputs;
  inputs.reserve(state.sites().size());
  for (const auto& entry : state.sites()) {
    ReconciliationInputs input;
    input.federation_generation = state.generation();
    input.site = entry.second;
    input.delegations = state.delegations_for(entry.second.site);
    input.federation_history = state.site_authority_digest(entry.second.site);
    input.report.site = entry.second.site;
    input.report.accepted = AcceptedGeneration{state.generation().value()};
    input.report.accepted_history = input.federation_history;
    input.report.membership_generation = entry.second.generation;
    input.now = UnixMillis{1000};
    inputs.push_back(std::move(input));
  }

  const auto start = std::chrono::steady_clock::now();
  std::uint64_t evaluations = 0;
  std::uint64_t in_sync = 0;
  for (const ReconciliationInputs& input : inputs) {
    const ReconciliationEvaluation evaluation = evaluate_reconciliation(input, limits);
    ++evaluations;
    if (evaluation.outcome == ReconciliationOutcome::InSync) {
      ++in_sync;
    }
  }
  const auto stop = std::chrono::steady_clock::now();

  Timing timing;
  timing.label = "reconciliation decision @" + std::to_string(sites) + " sites";
  timing.operations = evaluations;
  timing.milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
  report(timing, "REAL");
  if (in_sync != evaluations) {
    std::printf("  note: %llu of %llu reconciliations were not in sync\n",
                static_cast<unsigned long long>(evaluations - in_sync),
                static_cast<unsigned long long>(evaluations));
  }
}

void bench_serialization(std::uint64_t sites) {
  const Limits limits;
  const FederationState state = build_state(sites, limits);

  const auto start = std::chrono::steady_clock::now();
  std::uint64_t digests = 0;
  Digest last;
  for (int repeat = 0; repeat < 8; ++repeat) {
    last = state.authority_digest();
    ++digests;
  }
  const auto stop = std::chrono::steady_clock::now();
  Timing timing;
  timing.label = "canonical authority digest @" + std::to_string(sites) + " sites";
  timing.operations = digests;
  timing.milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
  report(timing, "REAL");
  if (last.is_zero()) {
    std::printf("  note: the digest was empty, so the measurement measured nothing\n");
  }

  const auto copy_start = std::chrono::steady_clock::now();
  std::uint64_t copies = 0;
  for (int repeat = 0; repeat < 8; ++repeat) {
    const FederationState copy = state;
    if (copy.site_count() != 0) {
      ++copies;
    }
  }
  const auto copy_stop = std::chrono::steady_clock::now();
  Timing copy_timing;
  copy_timing.label = "published state copy @" + std::to_string(sites) + " sites";
  copy_timing.operations = copies;
  copy_timing.milliseconds =
      std::chrono::duration<double, std::milli>(copy_stop - copy_start).count();
  report(copy_timing, "REAL");
}

void bench_durable_commit(const std::string& root, int entries) {
  auto opened = JournalStore::open(root, Limits{});
  if (!opened) {
    std::printf("the durable measurement could not run: %s\n",
                opened.error().detail.c_str());
    return;
  }
  JournalStore store = std::move(opened).value();
  FederationEngine engine(Limits{}, std::make_shared<BenchClock>());
  const JournalEntry genesis = genesis_entry(FederationId{0x3000ULL, 0x3001ULL}, UnixMillis{1});
  static_cast<void>(engine.replay(genesis));
  static_cast<void>(store.commit(genesis));

  // The measurement needs a site that exists, otherwise every command would be
  // a refusal and the harness would report a time for having done nothing.
  {
    Command registration;
    registration.payload =
        RegisterSiteCommand{bench_site(0), "bench", bench_declaration()};
    registration.origin = bench_site(0);
    const auto plan = engine.evaluate(registration);
    if (plan.has_value() && plan.value().persists) {
      static_cast<void>(store.commit(plan.value().entry));
      static_cast<void>(engine.commit(plan.value()));
    }
  }

  const auto start = std::chrono::steady_clock::now();
  std::uint64_t committed = 0;
  for (int index = 0; index < entries; ++index) {
    Command command;
    command.payload = RecordContactCommand{
        bench_site(0), AcceptedGeneration{}, Digest{},
        SiteLocalEpoch{static_cast<std::uint64_t>(index)}, UnixMillis{index}};
    command.origin = bench_site(0);
    const auto plan = engine.evaluate(command);
    if (!plan.has_value()) {
      continue;
    }
    if (store.commit(plan.value().entry).has_value()) {
      ++committed;
    }
    static_cast<void>(engine.commit(plan.value()));
  }
  const auto stop = std::chrono::steady_clock::now();

  Timing timing;
  timing.label = "durable commit (write + flush)";
  timing.operations = committed;
  timing.milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
  report(timing, "REAL");

  const auto compact_start = std::chrono::steady_clock::now();
  const auto compacted = store.compact(engine.state());
  const auto compact_stop = std::chrono::steady_clock::now();
  Timing compact_timing;
  compact_timing.label = "compaction (snapshot + rotate segment)";
  compact_timing.operations = compacted.has_value() ? 1 : 0;
  compact_timing.milliseconds =
      std::chrono::duration<double, std::milli>(compact_stop - compact_start).count();
  report(compact_timing, "REAL");
  static_cast<void>(store.close());
}

}  // namespace

int main(int argc, char** argv) {
  std::string scratch = ".";
  int durable_entries = 500;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--scratch" && index + 1 < argc) {
      scratch = argv[++index];
    } else if (argument == "--durable-entries" && index + 1 < argc) {
      durable_entries = std::atoi(argv[++index]);
    }
  }

  std::printf("data-center-federation benchmark\n");
  std::printf("hardware threads reported by the standard library: %u\n",
              std::thread::hardware_concurrency());
  std::printf("\n");
  std::printf("%-46s %10s %15s %15s  %s\n", "measurement", "ops", "total", "per operation",
              "evidence");
  std::printf(
      "--------------------------------------------------------------------------------------"
      "--------------\n");

  for (const std::uint64_t sites : {std::uint64_t{1000}, std::uint64_t{10000}}) {
    bench_evaluation(sites);
    bench_reconciliation(sites);
    bench_serialization(sites);
  }

  bench_durable_commit(scratch + "/bench-store", durable_entries);

  std::printf("\n");
  std::printf("Every figure above was measured by this process; none is estimated.\n");
  std::printf("Durable operations include a flush to storage and are reported separately\n");
  std::printf("from in-memory evaluation, because the two are not comparable.\n");
  return 0;
}
