// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "dcf/engine.hpp"
#include "dcf/platform.hpp"
#include "dcf/store.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;
using namespace dcf;

const FederationId kFederation{0x4242ULL, 0x2424ULL};
const SiteId kAlpha{0x11ULL, 0x22ULL};
const SiteId kBeta{0x33ULL, 0x44ULL};

[[nodiscard]] std::uint64_t size_of(const std::string& path) {
  std::error_code code;
  const auto size = fs::file_size(path, code);
  return code ? 0 : static_cast<std::uint64_t>(size);
}

void write_bytes_at(const std::string& path, std::uint64_t offset,
                    const std::vector<std::uint8_t>& bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  file.close();
}

void truncate_to(const std::string& path, std::uint64_t size) {
  std::error_code code;
  fs::resize_file(path, size, code);
}

// A small but complete sequence of authoritative changes.
void seed_entries(JournalStore& store, FederationEngine& engine) {
  const auto genesis = engine.replay(genesis_entry(kFederation, UnixMillis{1000}));
  if (!genesis) {
    return;
  }
  static_cast<void>(store.commit(genesis_entry(kFederation, UnixMillis{1000})));

  Command window;
  window.payload = DeclareWindowCommand{[] {
    CompatibilityWindow value;
    value.capability = CapabilityId{"dcf.membership"};
    value.offered = VersionRange{Version{1, 0}, Version{3, 0}};
    value.effective_from = FederationGeneration{1};
    return value;
  }()};
  auto plan = engine.evaluate(window);
  if (plan.has_value() && plan.value().persists) {
    static_cast<void>(store.commit(plan.value().entry));
    static_cast<void>(engine.commit(plan.value()));
  }

  const auto register_site = [&store, &engine](SiteId site, const char* name) {
    Command command;
    command.payload = RegisterSiteCommand{site, name, [] {
      CompatibilityDeclaration declaration;
      declaration.implementation = "summon.test-site";
      declaration.implementation_version = Version{1, 0};
      declaration.protocol = VersionRange{Version{1, 0}, Version{9, 9}};
      CapabilityDeclaration entry;
      entry.id = CapabilityId{"dcf.membership"};
      entry.supported = VersionRange{Version{1, 0}, Version{2, 0}};
      declaration.capabilities.push_back(entry);
      return declaration;
    }()};
    auto planned = engine.evaluate(command);
    if (planned.has_value() && planned.value().persists) {
      static_cast<void>(store.commit(planned.value().entry));
      static_cast<void>(engine.commit(planned.value()));
    }
  };
  register_site(kAlpha, "alpha");
  register_site(kBeta, "beta");
}

}  // namespace

DCF_TEST(store, commit_and_reopen_reproduces_the_state_exactly) {
  const std::string root = dcf::test::make_scratch_directory("store-reopen");
  Digest expected;
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    expected = engine.state().canonical_digest();
    DCF_REQUIRE(store.close().has_value());
  }
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    DCF_CHECK(store.recovery().reopened);
    DCF_CHECK_EQ(store.recovery().torn_tail_bytes_removed, std::uint64_t{0});
    DCF_CHECK(store.recovery().entries_replayed > 0);
    DCF_CHECK_EQ(store.recovered_state().canonical_digest(), expected);
    DCF_CHECK_EQ(store.recovered_state().federation(), kFederation);
    DCF_CHECK_EQ(store.recovered_state().site_count(), std::size_t{2});
    DCF_REQUIRE(store.close().has_value());
  }
}

DCF_TEST(store, torn_tail_is_truncated_and_reported) {
  const std::string root = dcf::test::make_scratch_directory("store-torn");
  Digest expected;
  std::uint64_t clean_size = 0;
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    expected = engine.state().canonical_digest();
    DCF_REQUIRE(store.close().has_value());
  }
  const std::string journal = root + "/journal.1.dcflog";
  clean_size = size_of(journal);
  DCF_CHECK(clean_size > 0);

  // Simulate a crash midway through writing one more record: the length and
  // checksum are present but the payload is not.
  {
    std::ofstream file(journal, std::ios::binary | std::ios::app);
    const std::uint8_t partial[8] = {0x40, 0x00, 0x00, 0x00, 0xde, 0xad, 0xbe, 0xef};
    file.write(reinterpret_cast<const char*>(partial), 8);
    file.close();
  }

  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    DCF_CHECK_EQ(store.recovery().torn_tail_bytes_removed, std::uint64_t{8});
    DCF_CHECK_EQ(store.recovered_state().canonical_digest(), expected);
    DCF_REQUIRE(store.close().has_value());
  }
  DCF_CHECK_EQ(size_of(journal), clean_size);
}

