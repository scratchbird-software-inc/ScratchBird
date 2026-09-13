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
struct Integer {
  mpz_t value;
  Integer() { mpz_init(value); }
  ~Integer() { mpz_clear(value); }
  Integer(const Integer&) = delete;
  Integer& operator=(const Integer&) = delete;
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
bool ValidateContext(NumericOperation operation, const NumericContext& context, NumericResult& result) {
  result.value.type = NumericType::real128;
  if (!Real128BackendAvailable()) {
    result.status = NumericStatusCode::backend_unavailable;
    result.diagnostic_code = "NUMERIC.BACKEND.UNAVAILABLE";
    return false;
  }
  if (context.rounding != RoundingMode::half_even && context.rounding != RoundingMode::half_up &&
      context.rounding != RoundingMode::truncate) {
    result.status = NumericStatusCode::invalid_context;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return false;
  }
  if (operation < NumericOperation::canonicalize || operation > NumericOperation::compare) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return false;
  }
  return true;
}

bool ReadBits(const Real128Bytes& bytes, const NumericContext& context,
              mpfr_ptr value, Lexical& kind, bool right, NumericResult& result) {
  const unsigned exponent = bytes[14] | (static_cast<unsigned>(bytes[15] & 0x7f) << 8);
  const int sign = (bytes[15] & 0x80) ? -1 : 1;
  Integer fraction;
  mpz_import(fraction.value, 14, -1, 1, 0, 0, bytes.data());
  const bool zero_fraction = mpz_sgn(fraction.value) == 0;
  kind = exponent != 0x7fff ? Lexical::finite : zero_fraction ? Lexical::infinity :
      (bytes[13] & 0x80) ? Lexical::quiet_nan : Lexical::signaling_nan;
  if (kind != Lexical::finite && !context.allow_special_values) {
    result.status = right ? NumericStatusCode::invalid_right : NumericStatusCode::invalid_left;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return false;
  }
  if (kind == Lexical::infinity) { mpfr_set_inf(value, sign); return true; }
  if (kind == Lexical::quiet_nan || kind == Lexical::signaling_nan) {
    mpfr_set_nan(value);
    return true;
  }
  result.subnormal = exponent == 0 && !zero_fraction;
  if (exponent == 0 && zero_fraction) { mpfr_set_zero(value, sign); return true; }
  if (exponent != 0) mpz_setbit(fraction.value, 112);
  mpfr_set_z(value, fraction.value, MPFR_RNDN);
  mpfr_mul_2si(value, value, exponent ? static_cast<long>(exponent) - 16383 - 112 : -16494,
              MPFR_RNDN);
  if (sign < 0) mpfr_neg(value, value, MPFR_RNDN);
  return true;
}

Real128Bytes WriteBits(mpfr_srcptr value, bool signaling_nan = false) {
  Real128Bytes bytes{};
  const auto sign = mpfr_signbit(value) ? 0x80u : 0u;
  if (mpfr_nan_p(value)) {
    bytes[14] = 0xff; bytes[15] = 0x7f;
    if (signaling_nan) bytes[0] = 1;
    else bytes[13] = 0x80;
    return bytes;
  }
  if (mpfr_inf_p(value)) {
    bytes[14] = 0xff; bytes[15] = static_cast<std::uint8_t>(0x7f | sign);
    return bytes;
  }
  if (mpfr_zero_p(value)) { bytes[15] = static_cast<std::uint8_t>(sign); return bytes; }
  const auto exponent = mpfr_get_exp(value) - 1;
  const auto field = exponent < -16382 ? 0u : static_cast<unsigned>(exponent + 16383);
  const auto quantum = field ? exponent - 112 : kQuantumExponent;
  Integer fraction;
  const auto source_exponent = mpfr_get_z_2exp(fraction.value, value);
  mpz_abs(fraction.value, fraction.value);
  if (source_exponent < quantum)
    mpz_tdiv_q_2exp(fraction.value, fraction.value, static_cast<mp_bitcnt_t>(quantum - source_exponent));
  else
    mpz_mul_2exp(fraction.value, fraction.value, static_cast<mp_bitcnt_t>(source_exponent - quantum));
  if (field) mpz_clrbit(fraction.value, 112);
  // The input is already rounded to binary128. The fraction is at most112
  // bits; exporting one-byte words is independent of host limb layout.
  mpz_export(bytes.data(), nullptr, -1, 1, 0, 0, fraction.value);
  bytes[14] = static_cast<std::uint8_t>(field);
  bytes[15] = static_cast<std::uint8_t>((field >> 8) | sign);
  return bytes;
}

