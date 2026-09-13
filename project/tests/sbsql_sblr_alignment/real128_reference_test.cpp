// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Component conformance only: this does not exercise SQL, IPC or admission.
#include "sbl_numeric.hpp"
#include "runtime_capabilities.hpp"
#include <mpfr.h>
#include <algorithm>
#include <atomic>
#include <cfenv>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local long allocation_budget = -1;
std::atomic<unsigned> checks{0}, failures{0};
void Check(bool valid, const std::string& detail) {
  ++checks;
  if (!valid && failures.fetch_add(1) < 20) std::cerr << "FAIL " << detail << '\n';
}
}
void* operator new(std::size_t n) {
  if (allocation_budget == 0) throw std::bad_alloc();
  if (allocation_budget > 0) --allocation_budget;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace scratchbird::libraries::sbl_numeric;
struct Real {
  mpfr_t v;
  explicit Real(mpfr_prec_t precision = 512) { mpfr_init2(v, precision); }
  ~Real() { mpfr_clear(v); }
  Real(const Real&) = delete;
};
struct Integer {
  mpz_t v;
  Integer() { mpz_init(v); }
  ~Integer() { mpz_clear(v); }
};
struct Rational {
  mpq_t v;
  Rational() { mpq_init(v); }
  Rational(const Rational& other) { mpq_init(v); mpq_set(v, other.v); }
  Rational& operator=(const Rational& other) { mpq_set(v, other.v); return *this; }
  ~Rational() { mpq_clear(v); }
};
struct Oracle {
  Rational value;
  bool inexact = false, underflow = false, overflow = false, subnormal = false;
};

// Independent integer quotient/remainder oracle. It does not use the
// production p+2 work precision, sticky-bit jamming or MPFR target rounding.
Oracle RoundExact(mpq_srcptr exact, RoundingMode mode) {
  Oracle out;
  if (!mpq_sgn(exact)) return out;
  Integer numerator, denominator, shifted, whole, remainder, twice;
  mpz_abs(numerator.v, mpq_numref(exact));
  mpz_set(denominator.v, mpq_denref(exact));
  long exponent = static_cast<long>(mpz_sizeinbase(numerator.v, 2)) -
                  static_cast<long>(mpz_sizeinbase(denominator.v, 2));
  if (exponent >= 0) {
    mpz_mul_2exp(shifted.v, denominator.v, exponent);
    if (mpz_cmp(numerator.v, shifted.v) < 0) --exponent;
  } else {
    mpz_mul_2exp(shifted.v, numerator.v, -exponent);
    if (mpz_cmp(shifted.v, denominator.v) < 0) --exponent;
  }
  const long quantum = std::max(exponent - 112, -16494L);
  if (quantum < 0) mpz_mul_2exp(numerator.v, numerator.v, -quantum);
  else mpz_mul_2exp(denominator.v, denominator.v, quantum);
  mpz_fdiv_qr(whole.v, remainder.v, numerator.v, denominator.v);
  out.inexact = mpz_sgn(remainder.v) != 0;
  mpz_mul_2exp(twice.v, remainder.v, 1);
  const int midpoint = mpz_cmp(twice.v, denominator.v);
  if (mode != RoundingMode::truncate &&
      (midpoint > 0 || (midpoint == 0 &&
       (mode == RoundingMode::half_up || mpz_odd_p(whole.v)))))
    mpz_add_ui(whole.v, whole.v, 1);
  mpq_set_z(out.value.v, whole.v);
  if (quantum < 0) mpq_div_2exp(out.value.v, out.value.v, -quantum);
  else mpq_mul_2exp(out.value.v, out.value.v, quantum);
  Rational boundary;
  mpq_set_ui(boundary.v, 1, 1);
  mpq_mul_2exp(boundary.v, boundary.v, 16384);
  out.overflow = mpq_cmp(out.value.v, boundary.v) >= 0;
  if (out.overflow) out.inexact = true;
  mpq_set_ui(boundary.v, 1, 1);
  mpq_div_2exp(boundary.v, boundary.v, 16382);
  const bool tiny = mpq_cmp(out.value.v, boundary.v) < 0;
  out.subnormal = tiny && mpq_sgn(out.value.v);
  out.underflow = tiny && out.inexact;
  if (mpq_sgn(exact) < 0) mpq_neg(out.value.v, out.value.v);
  return out;
}
Rational ExactHex(const std::string& text) {
  Real source;
  Check(mpfr_set_str(source.v, text.c_str(), 0, MPFR_RNDN) == 0,
        "oracle hexadecimal syntax");
  Rational exact;
  mpfr_get_q(exact.v, source.v);
  return exact;
}
NumericRequest Request(NumericOperation op, const std::string& a,
                       const std::string& b = "0",
                       RoundingMode mode = RoundingMode::half_even) {
  NumericRequest r;
  r.type = NumericType::real128;
  r.operation = op;
  r.left = {NumericType::real128, a, false};
  r.right = {NumericType::real128, b, false};
  r.context.rounding = mode;
  return r;
}
bool Equal(const std::string& text, mpq_srcptr expected) {
  if (text.empty()) return false;
  Real parsed(113);
  if (mpfr_set_str(parsed.v, text.c_str(), 10, MPFR_RNDN) != 0) return false;
  return mpfr_number_p(parsed.v) && mpfr_cmp_q(parsed.v, expected) == 0;
}
void Differential(const NumericRequest& request) {
  const auto a = RoundExact(ExactHex(request.left.encoded).v, request.context.rounding);
  const bool unary = request.operation == NumericOperation::canonicalize;
  const auto b = unary ? Oracle{} :
      RoundExact(ExactHex(request.right.encoded).v, request.context.rounding);
  Oracle expected = a;
  if (!a.overflow && !b.overflow && !unary) {
    Rational exact;
    switch (request.operation) {
      case NumericOperation::add: mpq_add(exact.v, a.value.v, b.value.v); break;
      case NumericOperation::subtract: mpq_sub(exact.v, a.value.v, b.value.v); break;
      case NumericOperation::multiply: mpq_mul(exact.v, a.value.v, b.value.v); break;
      case NumericOperation::divide:
        if (!mpq_sgn(b.value.v)) return; // Dedicated exceptional-value fixtures below.
        mpq_div(exact.v, a.value.v, b.value.v); break;
      default: std::abort();
    }
    expected = RoundExact(exact.v, request.context.rounding);
    expected.inexact |= a.inexact || b.inexact;
    expected.underflow |= a.underflow || b.underflow;
  }
  const auto got = ApplyNumericOperation(request);
  const auto label = request.left.encoded + " " + NumericOperationName(request.operation) +
                     " " + request.right.encoded + " mode=" +
                     std::to_string(static_cast<unsigned>(request.context.rounding));
  if (a.overflow || b.overflow || expected.overflow) {
    Check(got.status == NumericStatusCode::overflow && got.overflow &&
          got.inexact && !got.subnormal && got.value.encoded.empty() &&
          got.diagnostic_code == "NUMERIC.REAL128.OVERFLOW", label + " overflow");
    return;
  }
  Check(got.status == NumericStatusCode::ok, label + " status");
  Check(Equal(got.value.encoded, expected.value.v), label + " result=" + got.value.encoded);
  Check(got.inexact == expected.inexact, label + " inexact");
  Check(got.underflow == expected.underflow, label + " underflow");
  Check(got.subnormal == expected.subnormal, label + " subnormal");
  Check(!got.invalid && !got.divide_by_zero && !got.overflow, label + " exceptions");
  const auto canonical = ApplyNumericOperation(Request(NumericOperation::canonicalize, got.value.encoded));
  Check(canonical.status == NumericStatusCode::ok &&
        canonical.value.encoded == got.value.encoded, label + " render round trip");
}

void Arithmetic() {
  std::vector<std::string> values{
      "0", "-0", "1", "-1", "3", "-3", "0x1p-16494", "-0x1p-16494",
      "0x1p-16495", "-0x1p-16495", "0x1.00000000000000000000000000000001p-16495",
      "0x1.ffffffffffffffffffffffffffffp-16383", "0x1p-16382",
      "0x1.fffffffffffffffffffffffffffep-16383",
      "0x1.ffffffffffffffffffffffffffffp16383", "0x1p16384",
      "0x1.ffffffffffffffffffffffffffff7p16383",
      "0x1.ffffffffffffffffffffffffffff8p16383",
      "0x1.00000000000000000000000000000000000001p0",
      "0x1.0000000000000000000000000000008p0",
      "0x1.00000000000000000000000000000080000001p0",
      "0x1.0000000000000000000000000000007fffffffp0",
      "0x1p-16600", "0x1p16490", "0x1.8p-112", "0x1p-113"};
  std::mt19937_64 random(0x12811316494ULL);
  for (unsigned i = 0; i < 480; ++i) {
    std::string text = (random() & 1) ? "-0x1." : "0x1.";
    for (unsigned n = 0; n < 40; ++n) text += "0123456789abcdef"[random() & 15];
    const long e = (i % 3 == 0) ? -16500 + static_cast<long>(random() % 150) :
        (i % 3 == 1) ? 16300 + static_cast<long>(random() % 100) :
        static_cast<long>(random() % 32800) - 16400;
    values.push_back(text + "p" + std::to_string(e));
  }
  for (const auto mode : {RoundingMode::half_even, RoundingMode::half_up, RoundingMode::truncate}) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      Differential(Request(NumericOperation::canonicalize, values[i], "0", mode));
      for (const auto op : {NumericOperation::add, NumericOperation::subtract,
                            NumericOperation::multiply, NumericOperation::divide})
        Differential(Request(op, values[i], values[(i * 17 + 3) % values.size()], mode));
    }
    // Explicit positive/negative tie and near-tie arithmetic, not just parsing.
    for (const auto& a : {"1", "-1", "0x1.0000000000000000000000000001p0",
                          "-0x1.0000000000000000000000000001p0"}) {
      for (const auto& b : {"0x1p-113", "-0x1p-113", "0x1.8p-112",
                            "-0x1.8p-112", "0x1.ffffffffffffffffffffffffffffp-114"})
        Differential(Request(NumericOperation::add, a, b, mode));
    }
  }
}

