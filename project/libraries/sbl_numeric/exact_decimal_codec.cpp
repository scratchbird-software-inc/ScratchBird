// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include <algorithm>

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
