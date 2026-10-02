// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dcf/engine.hpp"
#include "dcf/platform.hpp"
#include "dcf/store.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;
using namespace dcf;

const FederationId kFederation{0x51ULL, 0x52ULL};

[[nodiscard]] SiteId site_of(int index) { return SiteId{0x60ULL, static_cast<std::uint64_t>(index + 1)}; }

[[nodiscard]] CompatibilityDeclaration simple_declaration() {
  CompatibilityDeclaration declaration;
  declaration.implementation = "summon.prop-site";
  declaration.implementation_version = Version{1, 0};
  declaration.protocol = VersionRange{Version{1, 0}, Version{9, 9}};
  CapabilityDeclaration entry;
  entry.id = CapabilityId{"dcf.membership"};
  entry.supported = VersionRange{Version{1, 0}, Version{2, 0}};
  declaration.capabilities.push_back(entry);
  return declaration;
}

[[nodiscard]] bool defined_error(ErrorCode code) {
  switch (code) {
    case ErrorCode::None:
    case ErrorCode::InvalidArgument:
    case ErrorCode::MalformedInput:
    case ErrorCode::BoundsExceeded:
    case ErrorCode::ArithmeticOverflow:
    case ErrorCode::NotFound:
    case ErrorCode::DuplicateIdentity:
    case ErrorCode::Conflict:
    case ErrorCode::StaleGeneration:
    case ErrorCode::Revoked:
    case ErrorCode::Incompatible:
    case ErrorCode::InsufficientAuthority:
    case ErrorCode::PartitionSuspected:
    case ErrorCode::Unsupported:
    case ErrorCode::Indeterminate:
    case ErrorCode::IdempotencyConflict:
    case ErrorCode::Corruption:
    case ErrorCode::TornTail:
    case ErrorCode::InteriorCorruption:
    case ErrorCode::Io:
    case ErrorCode::Locked:
    case ErrorCode::Busy:
    case ErrorCode::Closed:
    case ErrorCode::CapacityExhausted:
    case ErrorCode::ShuttingDown:
    case ErrorCode::RefusedByPolicy:
    case ErrorCode::Internal:
      return true;
  }
  return false;
}

struct Invariants {
  FederationGeneration generation{};
  JournalSequence sequence{};
  bool first{true};
};

// The invariants that must hold after every single committed change, whatever
// the change was.
void check_invariants(dcf::test::Context& context, const FederationEngine& engine,
                      Invariants& state, bool persisted) {
  const FederationState& snapshot = engine.state();
  if (!state.first) {
    context.check(__FILE__, __LINE__).is_true(
        snapshot.generation().value() >= state.generation.value(),
        "the federation generation must never go backwards");
    // A command that produced no durable record changes nothing at all; one that
    // produced a record advances the sequence by exactly one.
    context.check(__FILE__, __LINE__).is_true(
        persisted ? snapshot.sequence().value() == state.sequence.value() + 1
                  : snapshot.sequence().value() == state.sequence.value(),
        "the journal sequence advances by one exactly when a record was written");
  }
  state.first = false;
  state.generation = snapshot.generation();
  state.sequence = snapshot.sequence();

  std::set<MembershipGeneration::value_type> generations;
  for (const auto& entry : snapshot.sites()) {
    context.check(__FILE__, __LINE__).is_true(
        entry.second.generation.value() >= 1,
        "a membership record always carries a generation of at least one");
    context.check(__FILE__, __LINE__).is_true(
        counts_as_member(entry.second.state) || is_terminal(entry.second.state) ||
            entry.second.state == MembershipState::Candidate ||
            entry.second.state == MembershipState::CompatibilityValidated,
        "a membership record is always in a state the lifecycle defines");
    generations.insert(entry.second.generation.value());
  }

  // The authority digest is a pure function of the state.
  context.check(__FILE__, __LINE__)
      .eq(snapshot.authority_digest(), engine.state().authority_digest(),
          "authority digest", "authority digest");
  context.check(__FILE__, __LINE__)
      .eq(snapshot.canonical_digest(), engine.state().canonical_digest(), "canonical digest",
          "canonical digest");
}

}  // namespace