void DecimalOracle() {
  // MPFR's documented 113-bit/subnormalize route is independent of both the
  // production round-to-odd implementation and the integer rational oracle.
  for (const auto mode : {RoundingMode::half_even, RoundingMode::truncate}) {
    const auto rnd = mode == RoundingMode::half_even ? MPFR_RNDN : MPFR_RNDZ;
    for (const auto text : {"0.1", "-0.1", "1e-4966", "-1e-4966", "6.47517511943802511092443895822764655e-4966",
                           "3.36210314311209350626267781732175260e-4932",
                           "1.189731495357231765085759326628007e4932",
                           "340282366920938463463374607431768211455"}) {
      Real oracle(113);
      const auto emin = mpfr_get_emin(), emax = mpfr_get_emax();
      mpfr_set_emin(-16493);
      mpfr_set_emax(16384);
      const auto t = mpfr_strtofr(oracle.v, text, nullptr, 10, rnd);
      const auto exact = mpfr_subnormalize(oracle.v, t, rnd);
      mpfr_set_emin(emin);
      mpfr_set_emax(emax);
      Rational expected;
      mpfr_get_q(expected.v, oracle.v);
      const auto got = ApplyNumericOperation(Request(NumericOperation::canonicalize, text, "0", mode));
      Check(got.status == NumericStatusCode::ok && Equal(got.value.encoded, expected.v),
            std::string("decimal reference ") + text);
      Check(got.inexact == (exact != 0), std::string("decimal inexact ") + text);
    }
  }
}

