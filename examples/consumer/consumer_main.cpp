// Data Center Federation - downstream consumer.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// A program that uses the installed federation runtime for something small and
// real: it opens a store, records a compatibility window and a site through the
// public engine API, prints the canonical authority digest, closes the store,
// reopens it, and checks that the recovered state is identical. If any public
// header were missing from the installation, or the transitive thread and socket
// dependencies were not propagated by the exported target, this would not link
// or would not run.
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "dcf/engine.hpp"
#include "dcf/store.hpp"

namespace {

class FixedClock final : public dcf::Clock {
 public:
  [[nodiscard]] dcf::UnixMillis wall_clock() const noexcept override {
    return dcf::UnixMillis{1700000000000LL};
  }
};

[[nodiscard]] dcf::CompatibilityDeclaration declaration_for(const char* implementation) {
  dcf::CompatibilityDeclaration declaration;
  declaration.implementation = implementation;
  declaration.implementation_version = dcf::Version{1, 0};
  declaration.protocol = dcf::VersionRange{dcf::Version{1, 0}, dcf::Version{9, 9}};
  dcf::CapabilityDeclaration capability;
  capability.id = dcf::CapabilityId{"dcf.membership"};
  capability.supported = dcf::VersionRange{dcf::Version{1, 0}, dcf::Version{2, 0}};
  declaration.capabilities.push_back(capability);
  return declaration;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "dcf-consumer-store";
  const dcf::FederationId federation{0xabc0ULL, 0xdef0ULL};
  const dcf::SiteId site{0x0001ULL, 0x0002ULL};

  dcf::FederationEngine engine(dcf::Limits{}, std::make_shared<FixedClock>());
  const dcf::JournalEntry genesis = dcf::genesis_entry(federation, dcf::UnixMillis{1700000000000LL});
  if (!engine.replay(genesis)) {
    std::cerr << "the consumer could not establish a federation\n";
    return 1;
  }

  auto opened = dcf::JournalStore::open(root, dcf::Limits{});
  if (!opened) {
    std::cerr << "the consumer could not open a store: " << opened.error().detail << "\n";
    return 1;
  }
  dcf::JournalStore store = std::move(opened).value();
  if (!store.recovered_state().initialized()) {
    if (!store.commit(genesis)) {
      std::cerr << "the consumer could not commit the genesis entry\n";
      return 1;
    }
  }

  dcf::Command window;
  dcf::CompatibilityWindow published;
  published.capability = dcf::CapabilityId{"dcf.membership"};
  published.offered = dcf::VersionRange{dcf::Version{1, 0}, dcf::Version{3, 0}};
  published.effective_from = dcf::FederationGeneration{1};
  window.payload = dcf::DeclareWindowCommand{published};

  dcf::Command registration;
  registration.payload =
      dcf::RegisterSiteCommand{site, "consumer-site", declaration_for("summon.consumer")};
  registration.origin = site;

  for (const dcf::Command& command : {window, registration}) {
    const auto plan = engine.evaluate(command);
    if (!plan) {
      std::cerr << "the consumer could not evaluate a command: " << plan.error().detail << "\n";
      return 1;
    }
    if (plan.value().persists) {
      if (!store.commit(plan.value().entry)) {
        std::cerr << "the consumer could not commit: " << "commit failed\n";
        return 1;
      }
      if (!engine.commit(plan.value())) {
        std::cerr << "the consumer could not apply the committed entry\n";
        return 1;
      }
    }
  }

  const dcf::Digest digest = engine.state().authority_digest();
  std::cout << "federation " << dcf::to_string(engine.state().federation()) << "\n";
  std::cout << "generation " << engine.state().generation().value() << "\n";
  std::cout << "sites " << engine.state().site_count() << "\n";
  std::cout << "authority " << digest.to_hex() << "\n";
  if (!store.compact(engine.state())) {
    std::cerr << "the consumer could not compact\n";
    return 1;
  }
  if (!store.close()) {
    std::cerr << "the consumer could not close the store\n";
    return 1;
  }

  auto reopened = dcf::JournalStore::open(root, dcf::Limits{});
  if (!reopened) {
    std::cerr << "the consumer could not reopen the store: " << reopened.error().detail << "\n";
    return 1;
  }
  dcf::JournalStore again = std::move(reopened).value();
  const dcf::Digest recovered = again.recovered_state().authority_digest();
  const bool same = recovered == digest;
  std::cout << "recovered " << recovered.to_hex() << "\n";
  std::cout << (same ? "CONSUMER-OK" : "CONSUMER-MISMATCH") << "\n";
  if (!again.close()) {
    return 1;
  }
  return same ? 0 : 1;
}