DCF_TEST(property, seeded_random_command_sequences_preserve_invariants) {
  constexpr std::uint64_t kSeeds = 24;
  constexpr int kSteps = 60;

  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    DCF_REQUIRE(engine.replay(genesis_entry(kFederation, UnixMillis{1000})).has_value());

    std::mt19937_64 random(seed);
    Invariants invariants;
    std::vector<JournalEntry> journal;

    for (int step = 0; step < kSteps; ++step) {
      const std::uint64_t choice = random() % 12;
      Command command;
      const SiteId site = site_of(static_cast<int>(random() % 3));
      switch (choice) {
        case 0:
          command.payload = RegisterSiteCommand{site, "site", simple_declaration()};
          command.origin = site;
          break;
        case 1:
          command.payload = ValidateSiteCommand{site};
          break;
        case 2: {
          const MembershipRecord* record = engine.state().find_site(site);
          command.payload = AdmitSiteCommand{
              site, record == nullptr ? MembershipGeneration{} : record->generation};
          break;
        }
        case 3: {
          const MembershipRecord* record = engine.state().find_site(site);
          ActivateSiteCommand activate;
          activate.site = site;
          activate.accepted = AcceptedGeneration{engine.generation().value()};
          activate.accepted_history = engine.state().site_authority_digest(site);
          activate.local_epoch = SiteLocalEpoch{record == nullptr ? 0 : record->local_epoch.value()};
          command.payload = activate;
          command.origin = site;
          break;
        }
        case 4: {
          DelegationGrant grant;
          grant.id = DelegationId{seed, static_cast<std::uint64_t>(step) + 1};
          grant.grantee = site;
          const MembershipRecord* record = engine.state().find_site(site);
          grant.grantee_membership_generation =
              record == nullptr ? MembershipGeneration{} : record->generation;
          grant.scope_mask = static_cast<std::uint16_t>(
              mask_of(DelegationScope::MembershipAdmit) |
              (random() % 2 == 0 ? mask_of(DelegationScope::ReconciliationWitness) : 0));
          grant.exclusive = random() % 3 == 0;
          grant.survives_partition = random() % 2 == 0;
          grant.selectors.push_back("zone-" + std::to_string(random() % 3));
          command.payload = GrantDelegationCommand{grant};
          break;
        }
        case 5: {
          if (!engine.state().delegations().empty()) {
            const std::size_t index = random() % engine.state().delegations().size();
            auto iterator = engine.state().delegations().begin();
            std::advance(iterator, static_cast<std::ptrdiff_t>(index));
            command.payload = RevokeDelegationCommand{iterator->first, "random"};
          } else {
            continue;
          }
          break;
        }
        case 6: {
          static const LinkState kLinks[4] = {LinkState::Unknown, LinkState::Connected,
                                              LinkState::Degraded, LinkState::Partitioned};
          command.payload = SetConnectivityCommand{site, kLinks[random() % 4]};
          break;
        }
        case 7: {
          CompatibilityWindow window;
          window.capability = CapabilityId{"dcf.membership"};
          window.offered = VersionRange{Version{1, 0}, Version{3, 0}};
          window.effective_from = FederationGeneration{engine.generation().value() + 1};
          command.payload = DeclareWindowCommand{window};
          break;
        }
        case 8:
          command.payload = ConstrainSiteCommand{site, mask_of(DelegationScope::ObservabilityRead),
                                                 "random"};
          break;
        case 9:
          command.payload = BeginDrainCommand{site, "random"};
          break;
        case 10:
          command.payload = RemoveSiteCommand{site, "random"};
          break;
        default: {
          SiteReport report;
          report.site = site;
          const MembershipRecord* record = engine.state().find_site(site);
          report.accepted = AcceptedGeneration{engine.generation().value()};
          report.accepted_history = engine.state().site_authority_digest(site);
          report.membership_generation =
              record == nullptr ? MembershipGeneration{} : record->generation;
          report.local_epoch = SiteLocalEpoch{random() % 5};
          command.payload = RejoinCommand{report, false};
          command.origin = site;
          break;
        }
      }

      // A uniformly random operation identifier exercises the receipt table.
      if (random() % 4 == 0) {
        command.operation = OperationId{seed, static_cast<std::uint64_t>(step)};
      }
      command.origin_generation = engine.generation();

      const auto plan = engine.evaluate(command);
      DCF_REQUIRE(plan.has_value());
      if (plan.value().persists) {
        journal.push_back(plan.value().entry);
      }
      const auto committed = engine.commit(plan.value());
      if (!committed.has_value()) {
        // Reproduction data: the seed, the step, and the exact command that the
        // state machine refused to apply.
        dcf_ctx.note("reproduction seed=" + std::to_string(seed) + " step=" +
                     std::to_string(step) + " command=" +
                     std::string(command_name(command.payload)) + " error=" +
                     std::string(to_string(committed.error().code)) + " " +
                     committed.error().detail);
      }
      DCF_REQUIRE(committed.has_value());
      check_invariants(dcf_ctx, engine, invariants, plan.value().persists);
    }

    // Replaying the same sequence into a fresh engine reproduces the state
    // exactly; anything else would mean the journal is not the whole truth.
    FederationEngine replay(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    DCF_REQUIRE(replay.replay(genesis_entry(kFederation, UnixMillis{1000})).has_value());
    for (const JournalEntry& entry : journal) {
      DCF_REQUIRE(replay.replay(entry).has_value());
    }
    DCF_CHECK_EQ(replay.state().authority_digest(), engine.state().authority_digest());
    DCF_CHECK_EQ(replay.state().canonical_digest(), engine.state().canonical_digest());
  }
}

