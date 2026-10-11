// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>

namespace scratchbird::libraries::sbl_numeric {
namespace {
struct Parts {
  bool negative = false;
  bool zero = false;
  unsigned scale = 0;
  unsigned count = 0;
  // Numeric digits, not an ASCII carrier. Fixed scratch independent of input.
  std::array<std::uint8_t, 81> digits{};
};
ExactDecimalError Read(const std::uint8_t* bytes, std::size_t size,
                       const ExactDecimalProfile& profile, Parts* out) noexcept {
  if (!ExactDecimalProfileValid(profile)) return ExactDecimalError::invalid_profile;
  const unsigned capacity = profile.codec == ExactDecimalCodec::le24_v1 ? 5 : 9;
  const unsigned maximum = capacity == 5 ? 38 : 76;
  if (!bytes || size != 4 + 4 * capacity) return ExactDecimalError::invalid_encoding;
  Parts p;
  p.negative = (bytes[0] & 0x80) != 0;
  p.scale = bytes[0] & 0x7f;
  const unsigned used = bytes[2];
  if (p.scale > maximum || !bytes[1] || bytes[1] > maximum ||
      !used || used > capacity || bytes[3]) return ExactDecimalError::invalid_encoding;
  std::array<std::uint32_t, 9> groups{};
  for (unsigned g = 0; g < capacity; ++g) {
    for (unsigned b = 0; b < 4; ++b)
      groups[g] |= std::uint32_t(bytes[4 + 4*g + b]) << (8*b);
    if (groups[g] >= 1000000000 || (g >= used && groups[g]))
      return ExactDecimalError::invalid_encoding;
  }
  p.zero = used == 1 && groups[0] == 0;
  if ((used > 1 && !groups[used-1]) ||
      (p.zero && (p.negative || p.scale || bytes[1] != 1)) ||
      (!p.zero && p.scale && groups[0] % 10 == 0))
    return ExactDecimalError::invalid_encoding;
  unsigned leading = 0;
  for (auto n = groups[used-1]; n; n /= 10) ++leading;
  if (!leading) leading = 1;
  p.count = 9 * (used-1) + leading;
  if (p.count > maximum || std::max(p.count, p.scale) != bytes[1])
    return ExactDecimalError::invalid_encoding;
  unsigned position = p.count;
  for (unsigned g = 0; g < used; ++g) {
    auto n = groups[g];
    const unsigned digits = g + 1 == used ? leading : 9;
    for (unsigned d = 0; d < digits; ++d) {
      p.digits[--position] = n % 10;
      n /= 10;
    }
  }
  if (!p.zero) {
    if (p.scale > profile.scale) return ExactDecimalError::scale_loss;
    if (p.count + profile.scale - p.scale > profile.precision)
      return ExactDecimalError::precision_overflow;
  }
  *out = p;
  return ExactDecimalError::none;
}
}
bool ExactDecimalProfileValid(const ExactDecimalProfile& p) noexcept {
  return p.scale <= p.precision &&
      ((p.codec == ExactDecimalCodec::le24_v1 && p.precision >= 1 && p.precision <= 38) ||
       (p.codec == ExactDecimalCodec::le40_v1 && p.precision >= 39 && p.precision <= 76));
}
const char* ExactDecimalErrorName(ExactDecimalError e) noexcept {
  switch (e) {
    case ExactDecimalError::none: return "decimal_ok";
    case ExactDecimalError::invalid_profile: return "decimal_profile_invalid";
    case ExactDecimalError::invalid_encoding: return "decimal_encoding_invalid";
    case ExactDecimalError::scale_loss: return "decimal_scale_loss";
    case ExactDecimalError::precision_overflow: return "decimal_precision_overflow";
    case ExactDecimalError::invalid_text: return "decimal_text_invalid";
  }
  return "decimal_error_invalid";
}
ExactDecimalError ValidateExactDecimal(const std::uint8_t* bytes, std::size_t size,
                                       const ExactDecimalProfile& profile) noexcept {
  Parts p;
  return Read(bytes, size, profile, &p);
}
ExactDecimalArithmeticResult ApplyExactDecimalArithmetic(
    NumericOperation operation, ExactDecimalOperand left,
    ExactDecimalOperand right, ExactDecimalProfile result_profile,
    RoundingMode rounding) noexcept {
  ExactDecimalArithmeticResult result;
  if (!ExactDecimalProfileValid(result_profile) ||
      (rounding != RoundingMode::half_even && rounding != RoundingMode::half_up &&
       rounding != RoundingMode::truncate)) return result;
  switch (operation) {
    case NumericOperation::canonicalize: case NumericOperation::add:
    case NumericOperation::subtract: case NumericOperation::multiply:
    case NumericOperation::divide: case NumericOperation::compare: break;
    default: result.status = NumericStatusCode::invalid_operation; return result;
  }
  Parts l, r;
  if (Read(left.bytes, left.size, left.profile, &l) != ExactDecimalError::none) {
    result.status = NumericStatusCode::invalid_left; return result;
  }
  if (operation != NumericOperation::canonicalize &&
      Read(right.bytes, right.size, right.profile, &r) != ExactDecimalError::none) {
    result.status = NumericStatusCode::invalid_right; return result;
  }
  // The largest numerator is coefficient * 10^(right_scale + result_scale),
  // strictly below 10^228 < 2^758. Fixed 1024-bit storage covers every profile,
  // including doubled remainders. Allocator void prohibits heap-backed limbs.
  using Integer = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
      1024, 1024, boost::multiprecision::signed_magnitude,
      boost::multiprecision::unchecked, void>>;
  const auto coefficient = [](const Parts& p) {
    Integer value = 0;
    for (unsigned i = 0; i < p.count; ++i) { value *= 10; value += p.digits[i]; }
    if (p.negative) value = -value;
    return value;
  };
  const auto power = [](unsigned exponent) {
    Integer value = 1;
    for (unsigned i = 0; i < exponent; ++i) value *= 10;
    return value;
  };
  Integer numerator = coefficient(l), denominator = 1;
  const Integer rhs = coefficient(r);
  unsigned scale = l.scale;
  if (operation == NumericOperation::add || operation == NumericOperation::subtract ||
      operation == NumericOperation::compare) {
    scale = std::max(l.scale, r.scale);
    numerator *= power(scale - l.scale);
    const Integer aligned_right = rhs * power(scale - r.scale);
    if (operation == NumericOperation::compare) {
      result.status = NumericStatusCode::ok;
      result.comparison = numerator < aligned_right ? -1 : numerator > aligned_right ? 1 : 0;
      return result;
    }
    if (operation == NumericOperation::add) numerator += aligned_right;
    else numerator -= aligned_right;
  } else if (operation == NumericOperation::multiply) {
    numerator *= rhs; scale += r.scale;
  } else if (operation == NumericOperation::divide) {
    if (rhs == 0) { result.status = NumericStatusCode::divide_by_zero; return result; }
    numerator *= power(r.scale + result_profile.scale);
    denominator = rhs * power(l.scale);
    scale = result_profile.scale;
  }
  if (scale < result_profile.scale) numerator *= power(result_profile.scale - scale);
  else if (scale > result_profile.scale) denominator *= power(scale - result_profile.scale);
  const bool negative = (numerator < 0) != (denominator < 0);
  if (numerator < 0) numerator = -numerator;
  if (denominator < 0) denominator = -denominator;
  Integer quantized = numerator / denominator;
  const Integer remainder = numerator % denominator;
  result.inexact = remainder != 0;
  // Retain the entire remainder, not just one guard digit: 0.2501 is not a
  // half-even tie at scale 1, and recurring quotients must not become ties.
  if (rounding != RoundingMode::truncate &&
      (2 * remainder > denominator || (2 * remainder == denominator &&
       (rounding == RoundingMode::half_up || (quantized & 1) != 0)))) ++quantized;
  if (quantized >= power(result_profile.precision)) {
    result.status = NumericStatusCode::overflow; return result;
  }
  scale = result_profile.scale;
  if (quantized == 0) scale = 0;
  else while (scale && quantized % 10 == 0) { quantized /= 10; --scale; }
  auto digits_value = quantized;
  unsigned digits = 1;
  while (digits_value >= 10) { digits_value /= 10; ++digits; }
  result.bytes[0] = static_cast<std::uint8_t>(scale | (negative && quantized != 0 ? 128 : 0));
  result.bytes[1] = static_cast<std::uint8_t>(std::max(digits, scale));
  unsigned groups = 0;
  do {
    const auto group = (quantized % 1000000000).convert_to<std::uint32_t>();
    quantized /= 1000000000;
    for (unsigned b = 0; b < 4; ++b)
      result.bytes[4 + 4 * groups + b] = static_cast<std::uint8_t>(group >> (8*b));
    ++groups;
  } while (quantized != 0);
  result.bytes[2] = static_cast<std::uint8_t>(groups);
  result.size = result_profile.codec == ExactDecimalCodec::le24_v1 ? 24 : 40;
  result.status = NumericStatusCode::ok;
  return result;
}
ExactDecimalArithmeticResult Real64ToExactDecimal(
    const Real64Bytes& source, ExactDecimalProfile profile,
    RoundingMode rounding) noexcept {
  ExactDecimalArithmeticResult result;
  if (!ExactDecimalProfileValid(profile) ||
      (rounding != RoundingMode::half_even && rounding != RoundingMode::half_up &&
       rounding != RoundingMode::truncate)) return result;
  std::uint64_t bits = 0;
  for (unsigned i = 0; i < 8; ++i) bits |= std::uint64_t(source[i]) << (8*i);
  const unsigned exponent = (bits >> 52) & 0x7ff;
  const bool negative = (bits >> 63) != 0;
  if (exponent == 0x7ff) { result.status = NumericStatusCode::invalid_left; return result; }
  const auto significand = (bits & ((std::uint64_t{1} << 52) - 1)) |
                          (exponent ? std::uint64_t{1} << 52 : 0);
  const int shift = exponent ? int(exponent) - 1023 - 52 : -1074;
  // Largest scaled finite binary64 numerator is below 2^1024 * 10^76
  // (<2^1277); smallest subnormal denominator is 2^1074. Fixed 2048-bit
  // stack limbs cover both and the doubled remainder without allocation.
  using Integer = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
      2048, 2048, boost::multiprecision::unsigned_magnitude,
      boost::multiprecision::unchecked, void>>;
  const auto power = [](unsigned n) { Integer v = 1; while (n--) v *= 10; return v; };
  Integer numerator = significand;
  numerator *= power(profile.scale);
  Integer denominator = 1;
  if (shift >= 0) numerator <<= shift;
  else denominator <<= -shift;
  Integer quantized = numerator / denominator;
  const Integer remainder = numerator % denominator;
  result.inexact = remainder != 0;
  if (rounding != RoundingMode::truncate &&
      (2 * remainder > denominator || (2 * remainder == denominator &&
       (rounding == RoundingMode::half_up || (quantized & 1) != 0)))) ++quantized;
  if (quantized >= power(profile.precision)) {
    result.status = NumericStatusCode::overflow; return result;
  }
  unsigned scale = profile.scale;
  if (quantized == 0) scale = 0;
  else while (scale && quantized % 10 == 0) { quantized /= 10; --scale; }
  auto digits_value = quantized;
  unsigned digits = 1;
  while (digits_value >= 10) { digits_value /= 10; ++digits; }
  result.bytes[0] = static_cast<std::uint8_t>(scale | (negative && quantized != 0 ? 128 : 0));
  result.bytes[1] = static_cast<std::uint8_t>(std::max(digits, scale));
  unsigned groups = 0;
  do {
    const auto group = (quantized % 1000000000).convert_to<std::uint32_t>();
    quantized /= 1000000000;
    for (unsigned b = 0; b < 4; ++b)
      result.bytes[4 + 4 * groups + b] = static_cast<std::uint8_t>(group >> (8*b));
    ++groups;
  } while (quantized != 0);
  result.bytes[2] = static_cast<std::uint8_t>(groups);
  result.size = profile.codec == ExactDecimalCodec::le24_v1 ? 24 : 40;
  result.status = NumericStatusCode::ok;
  return result;
}

Real64BinaryResult ExactDecimalUnitFractionToReal64(
    const std::uint8_t* bytes, std::size_t size,
    const ExactDecimalProfile& profile) {
  Real64BinaryResult result;
  result.numeric.value.type = NumericType::real64;
  Parts p;
  if (Read(bytes, size, profile, &p) != ExactDecimalError::none) {
    result.numeric.status = NumericStatusCode::invalid_left;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = "NUMERIC.ENCODING.NONCANONICAL";
    return result;
  }
  // 10^76 is less than 2^253. Nine 32-bit words hold both operands
  // and a doubled remainder without heap allocation or host float state.
  using Words = std::array<std::uint32_t, 9>;
  const auto mul_add = [](Words& n, std::uint32_t m, std::uint32_t a) {
    std::uint64_t carry = a;
    for (auto& word : n) {
      const std::uint64_t next = std::uint64_t(word) * m + carry;
      word = static_cast<std::uint32_t>(next);
      carry = next >> 32;
    }
  };
  const auto compare = [](const Words& a, const Words& b) {
    for (std::size_t i = a.size(); i > 0; --i)
      if (a[i-1] != b[i-1]) return a[i-1] < b[i-1] ? -1 : 1;
    return 0;
  };
  const auto subtract = [](Words& a, const Words& b) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      const std::uint64_t subtrahend = std::uint64_t(b[i]) + borrow;
      borrow = std::uint64_t(a[i]) < subtrahend;
      a[i] = static_cast<std::uint32_t>(std::uint64_t(a[i]) - subtrahend);
    }
  };
  Words remainder{}, denominator{};
  for (unsigned i = 0; i < p.count; ++i) mul_add(remainder, 10, p.digits[i]);
  denominator[0] = 1;
  for (unsigned i = 0; i < p.scale; ++i) mul_add(denominator, 10, 0);
  if (p.negative || compare(remainder, denominator) > 0) {
    result.numeric.status = NumericStatusCode::invalid_left;
    result.numeric.invalid = true;
    result.numeric.diagnostic_code = "NUMERIC.REAL64.INVALID";
    return result;
  }
  Real64Bytes encoded{};
  if (p.zero) { result.bytes = encoded; return result; }
  int exponent = 0;
  while (compare(remainder, denominator) < 0) {
    mul_add(remainder, 2, 0);
    --exponent;
  }
  std::uint64_t significand = 0;
  for (unsigned bit = 0; bit < 53; ++bit) {
    significand <<= 1;
    if (compare(remainder, denominator) >= 0) {
      subtract(remainder, denominator);
      significand |= 1;
    }
    if (bit != 52) mul_add(remainder, 2, 0);
  }
  result.numeric.inexact = std::any_of(remainder.begin(), remainder.end(),
                                     [](auto word) { return word != 0; });
  mul_add(remainder, 2, 0);
  const int halfway = compare(remainder, denominator);
  if (halfway > 0 || (halfway == 0 && (significand & 1))) {
    if (++significand == (std::uint64_t{1} << 53)) {
      significand >>= 1;
      ++exponent;
    }
  }
  // The smallest nonzero profile value is 10^-76: no subnormal is possible.
  const auto bits = (std::uint64_t(exponent + 1023) << 52) |
                    (significand & ((std::uint64_t{1} << 52) - 1));
  for (unsigned i = 0; i < 8; ++i) encoded[i] = static_cast<std::uint8_t>(bits >> (8*i));
  result.bytes = encoded;
  return result;
}
ExactDecimalOrderKeyResult MakeExactDecimalOrderKey(
    const std::uint8_t* bytes, std::size_t size,
    const ExactDecimalProfile& profile) noexcept {
  Parts p;
  ExactDecimalOrderKeyResult result;
  result.error = Read(bytes, size, profile, &p);
  if (result.error != ExactDecimalError::none) return result;
  std::array<std::uint8_t, 40> key{};
  key[0] = p.zero ? 1 : p.negative ? 0 : 2;
  if (!p.zero) {
    // Scientific exponent digits-scale, range -75..76, with unsigned bias75.
    key[1] = static_cast<std::uint8_t>(int(p.count) - int(p.scale) + 75);
    for (unsigned d = 0; d < p.count; ++d)
      key[2 + d/2] |= p.digits[d] << (d%2 ? 0 : 4);
    if (p.negative) for (unsigned b = 1; b < key.size(); ++b) key[b] ^= 0xff;
  }
  result.key = key;
  return result;
}
BoundExactDecimalResult EncodeBoundExactDecimal(
    std::string_view text, const ExactDecimalProfile& profile) {
  BoundExactDecimalResult result;
  if (!ExactDecimalProfileValid(profile)) {
    result.error = ExactDecimalError::invalid_profile;
    return result;
  }
  if (text.empty() || text.size() > 256) {
    result.error = ExactDecimalError::invalid_text;
    return result;
  }
  // Scan bounded lexical input before expanding exponents. No arbitrary
  // precision coefficient or exponent-sized temporary is ever constructed.
  const auto space = [](char c) {
    return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f' || c=='\v';
  };
  while (!text.empty() && space(text.front())) text.remove_prefix(1);
  while (!text.empty() && space(text.back())) text.remove_suffix(1);
  bool negative = false;
  if (!text.empty() && (text.front()=='-' || text.front()=='+')) {
    negative = text.front()=='-'; text.remove_prefix(1);
  }
  std::string digits;
  bool point = false;
  int scale = 0;
  std::size_t cursor = 0;
  for (; cursor < text.size(); ++cursor) {
    const auto c = text[cursor];
    if (c=='.' && !point) { point=true; continue; }
    if (c<'0' || c>'9') break;
    digits.push_back(c);
    if (point) ++scale;
  }
  if (digits.empty()) { result.error=ExactDecimalError::invalid_text; return result; }
  if (cursor < text.size() && (text[cursor]=='e' || text[cursor]=='E')) {
    ++cursor;
    bool minus = false;
    if (cursor < text.size() && (text[cursor]=='+' || text[cursor]=='-'))
      minus = text[cursor++]=='-';
    const auto begin = cursor;
    int exponent = 0;
    for (; cursor < text.size() && text[cursor]>='0' && text[cursor]<='9'; ++cursor)
      exponent = std::min(333, exponent*10 + text[cursor]-'0');
    if (begin == cursor) { result.error=ExactDecimalError::invalid_text; return result; }
    scale += minus ? exponent : -exponent;
  }
  if (cursor != text.size()) { result.error=ExactDecimalError::invalid_text; return result; }
  const auto start = digits.find_first_not_of('0');
  if (start == std::string::npos) { digits="0"; scale=0; negative=false; }
  else {
    digits.erase(0,start);
    while (scale > 0 && digits.back()=='0') { digits.pop_back(); --scale; }
  }
  if (scale < 0) {
    if (digits.size() + static_cast<unsigned>(-scale) > profile.precision) {
      result.error=ExactDecimalError::precision_overflow; return result;
    }
    digits.append(static_cast<unsigned>(-scale),'0'); scale=0;
  }
  if (static_cast<unsigned>(scale) > profile.scale) { result.error = ExactDecimalError::scale_loss; return result; }
  if (digits != "0" && digits.size() + profile.scale - scale > profile.precision) {
    result.error = ExactDecimalError::precision_overflow;
    return result;
  }
  std::vector<std::uint8_t> bytes(profile.codec == ExactDecimalCodec::le24_v1 ? 24 : 40);
  bytes[0] = static_cast<std::uint8_t>(scale) | (negative && digits != "0" ? 0x80 : 0);
  bytes[1] = static_cast<std::uint8_t>(std::max(digits.size(), static_cast<std::size_t>(scale)));
  bytes[2] = static_cast<std::uint8_t>((digits.size()+8)/9);
  auto end = digits.size();
  for (unsigned g = 0; end; ++g) {
    const auto begin = end > 9 ? end-9 : 0;
    std::uint32_t n = 0;
    for (auto i = begin; i < end; ++i) n = 10*n + digits[i]-'0';
    for (unsigned b = 0; b < 4; ++b) bytes[4 + 4*g + b] = n >> (8*b);
    end = begin;
  }
  result.error = ValidateExactDecimal(bytes.data(), bytes.size(), profile);
  if (result.error == ExactDecimalError::none) {
    result.bytes = std::move(bytes);
    if (negative) result.text.push_back('-');
    if (scale && digits.size() <= static_cast<unsigned>(scale)) {
      result.text += "0.";
      result.text.append(scale-digits.size(),'0');
      result.text += digits;
    } else {
      result.text += digits;
      if (scale) result.text.insert(result.text.size()-scale,1,'.');
    }
  }
  return result;
}
BoundExactDecimalResult DecodeBoundExactDecimal(const std::uint8_t* bytes,
    std::size_t size, const ExactDecimalProfile& profile, bool render_text) {
  Parts p;
  BoundExactDecimalResult result;
  result.error = Read(bytes, size, profile, &p);
  if (result.error != ExactDecimalError::none) return result;
  result.bytes.assign(bytes, bytes+size);
  if (render_text) {
    if (p.negative) result.text.push_back('-');
    if (p.scale >= p.count) {
      result.text += "0.";
      result.text.append(p.scale - p.count, '0');
    }
    for (unsigned d = 0; d < p.count; ++d) {
      if (p.scale && p.scale < p.count && d == p.count-p.scale) result.text.push_back('.');
      result.text.push_back(static_cast<char>('0'+p.digits[d]));
    }
  }
  return result;
}
}  // namespace scratchbird::libraries::sbl_numeric
