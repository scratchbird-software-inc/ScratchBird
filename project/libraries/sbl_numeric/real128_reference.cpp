// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"

#include <mpfr.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>

namespace scratchbird::libraries::sbl_numeric {
namespace {
template<unsigned Bits>
struct BinaryRealReference {
static_assert(Bits == 64 || Bits == 128);
using Bytes = std::conditional_t<Bits == 128, Real128Bytes, Real64Bytes>;
using BinaryResult = std::conditional_t<Bits == 128, Real128BinaryResult, Real64BinaryResult>;
using BinaryRequest = std::conditional_t<Bits == 128, Real128BinaryRequest, Real64BinaryRequest>;
using TotalOrderKeyResult = std::conditional_t<Bits == 128, Real128TotalOrderKeyResult, Real64TotalOrderKeyResult>;
static constexpr auto kType = Bits == 128 ? NumericType::real128 : NumericType::real64;
static constexpr unsigned kFractionBits = Bits == 128 ? 112 : 52;
static constexpr unsigned kExponentBits = Bits == 128 ? 15 : 11;
static constexpr unsigned kBias = (1u << (kExponentBits - 1)) - 1;
static constexpr unsigned kSpecialExponent = (1u << kExponentBits) - 1;
static constexpr mpfr_prec_t kPrecision = kFractionBits + 1;
static constexpr mpfr_prec_t kWorkPrecision = kPrecision + 2;
static constexpr mpfr_exp_t kNormalMinExponent = 2 - static_cast<mpfr_exp_t>(kBias);
static constexpr mpfr_exp_t kMaximumExponent = kBias + 1;
static constexpr mpfr_exp_t kQuantumExponent = 1 - static_cast<mpfr_exp_t>(kBias) - kFractionBits;
static constexpr unsigned kRenderDigits = Bits == 128 ? 36 : 17;
static constexpr auto kInvalid = Bits == 128 ? "NUMERIC.REAL128.INVALID" : "NUMERIC.REAL64.INVALID";
static constexpr auto kOverflow = Bits == 128 ? "NUMERIC.REAL128.OVERFLOW" : "NUMERIC.REAL64.OVERFLOW";
static constexpr auto kDivideByZero = Bits == 128 ? "NUMERIC.REAL128.DIVIDE_BY_ZERO" : "NUMERIC.REAL64.DIVIDE_BY_ZERO";


struct Environment {
  mpfr_exp_t emin = mpfr_get_emin(), emax = mpfr_get_emax();
  mpfr_flags_t flags = mpfr_flags_save();
  Environment() {
    // Format limits are applied explicitly after each operation, including
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

static bool VersionAtLeast(const char* text, unsigned major, unsigned minor, unsigned patch) noexcept {
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

static bool Space(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}
static bool SameAscii(std::string_view text, std::string_view expected) noexcept {
  if (text.size() != expected.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char ch = text[i];
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    if (ch != expected[i]) return false;
  }
  return true;
}
enum class Lexical { invalid, finite, infinity, quiet_nan, signaling_nan };
static Lexical Classify(std::string_view text) noexcept {
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

static bool RoundToFormat(mpfr_ptr value, int work_inexact, RoundingMode mode, NumericResult& result) {
  if (mpfr_inf_p(value)) {
    result.overflow = result.inexact = true;
    result.status = NumericStatusCode::overflow;
    result.diagnostic_code = kOverflow;
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
      ? static_cast<mpfr_prec_t>(exponent - kQuantumExponent) : kPrecision;
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
    result.diagnostic_code = kOverflow;
    return false;
  }
  result.subnormal = exponent < kNormalMinExponent;
  result.underflow |= result.subnormal && inexact;
  if (negative) mpfr_neg(value, value, MPFR_RNDN);
  return true;
}

static bool Read(const NumericValue& input, const NumericContext& context, mpfr_ptr value,
          Lexical& kind, bool right, NumericResult& result) {
  auto text = std::string_view(input.encoded);
  while (!text.empty() && Space(text.front())) text.remove_prefix(1);
  while (!text.empty() && Space(text.back())) text.remove_suffix(1);
  kind = input.type == kType ? Classify(text) : Lexical::invalid;
  if (kind == Lexical::invalid || (kind != Lexical::finite && !context.allow_special_values)) {
    result.status = right ? NumericStatusCode::invalid_right : NumericStatusCode::invalid_left;
    result.invalid = true;
    result.diagnostic_code = kInvalid;
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
    result.diagnostic_code = kInvalid;
    return false;
  }
  return RoundToFormat(value, inexact, context.rounding, result);
}

static std::string Render(mpfr_srcptr value) {
  if (mpfr_nan_p(value)) return "NaN";
  if (mpfr_inf_p(value)) return mpfr_signbit(value) ? "-Infinity" : "Infinity";
  if (mpfr_zero_p(value)) return mpfr_signbit(value) ? "-0" : "0";
  mpfr_exp_t exponent = 0;
  Digits owned{mpfr_get_str(nullptr, &exponent, 10, kRenderDigits, value, MPFR_RNDN)};
  std::string digits(owned.value);
  std::string sign;
  if (digits.front() == '-') { sign = "-"; digits.erase(0, 1); }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  if (exponent < -3 || exponent > kRenderDigits) {
    std::string out = sign + digits.front();
    if (digits.size() > 1) out += "." + digits.substr(1);
    return out + "e" + (exponent > 0 ? "+" : "") + std::to_string(exponent - 1);
  }
  if (exponent <= 0) return sign + "0." + std::string(static_cast<std::size_t>(-exponent), '0') + digits;
  const auto whole = static_cast<std::size_t>(exponent);
  if (whole >= digits.size()) return sign + digits + std::string(whole - digits.size(), '0');
  return sign + digits.substr(0, whole) + "." + digits.substr(whole);
}
static bool ValidateContext(NumericOperation operation, const NumericContext& context, NumericResult& result) {
  result.value.type = kType;
  if (!Real128BackendAvailable()) {
    result.status = NumericStatusCode::backend_unavailable;
    result.diagnostic_code = "NUMERIC.BACKEND.UNAVAILABLE";
    return false;
  }
  if (context.rounding != RoundingMode::half_even && context.rounding != RoundingMode::half_up &&
      context.rounding != RoundingMode::truncate) {
    result.status = NumericStatusCode::invalid_context;
    result.invalid = true;
    result.diagnostic_code = kInvalid;
    return false;
  }
  if (operation < NumericOperation::canonicalize || operation > NumericOperation::compare) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = kInvalid;
    return false;
  }
  return true;
}

static bool ReadBits(const Bytes& bytes, const NumericContext& context,
              mpfr_ptr value, Lexical& kind, bool right, NumericResult& result) {
  unsigned exponent = 0;
  for (unsigned bit = 0; bit < kExponentBits; ++bit)
    exponent |= ((bytes[(kFractionBits + bit) / 8] >> ((kFractionBits + bit) % 8)) & 1u) << bit;
  const int sign = (bytes.back() & 0x80) ? -1 : 1;
  Integer fraction;
  mpz_import(fraction.value, bytes.size(), -1, 1, 0, 0, bytes.data());
  mpz_fdiv_r_2exp(fraction.value, fraction.value, kFractionBits);
  const bool zero_fraction = mpz_sgn(fraction.value) == 0;
  kind = exponent != kSpecialExponent ? Lexical::finite : zero_fraction ? Lexical::infinity :
      mpz_tstbit(fraction.value, kFractionBits - 1) ? Lexical::quiet_nan : Lexical::signaling_nan;
  if (kind != Lexical::finite && !context.allow_special_values) {
    result.status = right ? NumericStatusCode::invalid_right : NumericStatusCode::invalid_left;
    result.invalid = true;
    result.diagnostic_code = kInvalid;
    return false;
  }
  if (kind == Lexical::infinity) { mpfr_set_inf(value, sign); return true; }
  if (kind == Lexical::quiet_nan || kind == Lexical::signaling_nan) {
    mpfr_set_nan(value); return true;
  }
  result.subnormal = exponent == 0 && !zero_fraction;
  if (exponent == 0 && zero_fraction) { mpfr_set_zero(value, sign); return true; }
  if (exponent != 0) mpz_setbit(fraction.value, kFractionBits);
  mpfr_set_z(value, fraction.value, MPFR_RNDN);
  mpfr_mul_2si(value, value, exponent ? static_cast<long>(exponent) - kBias - kFractionBits : kQuantumExponent, MPFR_RNDN);
  if (sign < 0) mpfr_neg(value, value, MPFR_RNDN);
  return true;
}

static Bytes WriteBits(mpfr_srcptr value, bool signaling_nan = false) {
  Bytes bytes{};
  Integer fraction;
  unsigned field = 0;
  if (mpfr_nan_p(value)) {
    field = kSpecialExponent;
    mpz_setbit(fraction.value, signaling_nan ? 0 : kFractionBits - 1);
  } else if (mpfr_inf_p(value)) {
    field = kSpecialExponent;
  } else if (!mpfr_zero_p(value)) {
    const auto exponent = mpfr_get_exp(value) - 1;
    field = exponent < kNormalMinExponent - 1 ? 0u : static_cast<unsigned>(exponent + kBias);
    const auto quantum = field ? exponent - kFractionBits : kQuantumExponent;
    const auto source_exponent = mpfr_get_z_2exp(fraction.value, value);
    mpz_abs(fraction.value, fraction.value);
    if (source_exponent < quantum)
      mpz_tdiv_q_2exp(fraction.value, fraction.value, static_cast<mp_bitcnt_t>(quantum - source_exponent));
    else
      mpz_mul_2exp(fraction.value, fraction.value, static_cast<mp_bitcnt_t>(source_exponent - quantum));
    if (field) mpz_clrbit(fraction.value, kFractionBits);
  }
  // Inputs have already been rounded exactly once to the selected format.
  // Export byte words, independent of host limb layout and endianness.
  mpz_export(bytes.data(), nullptr, -1, 1, 0, 0, fraction.value);
  for (unsigned bit = 0; bit < kExponentBits; ++bit)
    bytes[(kFractionBits + bit) / 8] |= static_cast<std::uint8_t>(((field >> bit) & 1u) << ((kFractionBits + bit) % 8));
  if (!mpfr_nan_p(value) && mpfr_signbit(value)) bytes.back() |= 0x80;
  return bytes;
}

static bool Calculate(NumericOperation operation, const NumericContext& context,
               mpfr_srcptr left, Lexical left_kind, mpfr_srcptr right, Lexical right_kind,
               mpfr_ptr output, NumericResult& result) {
  result.subnormal = false;
  if (left_kind == Lexical::signaling_nan || right_kind == Lexical::signaling_nan) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = kInvalid;
    return false;
  }
  if (mpfr_nan_p(left) || mpfr_nan_p(right)) {
    if (operation == NumericOperation::compare) {
      result.status = NumericStatusCode::unordered;
      result.diagnostic_code = kInvalid;
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
        result.diagnostic_code = kDivideByZero;
        return false;
      }
      inexact = mpfr_div(output, left, right, MPFR_RNDZ); break;
    default:
      result.status = NumericStatusCode::invalid_operation;
      result.invalid = true;
      result.diagnostic_code = kInvalid;
      return false;
  }
  if (mpfr_nan_p(output)) {
    result.status = NumericStatusCode::invalid_operation;
    result.invalid = true;
    result.diagnostic_code = kInvalid;
    return false;
  }
  if (mpfr_inf_p(output) && (mpfr_inf_p(left) || mpfr_inf_p(right))) return true;
  return RoundToFormat(output, inexact, context.rounding, result);
}
static NumericResult ApplyTextOperation(const NumericRequest& request) {
  NumericResult result;
  if (!ValidateContext(request.operation, request.context, result)) return result;
  if (request.left.type != kType ||
      (request.operation != NumericOperation::canonicalize && request.right.type != kType)) {
    result.status = request.left.type != kType
        ? NumericStatusCode::invalid_left : NumericStatusCode::invalid_right;
    result.invalid = true;
    result.diagnostic_code = kInvalid;
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

static BinaryResult EncodeLittleEndian(std::string_view text, const NumericContext& context) {
  BinaryResult result;
  if (!ValidateContext(NumericOperation::canonicalize, context, result.numeric)) return result;
  Environment environment;
  Number value;
  Lexical kind;
  const NumericValue input{kType, std::string(text), false};
  if (Read(input, context, value.value, kind, false, result.numeric))
    result.bytes = WriteBits(value.value, kind == Lexical::signaling_nan);
  return result;
}

static BinaryResult DecodeLittleEndian(
    const std::uint8_t* bytes, std::size_t size, const NumericContext& context,
    bool render_canonical_text) {
  BinaryResult result;
  if (!ValidateContext(NumericOperation::canonicalize, context, result.numeric)) return result;
  if (!bytes || size != Bytes{}.size()) {
    result.numeric.status = NumericStatusCode::invalid_left;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = "NUMERIC.ENCODING.NONCANONICAL";
    return result;
  }
  Bytes input;
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

static TotalOrderKeyResult MakeTotalOrderKey(
    const std::uint8_t* bytes, std::size_t size, const NumericContext& context) {
  auto decoded = DecodeLittleEndian(bytes, size, context, false);
  TotalOrderKeyResult result;
  result.numeric = std::move(decoded.numeric);
  if (result.numeric.status != NumericStatusCode::ok || !decoded.bytes) return result;
  Bytes key;
  std::reverse_copy(decoded.bytes->begin(), decoded.bytes->end(), key.begin());
  if (key.front() & 0x80) {
    for (auto& byte : key) byte = static_cast<std::uint8_t>(~byte);
  } else {
    key.front() ^= 0x80;
  }
  result.key = key;
  return result;
}

static BinaryResult ApplyBinaryOperation(const BinaryRequest& request) {
  BinaryResult result;
  if (!ValidateContext(request.operation, request.context, result.numeric)) return result;
  if (!request.left || (request.operation != NumericOperation::canonicalize && !request.right)) {
    result.numeric.status = !request.left ? NumericStatusCode::invalid_left : NumericStatusCode::invalid_right;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = kInvalid;
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
};
}  // namespace

const char* Real128BackendName() { return "MPFR/GMP binary128 reference"; }
bool Real128BackendAvailable() noexcept {
  return mpfr_buildopt_tls_p() != 0 &&
      BinaryRealReference<128>::VersionAtLeast(mpfr_get_version(), 4, 2, 1) &&
      BinaryRealReference<128>::VersionAtLeast(gmp_version, 6, 2, 0);
}
void ReleaseReal128ThreadCache() noexcept {
  if (Real128BackendAvailable()) mpfr_free_cache2(MPFR_FREE_LOCAL_CACHE);
}


namespace detail {
NumericResult Real128ReferenceOperation(const NumericRequest& request) {
  return BinaryRealReference<128>::ApplyTextOperation(request);
}
NumericResult Real64ReferenceOperation(const NumericRequest& request) {
  return BinaryRealReference<64>::ApplyTextOperation(request);
}
}


Real128BinaryResult EncodeReal128LittleEndian(std::string_view text, const NumericContext& context) {
  return BinaryRealReference<128>::EncodeLittleEndian(text, context);
}
Real128BinaryResult DecodeReal128LittleEndian(const std::uint8_t* bytes, std::size_t size,
    const NumericContext& context, bool render_canonical_text) {
  return BinaryRealReference<128>::DecodeLittleEndian(bytes, size, context, render_canonical_text);
}
Real128BinaryResult ApplyReal128BinaryOperation(const Real128BinaryRequest& request) {
  return BinaryRealReference<128>::ApplyBinaryOperation(request);
}
Real128TotalOrderKeyResult MakeReal128TotalOrderKey(const std::uint8_t* bytes, std::size_t size,
    const NumericContext& context) {
  return BinaryRealReference<128>::MakeTotalOrderKey(bytes, size, context);
}

Real64BinaryResult EncodeReal64LittleEndian(std::string_view text, const NumericContext& context) {
  return BinaryRealReference<64>::EncodeLittleEndian(text, context);
}
Real64BinaryResult DecodeReal64LittleEndian(const std::uint8_t* bytes, std::size_t size,
    const NumericContext& context, bool render_canonical_text) {
  return BinaryRealReference<64>::DecodeLittleEndian(bytes, size, context, render_canonical_text);
}
Real64BinaryResult ApplyReal64BinaryOperation(const Real64BinaryRequest& request) {
  return BinaryRealReference<64>::ApplyBinaryOperation(request);
}
Real64TotalOrderKeyResult MakeReal64TotalOrderKey(const std::uint8_t* bytes, std::size_t size,
    const NumericContext& context) {
  return BinaryRealReference<64>::MakeTotalOrderKey(bytes, size, context);
}

Real64BinaryResult IntegerLittleEndianToReal64(
    const std::uint8_t* bytes, std::size_t size, bool is_signed,
    const NumericContext& context) {
  using R = BinaryRealReference<64>;
  Real64BinaryResult result;
  if (!R::ValidateContext(NumericOperation::canonicalize, context, result.numeric)) return result;
  if (!bytes || (size != 1 && size != 2 && size != 4 && size != 8 && size != 16)) {
    result.numeric.status = NumericStatusCode::invalid_left;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = "NUMERIC.ENCODING.NONCANONICAL";
    return result;
  }
  R::Environment environment;
  R::Integer integer, modulus;
  mpz_import(integer.value, size, -1, 1, 0, 0, bytes);
  if (is_signed && (bytes[size - 1] & 0x80)) {
    mpz_setbit(modulus.value, size * 8);
    mpz_sub(integer.value, integer.value, modulus.value);
  }
  R::Number value;
  const int inexact = mpfr_set_z(value.value, integer.value, MPFR_RNDZ);
  if (R::RoundToFormat(value.value, inexact, context.rounding, result.numeric))
    result.bytes = R::WriteBits(value.value);
  return result;
}

Real64IntegerResult Real64ToIntegerLittleEndian(
    const Real64Bytes& bytes, std::size_t width, bool is_signed,
    const NumericContext& context) {
  using R = BinaryRealReference<64>;
  Real64IntegerResult result;
  if (!R::ValidateContext(NumericOperation::canonicalize, context, result.numeric)) return result;
  if (width != 1 && width != 2 && width != 4 && width != 8 && width != 16) {
    result.numeric.status = NumericStatusCode::invalid_context;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = R::kInvalid;
    return result;
  }
  R::Environment environment;
  R::Number value;
  R::Lexical kind;
  if (!R::ReadBits(bytes, context, value.value, kind, false, result.numeric)) return result;
  if (!mpfr_number_p(value.value) || !mpfr_integer_p(value.value)) {
    result.numeric.status = NumericStatusCode::invalid_operation;
    result.numeric.invalid = true;
    result.numeric.inexact = mpfr_number_p(value.value) != 0;
    result.numeric.diagnostic_code = R::kInvalid;
    return result;
  }
  R::Integer integer, limit;
  mpfr_get_z(integer.value, value.value, MPFR_RNDZ);
  mpz_setbit(limit.value, width * 8 - (is_signed ? 1 : 0));
  bool fits = mpz_cmp(integer.value, limit.value) < 0;
  if (is_signed) mpz_neg(limit.value, limit.value);
  else mpz_set_ui(limit.value, 0);
  fits &= mpz_cmp(integer.value, limit.value) >= 0;
  if (!fits) {
    result.numeric.status = NumericStatusCode::overflow;
    result.numeric.overflow = true;
    result.numeric.diagnostic_code = R::kOverflow;
    return result;
  }
  mpz_fdiv_r_2exp(integer.value, integer.value, width * 8);
  result.bytes.resize(width);
  mpz_export(result.bytes.data(), nullptr, -1, 1, 0, 0, integer.value);
  return result;
}

namespace {
template<unsigned From, unsigned To>
typename BinaryRealReference<To>::BinaryResult ConvertReal(
    const typename BinaryRealReference<From>::Bytes& bytes, const NumericContext& context) {
  using S = BinaryRealReference<From>;
  using D = BinaryRealReference<To>;
  typename D::BinaryResult result;
  if (!D::ValidateContext(NumericOperation::canonicalize, context, result.numeric)) return result;
  typename D::Environment environment;
  typename S::Number source;
  typename S::Lexical kind;
  if (!S::ReadBits(bytes, context, source.value, kind, false, result.numeric)) return result;
  if (kind == S::Lexical::signaling_nan) {
    result.numeric.status = NumericStatusCode::invalid_operation;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = D::kInvalid;
    return result;
  }
  if (kind == S::Lexical::quiet_nan) {
    // Preserve sign and most significant payload bits; retain the quiet bit.
    typename D::Bytes output{};
    typename D::Integer payload;
    mpz_import(payload.value, bytes.size(), -1, 1, 0, 0, bytes.data());
    mpz_fdiv_r_2exp(payload.value, payload.value, S::kFractionBits);
    if constexpr (To > From) mpz_mul_2exp(payload.value, payload.value, D::kFractionBits - S::kFractionBits);
    else {
      result.numeric.inexact = mpz_scan1(payload.value, 0) < S::kFractionBits - D::kFractionBits;
      mpz_tdiv_q_2exp(payload.value, payload.value, S::kFractionBits - D::kFractionBits);
    }
    mpz_export(output.data(), nullptr, -1, 1, 0, 0, payload.value);
    for (unsigned bit = 0; bit < D::kExponentBits; ++bit)
      output[(D::kFractionBits + bit) / 8] |= 1u << ((D::kFractionBits + bit) % 8);
    output.back() |= bytes.back() & 0x80;
    result.bytes = output;
    return result;
  }
  typename D::Number target;
  const int inexact = mpfr_set(target.value, source.value, MPFR_RNDZ);
  if (kind == S::Lexical::infinity ||
      D::RoundToFormat(target.value, inexact, context.rounding, result.numeric))
    result.bytes = D::WriteBits(target.value);
  return result;
}
}  // namespace

Real64BinaryResult Real128ToReal64(const Real128Bytes& bytes, const NumericContext& context) {
  return ConvertReal<128, 64>(bytes, context);
}
Real128BinaryResult Real64ToReal128(const Real64Bytes& bytes, const NumericContext& context) {
  return ConvertReal<64, 128>(bytes, context);
}
}  // namespace scratchbird::libraries::sbl_numeric