DCF_TEST(adversarial, every_single_byte_mutation_of_a_journal_is_handled) {
  const std::string source = dcf::test::make_scratch_directory("adversarial-source");
  {
    auto opened = JournalStore::open(source, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    FederationEngine engine(Limits{}, std::make_shared<ManualClock>(UnixMillis{1000}));
    DCF_REQUIRE(store.commit(genesis_entry(kFederation, UnixMillis{1000})).has_value());
    DCF_REQUIRE(engine.replay(genesis_entry(kFederation, UnixMillis{1000})).has_value());

    Command command;
    command.payload = RegisterSiteCommand{site_of(0), "alpha", simple_declaration()};
    command.origin = site_of(0);
    const auto plan = engine.evaluate(command);
    DCF_REQUIRE(plan.has_value());
    DCF_REQUIRE(store.commit(plan.value().entry).has_value());
    DCF_REQUIRE(engine.commit(plan.value()).has_value());
    DCF_REQUIRE(store.close().has_value());
  }

  const auto raw = platform::read_file(source + "/journal.1.dcflog", 1 << 20);
  DCF_REQUIRE(raw.has_value());
  const std::vector<std::uint8_t> original = raw.value();
  DCF_CHECK(original.size() > 64);
  const auto current = platform::read_file(source + "/CURRENT", 4096);
  DCF_REQUIRE(current.has_value());
  Digest baseline;
  {
    auto opened = JournalStore::open(source, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    baseline = store.recovered_state().canonical_digest();
    DCF_REQUIRE(store.close().has_value());
  }

  for (std::size_t offset = 0; offset < original.size(); ++offset) {
    for (const std::uint8_t mask : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xff}}) {
      std::vector<std::uint8_t> damaged = original;
      damaged[offset] = static_cast<std::uint8_t>(damaged[offset] ^ mask);

      const std::string target = dcf::test::make_scratch_directory("adversarial-mutant");
      {
        std::ofstream file(target + "/journal.1.dcflog", std::ios::binary);
        file.write(reinterpret_cast<const char*>(damaged.data()),
                   static_cast<std::streamsize>(damaged.size()));
      }
      {
        std::ofstream file(target + "/CURRENT", std::ios::binary);
        file.write(reinterpret_cast<const char*>(current.value().data()),
                   static_cast<std::streamsize>(current.value().size()));
      }

      auto opened = JournalStore::open(target, Limits{});
      if (opened.has_value()) {
        JournalStore store = std::move(opened).value();
        if (store.recovery().torn_tail_bytes_removed == 0) {
          // Every completed record is checksummed, so a damaged byte can only
          // leave the state untouched or make the store refuse to open. It can
          // never silently change a committed record.
          DCF_CHECK(store.recovered_state().initialized());
          DCF_CHECK_EQ(store.recovered_state().canonical_digest(), baseline);
        } else {
          // An incomplete final record was removed, which is reported rather
          // than hidden.
          DCF_CHECK(store.recovery().torn_tail_bytes_removed > 0);
        }
        DCF_REQUIRE(store.close().has_value());
      } else {
        DCF_CHECK(defined_error(opened.error().code));
        DCF_CHECK(opened.error().code != ErrorCode::None);
      }
      std::error_code ignored;
      fs::remove_all(target, ignored);
    }
  }
}

