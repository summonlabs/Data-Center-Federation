// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>

namespace dcf::test {
namespace {

struct Case {
  std::string suite{};
  std::string name{};
  Body body{};
};

std::vector<Case>& cases() {
  static std::vector<Case> registry;
  return registry;
}

std::string g_scratch{};

[[nodiscard]] std::string random_suffix() {
  static std::mt19937_64 engine([] {
    std::random_device device;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::seed_seq seed{device(), device(), static_cast<unsigned>(now & 0xffffffffLL)};
    return std::mt19937_64(seed);
  }());
  static std::uint64_t counter = 0;
  ++counter;
  std::ostringstream stream;
  stream << std::hex << engine() << '-' << counter;
  return stream.str();
}

}  // namespace

void register_case(const char* suite, const char* name, Body body) {
  cases().push_back(Case{suite, name, body});
}

std::string scratch_directory() {
  if (g_scratch.empty()) {
    g_scratch = ".";
  }
  return g_scratch;
}

std::string make_scratch_directory(const std::string& label) {
  const std::filesystem::path base = std::filesystem::path(scratch_directory()) / "cases" /
                                     (label + "-" + random_suffix());
  std::error_code code;
  std::filesystem::remove_all(base, code);
  std::filesystem::create_directories(base, code);
  return base.string();
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--list") {
      list_only = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = std::string(argument.substr(9));
    } else if (argument.rfind("--scratch=", 0) == 0) {
      g_scratch = std::string(argument.substr(10));
    }
  }

  std::sort(cases().begin(), cases().end(), [](const Case& lhs, const Case& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  if (list_only) {
    for (const Case& item : cases()) {
      std::cout << item.suite << "." << item.name << "\n";
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed_cases = 0;
  int failed_checks = 0;
  std::uint64_t total_checks = 0;

  for (const Case& item : cases()) {
    const std::string full = item.suite + "." + item.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    Context context;
    try {
      item.body(context);
    } catch (const std::exception& error) {
      context.fail(__FILE__, __LINE__,
                   std::string("case threw std::exception: ") + error.what());
    } catch (...) {
      context.fail(__FILE__, __LINE__, "case threw a non-standard exception");
    }

    total_checks += context.checks();
    if (context.ok()) {
      std::cout << "[  ok  ] " << full << "\n";
    } else {
      ++failed_cases;
      std::cout << "[ FAIL ] " << full << "\n";
      for (const Failure& failure : context.failures()) {
        ++failed_checks;
        std::cout << "         " << failure.file << ":" << failure.line << ": "
                  << failure.message << "\n";
      }
    }
    for (const std::string& note : context.notes()) {
      std::cout << "         note: " << note << "\n";
    }
    std::cout.flush();
  }

  std::cout << "\n" << executed << " case(s) run, " << total_checks << " check(s) executed, "
            << failed_cases << " failed, " << failed_checks << " failing check(s)\n";
  std::cout.flush();
  return failed_checks;
}

}  // namespace dcf::test

int main(int argc, char** argv) {
  return dcf::test::run_all(argc, argv) == 0 ? 0 : 1;
}