DCF_TEST(store, all_zero_tail_is_a_torn_tail_not_a_corrupt_record) {
  const std::string root = dcf::test::make_scratch_directory("store-zero-tail");
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    DCF_REQUIRE(store.close().has_value());
  }
  const std::string journal = root + "/journal.1.dcflog";
  const std::uint64_t clean_size = size_of(journal);
  {
    std::ofstream file(journal, std::ios::binary | std::ios::app);
    const std::vector<char> zeroes(64, '\0');
    file.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
    file.close();
  }
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(opened.has_value());
  JournalStore store = std::move(opened).value();
  DCF_CHECK_EQ(store.recovery().torn_tail_bytes_removed, std::uint64_t{64});
  DCF_REQUIRE(store.close().has_value());
  DCF_CHECK_EQ(size_of(journal), clean_size);
}

DCF_TEST(store, interior_corruption_is_refused_and_never_truncated_through) {
  const std::string root = dcf::test::make_scratch_directory("store-interior");
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    DCF_REQUIRE(store.close().has_value());
  }
  const std::string journal = root + "/journal.1.dcflog";
  const std::uint64_t size = size_of(journal);
  DCF_CHECK(size > 60);

  // A single flipped byte inside the middle of one record. The record is
  // complete, so this is damage in place, not a torn write.
  write_bytes_at(journal, 32, {0x5a});

  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(!opened.has_value());
  DCF_CHECK_EQ(opened.error().code, ErrorCode::InteriorCorruption);
  // The damaged segment was left exactly as it was found.
  DCF_CHECK_EQ(size_of(journal), size);
}

DCF_TEST(store, a_complete_final_record_with_a_bad_checksum_is_corruption) {
  const std::string root = dcf::test::make_scratch_directory("store-tail-crc");
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    DCF_REQUIRE(store.close().has_value());
  }
  const std::string journal = root + "/journal.1.dcflog";
  const std::uint64_t size = size_of(journal);
  write_bytes_at(journal, size - 3, {0x77});

  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(!opened.has_value());
  DCF_CHECK_EQ(opened.error().code, ErrorCode::InteriorCorruption);
}

DCF_TEST(store, compaction_rotates_the_journal_and_keeps_the_state) {
  const std::string root = dcf::test::make_scratch_directory("store-compact");
  Digest expected;
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    expected = engine.state().canonical_digest();
    DCF_REQUIRE(store.compact(engine.state()).has_value());
    DCF_REQUIRE(store.close().has_value());
  }
  {
    // The superseded segment must be gone and the snapshot must be in use.
    const auto entries = platform::list_directory(root);
    DCF_REQUIRE(entries.has_value());
    std::size_t snapshots = 0;
    std::size_t journals = 0;
    for (const std::string& name : entries.value()) {
      if (name.rfind("state.", 0) == 0) {
        ++snapshots;
      }
      if (name.rfind("journal.", 0) == 0) {
        ++journals;
      }
    }
    DCF_CHECK_EQ(snapshots, std::size_t{1});
    DCF_CHECK_EQ(journals, std::size_t{1});
  }
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    DCF_CHECK(store.recovery().snapshot_sequence > 0);
    DCF_CHECK_EQ(store.recovery().entries_replayed, std::uint64_t{0});
    DCF_CHECK_EQ(store.recovered_state().canonical_digest(), expected);
    DCF_REQUIRE(store.close().has_value());
  }
}

DCF_TEST(store, writes_after_compaction_survive_another_reopen) {
  const std::string root = dcf::test::make_scratch_directory("store-compact-write");
  Digest expected;
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    DCF_REQUIRE(store.compact(engine.state()).has_value());

    Command command;
    command.payload = SetConnectivityCommand{kAlpha, LinkState::Degraded};
    auto plan = engine.evaluate(command);
    DCF_REQUIRE(plan.has_value());
    DCF_REQUIRE(store.commit(plan.value().entry).has_value());
    DCF_REQUIRE(engine.commit(plan.value()).has_value());
    expected = engine.state().canonical_digest();
    DCF_REQUIRE(store.close().has_value());
  }
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(opened.has_value());
  JournalStore store = std::move(opened).value();
  DCF_CHECK_EQ(store.recovery().entries_replayed, std::uint64_t{1});
  DCF_CHECK_EQ(store.recovered_state().canonical_digest(), expected);
  DCF_REQUIRE(store.close().has_value());
}

