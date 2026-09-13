// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include <mpfr.h>
#include <cfenv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {
long allocation_budget = -1;
unsigned checks = 0, failures = 0;
void Check(bool good, const std::string& message) {
  ++checks;
  if (!good && failures++ < 20) std::cerr << "FAIL " << message << '\n';
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
bool Good(const Real128BinaryResult& result) {
  return result.numeric.status == NumericStatusCode::ok && result.bytes.has_value();
}
Real128Bytes Bits(unsigned exponent, bool negative = false, std::uint8_t low = 0) {
  Real128Bytes bytes{};
  bytes[0] = low;
  bytes[14] = static_cast<std::uint8_t>(exponent);
  bytes[15] = static_cast<std::uint8_t>((exponent >> 8) | (negative ? 0x80 : 0));
  return bytes;
}
Real128BinaryResult Operation(NumericOperation op, const Real128Bytes& a,
                              const Real128Bytes& b, RoundingMode mode = RoundingMode::half_even,
                              bool specials = false) {
  Real128BinaryRequest request;
  request.operation = op; request.left = a; request.right = b;
  request.context.rounding = mode; request.context.allow_special_values = specials;
  return ApplyReal128BinaryOperation(request);
}

void GoldenValues() {
  struct Golden { const char* text; Real128Bytes bytes; };
  auto maximum = Bits(0x7ffe);
  auto largest_subnormal = Bits(0);
  for (unsigned i = 0; i < 14; ++i) maximum[i] = largest_subnormal[i] = 0xff;
  for (const auto& entry : std::vector<Golden>{
      {"0", Bits(0)}, {"-0", Bits(0, true)},
      {"1", Bits(0x3fff)}, {"-1", Bits(0x3fff, true)},
      {"2", Bits(0x4000)}, {"0x1p-16382", Bits(1)},
      {"0x1p-16494", Bits(0, false, 1)}, {"-0x1p-16494", Bits(0, true, 1)},
      {"0x1.ffffffffffffffffffffffffffffp16383", maximum},
      {"0x0.ffffffffffffffffffffffffffffp-16382", largest_subnormal}}) {
    const auto encoded = EncodeReal128LittleEndian(entry.text);
    Check(Good(encoded) && *encoded.bytes == entry.bytes, std::string("golden encode ") + entry.text);
    Check(encoded.numeric.value.encoded.empty() && !encoded.numeric.inexact, "binary output without text or extra rounding");
    const auto decoded = DecodeReal128LittleEndian(entry.bytes.data(), entry.bytes.size(), {}, true);
    Check(Good(decoded) && *decoded.bytes == entry.bytes, "golden decode bits");
    const auto again = EncodeReal128LittleEndian(decoded.numeric.value.encoded);
    Check(Good(again) && *again.bytes == entry.bytes, "golden text boundary round trip");
  }
}

void FiniteExponentCorpus() {
  mpfr_t expected, displayed;
  mpfr_init2(expected, 113);
  mpfr_init2(displayed, 113);
  std::uint64_t state = 0x128b17e5a9ULL;
  for (unsigned exponent = 0; exponent < 0x7fff; ++exponent) {
    for (bool negative : {false, true}) {
      auto bytes = Bits(exponent, negative);
      for (unsigned i = 0; i < 14; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        bytes[i] = static_cast<std::uint8_t>(state >> 40);
      }
      const auto original = bytes;
      // Independent field-to-value oracle: use the specified hexadecimal
      // significand and unbiased exponent, not either production codec.
      std::string exact = negative ? "-0x" : "0x";
      exact += exponent ? "1." : "0.";
      constexpr char hex[] = "0123456789abcdef";
      for (unsigned i = 14; i > 0; --i) {
        exact += hex[bytes[i - 1] >> 4];
        exact += hex[bytes[i - 1] & 15];
      }
      exact += "p" + std::to_string(exponent ? static_cast<int>(exponent) - 16383 : -16382);
      Check(mpfr_set_str(expected, exact.c_str(), 0, MPFR_RNDN) == 0,
            "independent hexadecimal oracle parses");
      const auto decoded = DecodeReal128LittleEndian(bytes.data(), bytes.size(), {}, true);
      Check(Good(decoded) && *decoded.bytes == bytes, "all-exponent decode identity");
      Check(!decoded.numeric.inexact && !decoded.numeric.underflow &&
            decoded.numeric.subnormal == (exponent == 0), "exact binary classification");
      Check(mpfr_set_str(displayed, decoded.numeric.value.encoded.c_str(), 10, MPFR_RNDN) == 0 &&
            mpfr_equal_p(displayed, expected), "decoded value matches independent field oracle");
      const auto from_exact = EncodeReal128LittleEndian(exact);
      Check(Good(from_exact) && *from_exact.bytes == bytes && !from_exact.numeric.inexact,
            "encoding exact independent field oracle");
      const auto encoded = EncodeReal128LittleEndian(decoded.numeric.value.encoded);
      Check(Good(encoded) && *encoded.bytes == bytes, "all-exponent encode after display");
      Real128BinaryRequest canonical;
      canonical.left = bytes;
      const auto retained = ApplyReal128BinaryOperation(canonical);
      Check(Good(retained) && *retained.bytes == bytes, "binary canonicalization preserves bits");
      if ((exponent % 257) == 0) {
        // A binary operand is already rounded. Rendering/reparsing it in
        // truncate mode can change it, and is not an allowed implementation.
        for (auto mode : {RoundingMode::half_even, RoundingMode::half_up, RoundingMode::truncate}) {
          const auto identity = Operation(NumericOperation::add, bytes, Bits(0), mode);
          Check(Good(identity) && *identity.bytes == bytes && !identity.numeric.inexact,
                "binary arithmetic has no text conversion round");
        }
      }
      Check(bytes == original, "codec does not mutate source");
    }
  }
  mpfr_clear(expected);
  mpfr_clear(displayed);
}

void Arithmetic() {
  const auto one = Bits(0x3fff);
  const auto half_ulp = Bits(16383 - 113);
  auto one_and_half_ulp = Bits(16383 - 112);
  one_and_half_ulp[13] = 0x80;
  for (auto mode : {RoundingMode::half_even, RoundingMode::half_up, RoundingMode::truncate}) {
    const auto tie = Operation(NumericOperation::add, one, half_ulp, mode);
    Check(Good(tie) && *tie.bytes == Bits(0x3fff, false, mode == RoundingMode::half_up ? 1 : 0) &&
          tie.numeric.inexact, "binary tie rounding");
    const auto larger = Operation(NumericOperation::add, one, one_and_half_ulp, mode);
    Check(Good(larger) && *larger.bytes == Bits(0x3fff, false, mode == RoundingMode::truncate ? 1 : 2),
          "binary inexact rounding");
    const auto tiny = Operation(NumericOperation::multiply, Bits(0, false, 1), Bits(0x3ffe), mode);
    Check(Good(tiny) && *tiny.bytes == Bits(0, false, mode == RoundingMode::half_up ? 1 : 0) &&
          tiny.numeric.inexact && tiny.numeric.underflow, "binary subnormal tie");
  }
  const auto negative_zero = Operation(NumericOperation::multiply, Bits(0), Bits(0x3fff, true));
  Check(Good(negative_zero) && *negative_zero.bytes == Bits(0, true), "negative zero product");
  const auto difference = Operation(NumericOperation::subtract, Bits(0x3fff, false, 1), one);
  Check(Good(difference) && *difference.bytes == Bits(16383 - 112) &&
        !difference.numeric.inexact, "binary subtraction retains low significand bit");
  const auto quotient = Operation(NumericOperation::divide, Bits(0x4000), Bits(0x4001));
  Check(Good(quotient) && *quotient.bytes == Bits(0x3ffe), "binary exact division");
  const auto comparison = Operation(NumericOperation::compare, one, Bits(0x3fff, false, 1));
  Check(comparison.numeric.status == NumericStatusCode::ok && comparison.numeric.comparison == -1 &&
        !comparison.bytes && comparison.numeric.value.encoded.empty(), "comparison is an outcome not numeric zero");
  auto maximum = Bits(0x7ffe);
  for (unsigned i = 0; i < 14; ++i) maximum[i] = 0xff;
  const auto overflow = Operation(NumericOperation::multiply, maximum, Bits(0x4000));
  Check(overflow.numeric.status == NumericStatusCode::overflow && overflow.numeric.overflow &&
        !overflow.bytes, "overflow has no binary value");
  for (const auto left : {Bits(0), one}) {
    const auto error = Operation(NumericOperation::divide, left, Bits(0));
    Check(!error.bytes && error.numeric.status != NumericStatusCode::ok, "invalid division has no value");
  }
}

void Specials() {
  NumericContext context;
  context.allow_special_values = true;
  auto quiet = Bits(0x7fff); quiet[13] = 0x80;
  auto signaling = Bits(0x7fff, false, 1);
  for (const auto& entry : std::vector<std::pair<std::string, Real128Bytes>>{
      {"Infinity", Bits(0x7fff)}, {"-Infinity", Bits(0x7fff, true)},
      {"NaN", quiet}, {"-NaN", quiet}, {"sNaN", signaling}, {"-sNaN", signaling}}) {
    const auto encoded = EncodeReal128LittleEndian(entry.first, context);
    Check(Good(encoded) && *encoded.bytes == entry.second, "special text encoding");
    Check(!EncodeReal128LittleEndian(entry.first).bytes, "special text policy");
  }
  for (unsigned bit = 0; bit < 112; ++bit) {
    for (bool negative : {false, true}) {
      for (bool is_quiet : {false, true}) {
        auto nan = Bits(0x7fff, negative);
        nan[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
        if (is_quiet) nan[13] |= 0x80;
        const bool signaling_class = (nan[13] & 0x80) == 0;
        const auto decoded = DecodeReal128LittleEndian(nan.data(), nan.size(), context, true);
        Check(Good(decoded) && *decoded.bytes == nan &&
              decoded.numeric.value.encoded == (signaling_class ? "sNaN" : "NaN"),
              "NaN sign and payload survive decode with display");
        Real128BinaryRequest canonical;
        canonical.left = nan; canonical.context = context;
        const auto retained = ApplyReal128BinaryOperation(canonical);
        Check(Good(retained) && *retained.bytes == nan, "NaN binary canonicalization is lossless");
        Check(!DecodeReal128LittleEndian(nan.data(), nan.size()).bytes, "NaN binary policy");
        const auto operation = Operation(NumericOperation::add, nan, Bits(0x3fff), RoundingMode::half_even, true);
        if (signaling_class)
          Check(!operation.bytes && operation.numeric.invalid, "signaling arithmetic rejected");
        else
          Check(Good(operation) && *operation.bytes == nan, "quiet payload arithmetic propagation");
      }
    }
  }
  auto second = quiet; second[0] = 0x35; second[15] |= 0x80;
  auto propagated = Operation(NumericOperation::add, quiet, second, RoundingMode::half_even, true);
  Check(Good(propagated) && *propagated.bytes == quiet, "left quiet NaN wins");
  propagated = Operation(NumericOperation::add, Bits(0x3fff), second, RoundingMode::half_even, true);
  Check(Good(propagated) && *propagated.bytes == second, "right quiet NaN preserved");
  const auto unordered = Operation(NumericOperation::compare, quiet, Bits(0), RoundingMode::half_even, true);
  Check(unordered.numeric.status == NumericStatusCode::unordered && !unordered.bytes &&
        !unordered.numeric.invalid, "quiet NaN comparison unordered");
}

void ShapeAndState() {
  Real128Bytes bytes{};
  for (std::size_t size = 0; size < 34; ++size) {
    if (size == 16) continue;
    const auto got = DecodeReal128LittleEndian(bytes.data(), size);
    Check(!got.bytes && got.numeric.invalid &&
          got.numeric.diagnostic_code == "NUMERIC.ENCODING.NONCANONICAL", "wrong payload length rejected");
  }
  Check(!DecodeReal128LittleEndian(nullptr, 16).bytes, "null data pointer rejected");
  Check(!DecodeReal128LittleEndian(bytes.data(), std::numeric_limits<std::size_t>::max()).bytes,
        "length overflow rejected before reading");
  Real128BinaryRequest request;
  Check(ApplyReal128BinaryOperation(request).numeric.status == NumericStatusCode::invalid_left,
        "missing left is not implicit zero");
  request.left = bytes; request.operation = NumericOperation::add;
  Check(ApplyReal128BinaryOperation(request).numeric.status == NumericStatusCode::invalid_right,
        "missing right is not implicit zero");
  request.operation = static_cast<NumericOperation>(99);
  Check(ApplyReal128BinaryOperation(request).numeric.status == NumericStatusCode::invalid_operation,
        "unknown binary operation rejected");
  request.operation = NumericOperation::canonicalize;
  request.context.rounding = static_cast<RoundingMode>(99);
  Check(ApplyReal128BinaryOperation(request).numeric.status == NumericStatusCode::invalid_context,
        "unknown binary context rejected");
  const auto emin = mpfr_get_emin(), emax = mpfr_get_emax();
  const auto precision = mpfr_get_default_prec();
  const auto mode = mpfr_get_default_rounding_mode();
  const auto flags = mpfr_flags_save();
  fenv_t host;
  std::fegetenv(&host);
  mpfr_set_emin(-100); mpfr_set_emax(100);
  mpfr_set_default_prec(17); mpfr_set_default_rounding_mode(MPFR_RNDU);
  mpfr_flags_restore(MPFR_FLAGS_OVERFLOW | MPFR_FLAGS_NAN, MPFR_FLAGS_ALL);
  std::fesetround(FE_UPWARD); std::feclearexcept(FE_ALL_EXCEPT); std::feraiseexcept(FE_DIVBYZERO);
  const auto decoded = DecodeReal128LittleEndian(Bits(0x7ffe).data(), 16, {}, true);
  const auto encoded = EncodeReal128LittleEndian(decoded.numeric.value.encoded);
  const auto value = Operation(NumericOperation::divide, Bits(0x3fff), Bits(0x4000));
  Check(Good(decoded) && Good(encoded) && Good(value), "binary codecs ignore ambient range and precision");
  Check(mpfr_get_emin() == -100 && mpfr_get_emax() == 100 &&
        mpfr_get_default_prec() == 17 && mpfr_get_default_rounding_mode() == MPFR_RNDU &&
        mpfr_flags_save() == (MPFR_FLAGS_OVERFLOW | MPFR_FLAGS_NAN), "binary codecs restore MPFR state");
  Check(std::fegetround() == FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
        "binary codecs preserve host environment");
  mpfr_set_emin(emin); mpfr_set_emax(emax); mpfr_set_default_prec(precision);
  mpfr_set_default_rounding_mode(mode); mpfr_flags_restore(flags, MPFR_FLAGS_ALL);
  std::fesetenv(&host);
}

void AllocationFailure() {
  const std::string text = "1.23456789012345678901234567890123456";
  const auto encoded = EncodeReal128LittleEndian(text);
  Check(Good(encoded), "allocation fixture encodes");
  if (!Good(encoded)) return;
  const auto bytes = *encoded.bytes;
  unsigned faults = 0;
  for (bool render : {false, true}) {
    bool success = false;
    for (long budget = 0; budget < 80; ++budget) {
      const auto flags = mpfr_flags_save();
      allocation_budget = budget;
      try {
        auto result = render ? DecodeReal128LittleEndian(bytes.data(), bytes.size(), {}, true)
                             : EncodeReal128LittleEndian(text);
        allocation_budget = -1;
        Check(Good(result) && *result.bytes == bytes, "allocation sweep value");
        success = true;
      } catch (const std::bad_alloc&) {
        allocation_budget = -1;
        ++faults;
      }
      Check(mpfr_flags_save() == flags, "allocation unwind restores backend flags");
      if (success) break;
    }
    Check(success, "allocation sweep completes");
  }
  Check(faults >= 3, "text-boundary allocation faults exercised");
  std::cout << "cpp_allocation_faults=" << faults << '\n';
}
}
int main() {
  GoldenValues(); FiniteExponentCorpus(); Arithmetic(); Specials(); ShapeAndState(); AllocationFailure();
  ReleaseReal128ThreadCache();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