bool Calculate(NumericOperation operation, const NumericContext& context,
               mpfr_srcptr left, Lexical left_kind, mpfr_srcptr right, Lexical right_kind,
               mpfr_ptr output, NumericResult& result) {
  result.subnormal = false;
  if (left_kind == Lexical::signaling_nan || right_kind == Lexical::signaling_nan) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return false;
  }
  if (mpfr_nan_p(left) || mpfr_nan_p(right)) {
    if (operation == NumericOperation::compare) {
      result.status = NumericStatusCode::unordered;
      result.diagnostic_code = "NUMERIC.REAL128.INVALID";
      return false;
    }
    mpfr_set_nan(output);
    return true;
  }
  if (operation == NumericOperation::compare) {
    const int comparison = mpfr_cmp(left, right);
    result.comparison = comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
    return true;
  }
  int inexact = 0;
  switch (operation) {
    case NumericOperation::add: inexact = mpfr_add(output, left, right, MPFR_RNDZ); break;
    case NumericOperation::subtract: inexact = mpfr_sub(output, left, right, MPFR_RNDZ); break;
    case NumericOperation::multiply: inexact = mpfr_mul(output, left, right, MPFR_RNDZ); break;
    case NumericOperation::divide:
      if (mpfr_zero_p(right) && mpfr_number_p(left) && !mpfr_zero_p(left)) {
        result.status = NumericStatusCode::divide_by_zero;
        result.divide_by_zero = true;
        result.diagnostic_code = "NUMERIC.REAL128.DIVIDE_BY_ZERO";
        return false;
      }
      inexact = mpfr_div(output, left, right, MPFR_RNDZ); break;
    default:
      result.status = NumericStatusCode::invalid_operation;
      result.invalid = true;
      result.diagnostic_code = "NUMERIC.REAL128.INVALID";
      return false;
  }
  if (mpfr_nan_p(output)) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return false;
  }
  if (mpfr_inf_p(output) && (mpfr_inf_p(left) || mpfr_inf_p(right))) return true;
  return RoundBinary128(output, inexact, context.rounding, result);
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
  if (!ValidateContext(request.operation, request.context, result)) return result;
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
  result.subnormal = false;
  if (!Read(request.right, request.context, right.value, right_kind, true, result)) return result;
  if (Calculate(request.operation, request.context, left.value, left_kind,
                right.value, right_kind, output.value, result)) {
    result.value.encoded = request.operation == NumericOperation::compare
        ? (result.comparison == 0 ? "true" : "false") : Render(output.value);
  }
  return result;
}
}  // namespace detail

Real128BinaryResult EncodeReal128LittleEndian(std::string_view text, const NumericContext& context) {
  Real128BinaryResult result;
  if (!ValidateContext(NumericOperation::canonicalize, context, result.numeric)) return result;
  Environment environment;
  Number value;
  Lexical kind;
  const NumericValue input{NumericType::real128, std::string(text), false};
  if (Read(input, context, value.value, kind, false, result.numeric))
    result.bytes = WriteBits(value.value, kind == Lexical::signaling_nan);
  return result;
}

Real128BinaryResult DecodeReal128LittleEndian(
    const std::uint8_t* bytes, std::size_t size, const NumericContext& context,
    bool render_canonical_text) {
  Real128BinaryResult result;
  if (!ValidateContext(NumericOperation::canonicalize, context, result.numeric)) return result;
  if (!bytes || size != Real128Bytes{}.size()) {
    result.numeric.status = NumericStatusCode::invalid_left;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = "NUMERIC.ENCODING.NONCANONICAL";
    return result;
  }
  Real128Bytes input;
  std::copy_n(bytes, input.size(), input.begin());
  Environment environment;
  Number value;
  Lexical kind;
  if (!ReadBits(input, context, value.value, kind, false, result.numeric)) return result;
  if (render_canonical_text)
    result.numeric.value.encoded = kind == Lexical::signaling_nan ? "sNaN" : Render(value.value);
  result.bytes = input;
  return result;
}

Real128BinaryResult ApplyReal128BinaryOperation(const Real128BinaryRequest& request) {
  Real128BinaryResult result;
  if (!ValidateContext(request.operation, request.context, result.numeric)) return result;
  if (!request.left || (request.operation != NumericOperation::canonicalize && !request.right)) {
    result.numeric.status = !request.left ? NumericStatusCode::invalid_left : NumericStatusCode::invalid_right;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = "NUMERIC.REAL128.INVALID";
    return result;
  }
  Environment environment;
  Number left, right, output;
  Lexical left_kind, right_kind;
  if (!ReadBits(*request.left, request.context, left.value, left_kind, false, result.numeric)) return result;
  if (request.operation == NumericOperation::canonicalize) {
    result.bytes = request.left;
    return result;
  }
  result.numeric.subnormal = false;
  if (!ReadBits(*request.right, request.context, right.value, right_kind, true, result.numeric)) return result;
  if (!Calculate(request.operation, request.context, left.value, left_kind,
                 right.value, right_kind, output.value, result.numeric)) return result;
  if (request.operation == NumericOperation::compare) return result;
  if (mpfr_nan_p(output.value)) {
    // MPFR does not carry interchange NaN payloads. Keep the admitted first
    // quiet-NaN operand explicitly; signaling operands were rejected above.
    result.bytes = left_kind == Lexical::quiet_nan ? request.left : request.right;
  } else result.bytes = WriteBits(output.value);
  return result;
}
}  // namespace scratchbird::libraries::sbl_numeric