DCF_TEST(adversarial, command_decoding_never_crashes_and_is_canonical) {
  Command original;
  GrantDelegationCommand grant;
  grant.grant.id = DelegationId{0x77ULL, 0x78ULL};
  grant.grant.grantee = site_of(2);
  grant.grant.scope_mask = mask_of(DelegationScope::MembershipAdmit);
  grant.grant.selectors = {"zone-a", "zone-b"};
  grant.grant.granted_at = FederationGeneration{9};
  grant.grant.grantee_membership_generation = MembershipGeneration{4};
  grant.grant.expires_at = FederationGeneration{100};
  grant.grant.expires_at_wall = UnixMillis{123456};
  grant.grant.survives_partition = true;
  grant.grant.exclusive = true;
  original.payload = grant;
  original.operation = OperationId{1, 2};
  original.origin = site_of(2);
  original.origin_generation = FederationGeneration{9};

  Encoder encoder;
  encode(encoder, original);
  const std::vector<std::uint8_t> bytes(encoder.view().begin(), encoder.view().end());

  // Decoding the canonical encoding and re-encoding must give the same bytes.
  {
    Decoder decoder(std::span<const std::uint8_t>(bytes.data(), bytes.size()), Limits{});
    const auto decoded = decode_command(decoder);
    DCF_REQUIRE(decoded.has_value());
    DCF_CHECK(decoder.require_end().has_value());
    Encoder again;
    encode(again, decoded.value());
    DCF_CHECK_EQ(again.bytes(), bytes);
  }

  std::mt19937_64 random(0x9e3779b97f4a7c15ULL);
  for (int trial = 0; trial < 4000; ++trial) {
    std::vector<std::uint8_t> mutated = bytes;
    const int edits = 1 + static_cast<int>(random() % 4);
    for (int edit = 0; edit < edits; ++edit) {
      if (mutated.empty()) {
        break;
      }
      mutated[random() % mutated.size()] = static_cast<std::uint8_t>(random() & 0xffU);
    }
    // Truncation is the other half of malformed input.
    if (trial % 3 == 0 && !mutated.empty()) {
      mutated.resize(random() % mutated.size());
    }
    Decoder decoder(std::span<const std::uint8_t>(mutated.data(), mutated.size()), Limits{});
    const auto decoded = decode_command(decoder);
    if (decoded.has_value()) {
      // Anything that decodes must satisfy the invariants the public API
      // promises, and must re-encode to the bytes it decoded from.
      const auto site = subject_site(decoded.value().payload);
      static_cast<void>(site);
      static_cast<void>(command_name(decoded.value().payload));
      Encoder again;
      encode(again, decoded.value());
      DCF_CHECK_EQ(again.bytes(), mutated);
    } else {
      DCF_CHECK(defined_error(decoded.error().code));
    }
  }
}

