// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"

#include <mpfr.h>
#include <algorithm>
#include <string>
#include <string_view>

namespace scratchbird::libraries::sbl_numeric {
namespace {
constexpr mpfr_prec_t kWorkPrecision = 115;  // binary128 plus two guard bits
constexpr mpfr_exp_t kNormalMinExponent = -16381;
constexpr mpfr_exp_t kMaximumExponent = 16384;
constexpr mpfr_exp_t kQuantumExponent = -16494;

struct Environment {
  mpfr_exp_t emin = mpfr_get_emin(), emax = mpfr_get_emax();
  mpfr_flags_t flags = mpfr_flags_save();
  Environment() {
    // Binary128 limits are applied explicitly after each operation, including
    // subnormal rounding. The wide work range avoids premature double rounding.
    mpfr_set_emin(mpfr_get_emin_min());
    mpfr_set_emax(mpfr_get_emax_max());
    mpfr_clear_flags();
  }
  ~Environment() {
    mpfr_set_emin(emin);
    mpfr_set_emax(emax);
    mpfr_flags_restore(flags, MPFR_FLAGS_ALL);
  }
};
struct Number {
  mpfr_t value;
  Number() { mpfr_init2(value, kWorkPrecision); }
  ~Number() { mpfr_clear(value); }
  Number(const Number&) = delete;
  Number& operator=(const Number&) = delete;
};
struct Digits {
  char* value;
  ~Digits() { if (value) mpfr_free_str(value); }
};

bool VersionAtLeast(const char* text, unsigned major, unsigned minor, unsigned patch) noexcept {
  if (!text) return false;
  const unsigned required[] = {major, minor, patch};
  unsigned parsed[3]{};
  for (unsigned i = 0; i < 3; ++i) {
    if (*text < '0' || *text > '9') return false;
    do {
      if (parsed[i] > 100000) return false;
      parsed[i] = parsed[i] * 10 + static_cast<unsigned>(*text++ - '0');
    } while (*text >= '0' && *text <= '9');
    if (i < 2 && *text++ != '.') return false;
  }
  for (unsigned i = 0; i < 3; ++i) {
    if (parsed[i] != required[i]) return parsed[i] > required[i];
  }
  return true;
}

bool Space(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}
bool SameAscii(std::string_view text, std::string_view expected) noexcept {
  if (text.size() != expected.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char ch = text[i];
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    if (ch != expected[i]) return false;
  }
  return true;
}
enum class Lexical { invalid, finite, infinity, quiet_nan, signaling_nan };
Lexical Classify(std::string_view text) noexcept {
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) text.remove_prefix(1);
  if (SameAscii(text, "nan")) return Lexical::quiet_nan;
  if (SameAscii(text, "snan")) return Lexical::signaling_nan;
  if (SameAscii(text, "inf") || SameAscii(text, "infinity")) return Lexical::infinity;
  bool hex = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  if (hex) text.remove_prefix(2);
  std::size_t pos = 0, digits = 0;
  bool point = false;
  for (; pos < text.size(); ++pos) {
    const char ch = text[pos];
    if (ch >= '0' && ch <= '9') { ++digits; continue; }
    if (hex && ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) { ++digits; continue; }
    if (ch == '.' && !point) { point = true; continue; }
    break;
  }
  if (!digits) return Lexical::invalid;
  if (pos == text.size()) return Lexical::finite;
  const char exponent = text[pos++];
  if (hex ? exponent != 'p' && exponent != 'P' : exponent != 'e' && exponent != 'E') return Lexical::invalid;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
  const auto begin = pos;
  for (; pos < text.size(); ++pos) if (text[pos] < '0' || text[pos] > '9') return Lexical::invalid;
  return pos == begin ? Lexical::invalid : Lexical::finite;
}

bool RoundBinary128(mpfr_ptr value, int work_inexact, RoundingMode mode, NumericResult& result) {
  if (mpfr_inf_p(value)) {
    result.overflow = result.inexact = true;
    result.status = NumericStatusCode::overflow;
    result.diagnostic_code = "NUMERIC.REAL128.OVERFLOW";
    return false;
  }
  if (mpfr_zero_p(value)) {
    result.subnormal = false;
    result.inexact |= work_inexact != 0;
    result.underflow |= work_inexact != 0;
    return true;
  }
  const bool negative = mpfr_signbit(value) != 0;
  mpfr_abs(value, value, MPFR_RNDN);
  auto exponent = mpfr_get_exp(value);
  if (exponent <= kQuantumExponent) {
    const int halfway = mpfr_cmp_ui_2exp(value, 1, kQuantumExponent - 1);
    const bool up = mode != RoundingMode::truncate &&
        (halfway > 0 || (halfway == 0 && (work_inexact != 0 || mode == RoundingMode::half_up)));
    if (up) mpfr_set_ui_2exp(value, 1, kQuantumExponent, MPFR_RNDN);
    else mpfr_set_zero(value, 1);
    if (negative) mpfr_neg(value, value, MPFR_RNDN);
    result.inexact = result.underflow = true;
    result.subnormal = up;
    return true;
  }
  const mpfr_prec_t precision = exponent < kNormalMinExponent
      ? static_cast<mpfr_prec_t>(exponent - kQuantumExponent) : 113;
  const bool halfway = work_inexact == 0 && mpfr_min_prec(value) == precision + 1;
  // Round-to-odd at p+2 retains the sticky information lost by RNDZ. Unlike a
  // second ordinary rounding, it cannot turn an inexact value into a p-bit tie.
  if (work_inexact != 0 && mpfr_min_prec(value) < kWorkPrecision) mpfr_nextabove(value);
  const auto rounding = mode == RoundingMode::truncate ? MPFR_RNDZ :
      mode == RoundingMode::half_up && halfway ? MPFR_RNDA : MPFR_RNDN;
  const bool inexact = mpfr_prec_round(value, precision, rounding) != 0 || work_inexact != 0;
  result.inexact |= inexact;
  exponent = mpfr_get_exp(value);
  if (exponent > kMaximumExponent) {
    result.overflow = result.inexact = true;
    result.status = NumericStatusCode::overflow;
    result.diagnostic_code = "NUMERIC.REAL128.OVERFLOW";
    return false;
  }
  result.subnormal = exponent < kNormalMinExponent;
  result.underflow |= result.subnormal && inexact;
  if (negative) mpfr_neg(value, value, MPFR_RNDN);
  return true;
}

bool Read(const NumericValue& input, const NumericContext& context, mpfr_ptr value,
          Lexical& kind, bool right, NumericResult& result) {
  auto text = std::string_view(input.encoded);
  while (!text.empty() && Space(text.front())) text.remove_prefix(1);
  while (!text.empty() && Space(text.back())) text.remove_suffix(1);
  kind = input.type == NumericType::real128 ? Classify(text) : Lexical::invalid;
  if (kind == Lexical::invalid || (kind != Lexical::finite && !context.allow_special_values)) {
    result.status = right ? NumericStatusCode::invalid_right : NumericStatusCode::invalid_left;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return false;
  }
  if (kind == Lexical::infinity) { mpfr_set_inf(value, text.front() == '-' ? -1 : 1); return true; }
  if (kind == Lexical::quiet_nan || kind == Lexical::signaling_nan) { mpfr_set_nan(value); return true; }
  const std::string terminated(text);
  char* end = nullptr;
  const int inexact = mpfr_strtofr(value, terminated.c_str(), &end, 0, MPFR_RNDZ);
  if (end != terminated.data() + terminated.size()) {
    result.status = right ? NumericStatusCode::invalid_right : NumericStatusCode::invalid_left;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return false;
  }
  return RoundBinary128(value, inexact, context.rounding, result);
}

std::string Render(mpfr_srcptr value) {
  if (mpfr_nan_p(value)) return "NaN";
  if (mpfr_inf_p(value)) return mpfr_signbit(value) ? "-Infinity" : "Infinity";
  if (mpfr_zero_p(value)) return mpfr_signbit(value) ? "-0" : "0";
  mpfr_exp_t exponent = 0;
  Digits owned{mpfr_get_str(nullptr, &exponent, 10, 36, value, MPFR_RNDN)};
  std::string digits(owned.value);
  std::string sign;
  if (digits.front() == '-') { sign = "-"; digits.erase(0, 1); }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  if (exponent < -3 || exponent > 36) {
    std::string out = sign + digits.front();
    if (digits.size() > 1) out += "." + digits.substr(1);
    return out + "e" + (exponent > 0 ? "+" : "") + std::to_string(exponent - 1);
  }
  if (exponent <= 0) return sign + "0." + std::string(static_cast<std::size_t>(-exponent), '0') + digits;
  const auto whole = static_cast<std::size_t>(exponent);
  if (whole >= digits.size()) return sign + digits + std::string(whole - digits.size(), '0');
  return sign + digits.substr(0, whole) + "." + digits.substr(whole);
}
}  // namespace