void Policy() {
  for (const auto& text : std::vector<std::string>{
      "", "+", ".", "0x", "0xp1", "1e", "1e+", "0x1p", "0b10", "1@2", "1,5",
      "1 2", "1x", "nan(1)", "infinityx", std::string("1\0", 2)}) {
    const auto r = ApplyNumericOperation(Request(NumericOperation::canonicalize, text));
    Check(r.status == NumericStatusCode::invalid_left && r.invalid &&
          r.value.encoded.empty(), "invalid lexical");
  }
  for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
      {" \t+1.25\r\n", "1.25"}, {"0x1.8p1", "3"}, {"-0", "-0"},
      {"0.0001220703125", "0.0001220703125"},
      {"0.000030517578125", "3.0517578125e-5"},
      {"1e36", "1e+36"}}) {
    const auto r = ApplyNumericOperation(Request(NumericOperation::canonicalize, pair.first));
    Check(r.status == NumericStatusCode::ok && r.value.encoded == pair.second, "canonical render " + pair.first);
  }
  for (const auto text : {"NaN", "sNaN", "Infinity", "-Inf", "+SNAN"}) {
    auto r = Request(NumericOperation::canonicalize, text);
    auto got = ApplyNumericOperation(r);
    Check(got.status == NumericStatusCode::invalid_left && got.invalid, "special disabled");
    r.context.allow_special_values = true;
    got = ApplyNumericOperation(r);
    Check(got.status == NumericStatusCode::ok, "special enabled");
  }
  for (const auto& entry : std::vector<std::pair<NumericOperation, std::pair<std::string, std::string>>>{
      {NumericOperation::add, {"Infinity", "-Infinity"}},
      {NumericOperation::subtract, {"Infinity", "Infinity"}},
      {NumericOperation::multiply, {"0", "Infinity"}},
      {NumericOperation::divide, {"0", "0"}},
      {NumericOperation::divide, {"Infinity", "Infinity"}},
      {NumericOperation::add, {"sNaN", "1"}},
      {NumericOperation::compare, {"1", "sNaN"}}}) {
    auto request = Request(entry.first, entry.second.first, entry.second.second);
    request.context.allow_special_values = true;
    const auto got = ApplyNumericOperation(request);
    Check(got.status == NumericStatusCode::invalid_operation && got.invalid &&
          got.value.encoded.empty() && got.diagnostic_code == "NUMERIC.REAL128.INVALID", "invalid operation");
  }
  for (const auto zero : {"0", "-0"}) {
    auto request = Request(NumericOperation::divide, "-2", zero);
    const auto got = ApplyNumericOperation(request);
    Check(got.status == NumericStatusCode::divide_by_zero && got.divide_by_zero &&
          got.diagnostic_code == "NUMERIC.REAL128.DIVIDE_BY_ZERO", "divide by zero");
    request.left.encoded = "Infinity";
    request.context.allow_special_values = true;
    const auto infinite = ApplyNumericOperation(request);
    Check(infinite.status == NumericStatusCode::ok &&
          infinite.value.encoded == (zero[0] == '-' ? "-Infinity" : "Infinity"), "infinity / zero");
  }
  auto request = Request(NumericOperation::compare, "NaN", "1");
  request.context.allow_special_values = true;
  Check(ApplyNumericOperation(request).status == NumericStatusCode::unordered, "NaN unordered");
  request.operation = NumericOperation::add;
  Check(ApplyNumericOperation(request).value.encoded == "NaN", "NaN propagation");
  request = Request(NumericOperation::add, "bad", "also bad");
  request.left.is_null = true;
  Check(ApplyNumericOperation(request).status == NumericStatusCode::null_result, "strict null");
  request.context.rounding = static_cast<RoundingMode>(99);
  Check(ApplyNumericOperation(request).status == NumericStatusCode::invalid_context, "null context validation");
  request.context.rounding = RoundingMode::half_even;
  request.right.type = NumericType::decimal;
  Check(ApplyNumericOperation(request).status == NumericStatusCode::invalid_right, "null family validation");
  request.right.type = NumericType::real128;
  request.operation = static_cast<NumericOperation>(99);
  Check(ApplyNumericOperation(request).status == NumericStatusCode::invalid_operation, "null operation validation");
  for (const auto& entry : std::vector<std::pair<NumericOperation, std::pair<std::string, std::string>>>{
      {NumericOperation::add, {"-0", "-0"}},
      {NumericOperation::multiply, {"0", "-2"}},
      {NumericOperation::divide, {"0", "-2"}}}) {
    const auto got = ApplyNumericOperation(Request(entry.first, entry.second.first, entry.second.second));
    Check(got.value.encoded == "-0", "negative zero result");
  }
  const auto cancel = ApplyNumericOperation(Request(NumericOperation::subtract, "1", "1"));
  Check(cancel.value.encoded == "0", "exact cancellation positive zero");
  for (const auto right : {"bad", "0", "0x1p16384"}) {
    const auto got = ApplyNumericOperation(Request(NumericOperation::divide, "0x1p-16494", right));
    Check(got.status != NumericStatusCode::ok && !got.subnormal &&
          got.value.encoded.empty(), "failed operation does not publish operand subnormal fact");
  }
  auto signaling = Request(NumericOperation::add, "sNaN", "0x1p-16494");
  signaling.context.allow_special_values = true;
  const auto invalid = ApplyNumericOperation(signaling);
  Check(invalid.status == NumericStatusCode::invalid_operation && !invalid.subnormal,
        "invalid operation does not publish right operand subnormal fact");
  struct Comparison { const char* left; const char* right; int expected; };
  for (const auto& entry : std::vector<Comparison>{
      {"1", "1", 0}, {"0", "-0", 0}, {"-3", "-2", -1},
      {"0x1p-16494", "0", 1},
      {"0x1.0000000000000000000000000001p0", "1", 1},
      {"Infinity", "Infinity", 0}, {"-Infinity", "Infinity", -1}}) {
    auto compare = Request(NumericOperation::compare, entry.left, entry.right);
    compare.context.allow_special_values = true;
    const auto got = ApplyNumericOperation(compare);
    Check(got.status == NumericStatusCode::ok && got.comparison == entry.expected &&
          !got.subnormal && got.value.encoded == (entry.expected == 0 ? "true" : "false"),
          "finite and special comparison");
  }
}

