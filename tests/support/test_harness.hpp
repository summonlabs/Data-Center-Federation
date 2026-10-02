// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dcf::test {

struct Failure {
  const char* file{};
  int line{0};
  std::string message{};
};

class Context {
 public:
  class Check;

  void fail(const char* file, int line, std::string message) {
    failures_.push_back(Failure{file, line, std::move(message)});
  }
  [[nodiscard]] const std::vector<Failure>& failures() const noexcept { return failures_; }
  [[nodiscard]] std::uint64_t checks() const noexcept { return checks_; }
  void count_check() noexcept { ++checks_; }
  [[nodiscard]] bool ok() const noexcept { return failures_.empty(); }
  void note(std::string text) { notes_.push_back(std::move(text)); }
  [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
  Check check(const char* file, int line) noexcept;

 private:
  std::vector<Failure> failures_{};
  std::vector<std::string> notes_{};
  std::uint64_t checks_{0};
};

template <class T, class = void>
struct IsStreamable : std::false_type {};

template <class T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (IsStreamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<value>";
  }
}

[[nodiscard]] inline std::string describe(const std::string& value) { return value; }
[[nodiscard]] inline std::string describe(const char* value) {
  return value == nullptr ? std::string("<null>") : std::string(value);
}
[[nodiscard]] inline std::string describe(bool value) { return value ? "true" : "false"; }

// Checks are made through ordinary functions rather than multi-line macros. A
// macro that binds a reference to an expression such as decoder.text().value()
// would dangle: the Result temporary dies at the end of the initialising
// statement and reference lifetime extension does not reach through the
// accessor call. Reference parameters of a function call are safe, because the
// caller's temporaries live until the call returns.
class Context::Check {
 public:
  Check(Context& context, const char* file, int line) noexcept
      : context_(context), file_(file), line_(line) {}

  void is_true(bool value, const char* text) {
    context_.count_check();
    if (!value) {
      context_.fail(file_, line_, std::string("check failed: ") + text);
    }
  }

  template <class Left, class Right>
  void eq(const Left& lhs, const Right& rhs, const char* lhs_text, const char* rhs_text) {
    context_.count_check();
    if (!(lhs == rhs)) {
      context_.fail(file_, line_,
                    std::string("expected ") + lhs_text + " == " + rhs_text + " but got " +
                        describe(lhs) + " vs " + describe(rhs));
    }
  }

  template <class Left, class Right>
  void ne(const Left& lhs, const Right& rhs, const char* lhs_text, const char* rhs_text) {
    context_.count_check();
    if (lhs == rhs) {
      context_.fail(file_, line_,
                    std::string("expected ") + lhs_text + " != " + rhs_text +
                        " but both were " + describe(lhs));
    }
  }

 private:
  Context& context_;
  const char* file_;
  int line_;
};

inline Context::Check Context::check(const char* file, int line) noexcept {
  return Check(*this, file, line);
}

using Body = void (*)(Context&);

void register_case(const char* suite, const char* name, Body body);
// Runs every registered case, or the subset selected on the command line.
// Returns the number of failed checks across all cases.
int run_all(int argc, char** argv);
// The directory the running test executable was started in. Suites use it for
// scratch state so that CTest can give each suite its own working tree.
[[nodiscard]] std::string scratch_directory();
// Creates a uniquely named subdirectory under the scratch directory.
[[nodiscard]] std::string make_scratch_directory(const std::string& label);

struct Registrar {
  Registrar(const char* suite, const char* name, Body body) { register_case(suite, name, body); }
};

}  // namespace dcf::test

#define DCF_TEST(suite, name) static void dcf_case_##suite##_##name(::dcf::test::Context& dcf_ctx); static const ::dcf::test::Registrar dcf_registrar_##suite##_##name(#suite, #name, &dcf_case_##suite##_##name); static void dcf_case_##suite##_##name(::dcf::test::Context& dcf_ctx)
#define DCF_CHECK(condition) dcf_ctx.check(__FILE__, __LINE__).is_true(static_cast<bool>(condition), #condition)
#define DCF_REQUIRE(condition) do { if (!(condition)) { dcf_ctx.fail(__FILE__, __LINE__, std::string("requirement failed: ") + #condition); return; } } while (false)
#define DCF_CHECK_EQ(lhs, rhs) dcf_ctx.check(__FILE__, __LINE__).eq((lhs), (rhs), #lhs, #rhs)
#define DCF_CHECK_NE(lhs, rhs) dcf_ctx.check(__FILE__, __LINE__).ne((lhs), (rhs), #lhs, #rhs)