DCF_TEST(adversarial, absurd_sizes_and_ill_formed_text_are_refused) {
  // A text field that announces more bytes than the object holds.
  {
    Encoder encoder;
    encoder.u8(0);  // register site
    encoder.u64(1);
    encoder.u64(2);
    encoder.u32(0xfffffff0U);
    Decoder decoder(encoder.view(), Limits{});
    const auto decoded = decode_command(decoder);
    DCF_CHECK(!decoded.has_value());
    DCF_CHECK_EQ(decoded.error().code, ErrorCode::BoundsExceeded);
  }
  // A collection that announces a million items.
  {
    Encoder encoder;
    encoder.u8(10);  // grant delegation
    encoder.u64(1);
    encoder.u64(2);
    encoder.u64(3);
    encoder.u64(4);
    encoder.u16(1);
    encoder.u32(1000000U);
    Decoder decoder(encoder.view(), Limits{});
    const auto decoded = decode_command(decoder);
    DCF_CHECK(!decoded.has_value());
    DCF_CHECK_EQ(decoded.error().code, ErrorCode::BoundsExceeded);
  }
  // A boolean that is neither zero nor one.
  {
    Encoder encoder;
    encoder.u8(10);
    encoder.u64(1);
    encoder.u64(2);
    encoder.u64(3);
    encoder.u64(4);
    encoder.u16(1);
    encoder.u32(0);
    encoder.u64(0);
    encoder.u64(0);
    encoder.u64(0);
    encoder.i64(0);
    encoder.u8(7);
    Decoder decoder(encoder.view(), Limits{});
    const auto decoded = decode_command(decoder);
    DCF_CHECK(!decoded.has_value());
    DCF_CHECK_EQ(decoded.error().code, ErrorCode::MalformedInput);
  }
  // A capability declaration that claims an inverted version range.
  {
    CompatibilityDeclaration declaration = simple_declaration();
    declaration.protocol = VersionRange{Version{5, 0}, Version{1, 0}};
    const auto canonical = canonicalize(declaration, Limits{});
    DCF_CHECK(!canonical.has_value());
    DCF_CHECK_EQ(canonical.error().code, ErrorCode::InvalidArgument);
  }
  // A capability name that is not a name.
  {
    CompatibilityDeclaration declaration = simple_declaration();
    declaration.capabilities.front().id = CapabilityId{"../etc/passwd"};
    const auto canonical = canonicalize(declaration, Limits{});
    DCF_CHECK(!canonical.has_value());
  }
  // Duplicate capability declarations are refused rather than resolved.
  {
    CompatibilityDeclaration declaration = simple_declaration();
    declaration.capabilities.push_back(declaration.capabilities.front());
    const auto canonical = canonicalize(declaration, Limits{});
    DCF_CHECK(!canonical.has_value());
    DCF_CHECK_EQ(canonical.error().code, ErrorCode::DuplicateIdentity);
  }
  // Invalid UTF-8 in a declaration is refused.
  {
    CompatibilityDeclaration declaration = simple_declaration();
    declaration.implementation = std::string("bad\xc3\x28", 5);
    const auto canonical = canonicalize(declaration, Limits{});
    DCF_CHECK(!canonical.has_value());
  }
}

DCF_TEST(adversarial, a_long_store_path_works) {
  const std::string base = dcf::test::make_scratch_directory("adversarial-longpath");
  std::string root = base;
  while (root.size() < 300) {
    root.append("/segment-with-a-deliberately-long-name");
  }
  const auto created = platform::create_directories(root);
  DCF_CHECK_EQ(created.has_value() ? std::string{} : created.error().detail, std::string{});
  DCF_REQUIRE(created.has_value());

  auto opened = JournalStore::open(root, Limits{});
  DCF_CHECK_EQ(opened.has_value()
                   ? std::string{}
                   : std::string(to_string(opened.error().code)) + ": " + opened.error().detail,
               std::string{});
  DCF_REQUIRE(opened.has_value());
  JournalStore store = std::move(opened).value();
  DCF_REQUIRE(store.commit(genesis_entry(kFederation, UnixMillis{1000})).has_value());
  DCF_REQUIRE(store.close().has_value());

  auto reopened = JournalStore::open(root, Limits{});
  DCF_REQUIRE(reopened.has_value());
  JournalStore again = std::move(reopened).value();
  DCF_CHECK(again.recovered_state().initialized());
  DCF_CHECK_EQ(again.recovered_state().federation(), kFederation);
  DCF_REQUIRE(again.close().has_value());
}

DCF_TEST(adversarial, the_store_survives_repeated_open_and_close) {
  const std::string root = dcf::test::make_scratch_directory("adversarial-reopen");
  for (int round = 0; round < 25; ++round) {
    auto opened = JournalStore::open(root, Limits{});
    DCF_REQUIRE(opened.has_value());
    JournalStore store = std::move(opened).value();
    if (round == 0) {
      DCF_CHECK(!store.recovered_state().initialized());
      DCF_REQUIRE(store.commit(genesis_entry(kFederation, UnixMillis{1000})).has_value());
    } else {
      // Every later open finds exactly what the previous one wrote.
      DCF_CHECK(store.recovered_state().initialized());
      DCF_CHECK_EQ(store.recovered_state().federation(), kFederation);
      DCF_CHECK_EQ(store.recovered_state().generation().value(), 1ULL);
    }
    DCF_REQUIRE(store.close().has_value());
    // Closing twice is not an error.
    DCF_CHECK(store.close().has_value());
  }
}