void State(unsigned index) {
  const auto old_emin = mpfr_get_emin(), old_emax = mpfr_get_emax();
  const auto old_precision = mpfr_get_default_prec();
  const auto old_round = mpfr_get_default_rounding_mode();
  const auto old_flags = mpfr_flags_save();
  fenv_t host;
  std::fegetenv(&host);
  mpfr_set_emin(-1000 - static_cast<long>(index));
  mpfr_set_emax(1000 + index);
  mpfr_set_default_prec(29 + index);
  mpfr_set_default_rounding_mode(MPFR_RNDU);
  mpfr_flags_restore(MPFR_FLAGS_NAN | MPFR_FLAGS_OVERFLOW, MPFR_FLAGS_ALL);
  std::fesetround(FE_DOWNWARD);
  std::feclearexcept(FE_ALL_EXCEPT);
  std::feraiseexcept(FE_DIVBYZERO);
  for (unsigned n = 0; n < 100; ++n) {
    const auto a = ApplyNumericOperation(Request(NumericOperation::add, "1", "0x1.8p-112"));
    const auto b = ApplyNumericOperation(Request(NumericOperation::add, "1", "0x1.8p-112", RoundingMode::truncate));
    Check(a.status == NumericStatusCode::ok && b.status == NumericStatusCode::ok &&
          a.value.encoded != b.value.encoded, "explicit rounding independent of ambient state");
    Check(mpfr_get_emin() == -1000 - static_cast<long>(index) &&
          mpfr_get_emax() == 1000 + index && mpfr_get_default_prec() == 29 + index &&
          mpfr_get_default_rounding_mode() == MPFR_RNDU &&
          mpfr_flags_save() == (MPFR_FLAGS_NAN | MPFR_FLAGS_OVERFLOW), "MPFR state restored");
    Check(std::fegetround() == FE_DOWNWARD &&
          std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO, "host environment restored");
  }
  mpfr_set_emin(old_emin); mpfr_set_emax(old_emax);
  mpfr_set_default_prec(old_precision); mpfr_set_default_rounding_mode(old_round);
  mpfr_flags_restore(old_flags, MPFR_FLAGS_ALL);
  std::fesetenv(&host);
  ReleaseReal128ThreadCache();
}