const char* Real128BackendName() { return "MPFR/GMP binary128 reference"; }
bool Real128BackendAvailable() noexcept {
  return mpfr_buildopt_tls_p() != 0 &&
      VersionAtLeast(mpfr_get_version(), 4, 2, 1) &&
      VersionAtLeast(gmp_version, 6, 2, 0);
}
void ReleaseReal128ThreadCache() noexcept {
  if (Real128BackendAvailable()) mpfr_free_cache2(MPFR_FREE_LOCAL_CACHE);
}

namespace detail {
NumericResult Real128ReferenceOperation(const NumericRequest& request) {
  NumericResult result;
  result.value.type = NumericType::real128;
  if (!Real128BackendAvailable()) {
    result.status = NumericStatusCode::backend_unavailable;
    result.diagnostic_code = "NUMERIC.BACKEND.UNAVAILABLE";
    return result;
  }
  if (request.context.rounding != RoundingMode::half_even && request.context.rounding != RoundingMode::half_up &&
      request.context.rounding != RoundingMode::truncate) {
    result.status = NumericStatusCode::invalid_context;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return result;
  }
  if (request.operation < NumericOperation::canonicalize || request.operation > NumericOperation::compare) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return result;
  }
  if (request.left.type != NumericType::real128 ||
      (request.operation != NumericOperation::canonicalize && request.right.type != NumericType::real128)) {
    result.status = request.left.type != NumericType::real128
        ? NumericStatusCode::invalid_left : NumericStatusCode::invalid_right;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return result;
  }
  if (request.left.is_null || (request.operation != NumericOperation::canonicalize && request.right.is_null)) {
    result.status = NumericStatusCode::null_result;
    result.value.is_null = true;
    return result;
  }
  Environment environment;
  Number left, right, output;
  Lexical left_kind, right_kind;
  if (!Read(request.left, request.context, left.value, left_kind, false, result)) return result;
  if (request.operation == NumericOperation::canonicalize) {
    result.value.encoded = left_kind == Lexical::signaling_nan ? "sNaN" : Render(left.value);
    return result;
  }
  // Subnormal describes a published numeric result, not either operand.
  result.subnormal = false;
  if (!Read(request.right, request.context, right.value, right_kind, true, result)) return result;
  result.subnormal = false;
  if (left_kind == Lexical::signaling_nan || right_kind == Lexical::signaling_nan) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return result;
  }
  if (mpfr_nan_p(left.value) || mpfr_nan_p(right.value)) {
    if (request.operation == NumericOperation::compare) {
      result.status = NumericStatusCode::unordered;
      result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    } else result.value.encoded = "NaN";
    result.subnormal = false;
    return result;
  }
  if (request.operation == NumericOperation::compare) {
    const int comparison = mpfr_cmp(left.value, right.value);
    result.comparison = comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
    result.value.encoded = comparison == 0 ? "true" : "false";
    result.subnormal = false;
    return result;
  }
  int inexact = 0;
  switch (request.operation) {
    case NumericOperation::add: inexact = mpfr_add(output.value, left.value, right.value, MPFR_RNDZ); break;
    case NumericOperation::subtract: inexact = mpfr_sub(output.value, left.value, right.value, MPFR_RNDZ); break;
    case NumericOperation::multiply: inexact = mpfr_mul(output.value, left.value, right.value, MPFR_RNDZ); break;
    case NumericOperation::divide:
      if (mpfr_zero_p(right.value) && mpfr_number_p(left.value) && !mpfr_zero_p(left.value)) {
        result.status = NumericStatusCode::divide_by_zero;
        result.divide_by_zero = true;
        result.diagnostic_code = "NUMERIC.REAL128.DIVIDE_BY_ZERO";
        return result;
      }
      inexact = mpfr_div(output.value, left.value, right.value, MPFR_RNDZ); break;
    default: break;  // Validated before reading either operand.
  }
  if (mpfr_nan_p(output.value)) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return result;
  }
  if (mpfr_inf_p(output.value) && (mpfr_inf_p(left.value) || mpfr_inf_p(right.value))) {
    result.value.encoded = Render(output.value);
    result.subnormal = false;
    return result;
  }
  if (RoundBinary128(output.value, inexact, request.context.rounding, result)) result.value.encoded = Render(output.value);
  return result;
}
}  // namespace detail
}  // namespace scratchbird::libraries::sbl_numeric