DCF_TEST(store, orphaned_files_from_a_crash_during_compaction_are_removed) {
  const std::string root = dcf::test::make_scratch_directory("store-orphans");
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    DCF_REQUIRE(store.close().has_value());
  }
  // Files written by a compaction that crashed before it replaced CURRENT.
  {
    std::ofstream orphan(root + "/state.999.dcfsnap", std::ios::binary);
    orphan << "not a real snapshot";
  }
  {
    std::ofstream orphan(root + "/journal.1000.dcflog", std::ios::binary);
    orphan << "not a real segment";
  }
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(opened.has_value());
  JournalStore store = std::move(opened).value();
  DCF_CHECK_EQ(store.recovery().orphaned_files_removed.size(), std::size_t{2});
  DCF_REQUIRE(store.close().has_value());
}

DCF_TEST(store, a_missing_snapshot_named_by_current_is_refused) {
  const std::string root = dcf::test::make_scratch_directory("store-missing-snapshot");
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    DCF_REQUIRE(store.compact(engine.state()).has_value());
    DCF_REQUIRE(store.close().has_value());
  }
  const auto entries = platform::list_directory(root);
  DCF_REQUIRE(entries.has_value());
  for (const std::string& name : entries.value()) {
    if (name.rfind("state.", 0) == 0) {
      static_cast<void>(platform::remove_file(root + "/" + name));
    }
  }
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(!opened.has_value());
  DCF_CHECK_EQ(opened.error().code, ErrorCode::Corruption);
}

DCF_TEST(store, a_damaged_current_file_is_refused) {
  const std::string root = dcf::test::make_scratch_directory("store-bad-current");
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    DCF_REQUIRE(store.close().has_value());
  }
  {
    std::ofstream file(root + "/CURRENT", std::ios::binary | std::ios::trunc);
    file << "not-a-number\n-\njournal.1.dcflog\n";
  }
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(!opened.has_value());
  DCF_CHECK_EQ(opened.error().code, ErrorCode::Corruption);
}

DCF_TEST(store, a_second_open_of_the_same_root_is_locked_out) {
  const std::string root = dcf::test::make_scratch_directory("store-lock");
  auto first = JournalStore::open(root, Limits{});
  DCF_REQUIRE(first.has_value());
  JournalStore store = std::move(first).value();
  const auto second = JournalStore::open(root, Limits{});
  DCF_CHECK(!second.has_value());
  DCF_CHECK_EQ(second.error().code, ErrorCode::Locked);
  DCF_REQUIRE(store.close().has_value());
}

DCF_TEST(store, a_reordered_or_replayed_entry_is_refused) {
  const std::string root = dcf::test::make_scratch_directory("store-order");
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(opened.has_value());
  JournalStore store = std::move(opened).value();

  const JournalEntry genesis = genesis_entry(kFederation, UnixMillis{1000});
  DCF_REQUIRE(store.commit(genesis).has_value());
  // Replaying the same sequence is refused rather than applied twice.
  const auto replay = store.commit(genesis);
  DCF_REQUIRE(!replay.has_value());
  DCF_CHECK_EQ(replay.error().code, ErrorCode::Conflict);

  JournalEntry ahead;
  ahead.sequence = JournalSequence{5};
  ahead.generation = FederationGeneration{2};
  ahead.changes.push_back(ConnectivityChangedMutation{kAlpha, LinkState::Connected});
  const auto skipped = store.commit(ahead);
  DCF_REQUIRE(!skipped.has_value());
  DCF_CHECK_EQ(skipped.error().code, ErrorCode::Conflict);
  DCF_REQUIRE(store.close().has_value());
}

DCF_TEST(store, compaction_refuses_a_state_that_is_ahead_of_the_journal) {
  const std::string root = dcf::test::make_scratch_directory("store-compact-ahead");
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(opened.has_value());
  JournalStore store = std::move(opened).value();
  FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
  static_cast<void>(engine.replay(genesis_entry(kFederation, UnixMillis{1000})));
  const auto refused = store.compact(engine.state());
  DCF_REQUIRE(!refused.has_value());
  DCF_CHECK_EQ(refused.error().code, ErrorCode::Conflict);
  DCF_REQUIRE(store.close().has_value());
}

DCF_TEST(store, a_truncated_snapshot_is_refused) {
  const std::string root = dcf::test::make_scratch_directory("store-truncated-snapshot");
  {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    seed_entries(store, engine);
    DCF_REQUIRE(store.compact(engine.state()).has_value());
    DCF_REQUIRE(store.close().has_value());
  }
  const auto entries = platform::list_directory(root);
  DCF_REQUIRE(entries.has_value());
  for (const std::string& name : entries.value()) {
    if (name.rfind("state.", 0) == 0) {
      const std::string path = root + "/" + name;
      truncate_to(path, size_of(path) / 2);
    }
  }
  auto opened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(!opened.has_value());
  DCF_CHECK_EQ(opened.error().code, ErrorCode::Corruption);
}