void AllocationFailure() {
  const auto request = Request(NumericOperation::divide, "1.2345678901234567890123456789012345", "7");
  const auto flags = mpfr_flags_save();
  const auto emin = mpfr_get_emin(), emax = mpfr_get_emax();
  unsigned faults = 0;
  bool success = false;
  for (long budget = 0; budget < 100; ++budget) {
    allocation_budget = budget;
    try {
      auto value = ApplyNumericOperation(request);
      allocation_budget = -1;
      Check(value.status == NumericStatusCode::ok, "allocation sweep success");
      success = true;
    } catch (const std::bad_alloc&) {
      allocation_budget = -1;
      ++faults;
    }
    Check(mpfr_flags_save() == flags && mpfr_get_emin() == emin &&
          mpfr_get_emax() == emax, "allocation unwind restores numeric environment");
    Check(request.left.encoded == "1.2345678901234567890123456789012345" &&
          request.right.encoded == "7", "allocation unwind preserves operands");
    if (success) break;
  }
  Check(success && faults > 2, "all C++ allocation sites swept");
  std::cout << "cpp_allocation_faults=" << faults << '\n';
}
}
int main() {
  Check(Real128BackendAvailable(), "TLS reference available");
  Check(std::string(Real128BackendName()) == "MPFR/GMP binary128 reference", "actual reference identity");
  const auto manifest = scratchbird::core::platform::DetectRuntimeCapabilities();
  const auto capability = std::find_if(manifest.capabilities.begin(), manifest.capabilities.end(),
      [](const auto& entry) { return entry.key == "numeric.real128"; });
  Check(capability != manifest.capabilities.end() &&
        capability->provider == Real128BackendName() &&
        capability->state == scratchbird::core::platform::CapabilityState::present,
        "Core capability reports linked reference");
  Arithmetic();
  DecimalOracle();
  Policy();
  AllocationFailure();
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 4; ++i) workers.emplace_back(State, i);
  for (auto& worker : workers) worker.join();
  ReleaseReal128ThreadCache();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
