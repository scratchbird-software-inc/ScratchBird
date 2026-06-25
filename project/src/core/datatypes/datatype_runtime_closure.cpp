// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"

#include "sbl_numeric.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>

namespace scratchbird::core::datatypes {
namespace {
namespace numeric = scratchbird::libraries::sbl_numeric;

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string EncodeUtf8Codepoint(std::uint32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7f) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | ((codepoint >> 6) & 0x1f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | ((codepoint >> 12) & 0x0f)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  return out;
}

bool DecodeNextUtf8Codepoint(const std::string& value,
                             std::size_t* offset,
                             std::uint32_t* codepoint) {
  const auto byte = static_cast<unsigned char>(value[*offset]);
  if (byte < 0x80) {
    *codepoint = byte;
    ++(*offset);
    return true;
  }
  const auto continuation = [&value](std::size_t pos, unsigned char* out) {
    if (pos >= value.size()) { return false; }
    const auto next = static_cast<unsigned char>(value[pos]);
    if ((next & 0xc0) != 0x80) { return false; }
    *out = next;
    return true;
  };
  unsigned char b1 = 0;
  unsigned char b2 = 0;
  unsigned char b3 = 0;
  if ((byte & 0xe0) == 0xc0) {
    if (!continuation(*offset + 1, &b1)) { return false; }
    *codepoint = ((byte & 0x1f) << 6) | (b1 & 0x3f);
    *offset += 2;
    return true;
  }
  if ((byte & 0xf0) == 0xe0) {
    if (!continuation(*offset + 1, &b1) ||
        !continuation(*offset + 2, &b2)) {
      return false;
    }
    *codepoint = ((byte & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
    *offset += 3;
    return true;
  }
  if ((byte & 0xf8) == 0xf0) {
    if (!continuation(*offset + 1, &b1) ||
        !continuation(*offset + 2, &b2) ||
        !continuation(*offset + 3, &b3)) {
      return false;
    }
    *codepoint = ((byte & 0x07) << 18) | ((b1 & 0x3f) << 12) |
                 ((b2 & 0x3f) << 6) | (b3 & 0x3f);
    *offset += 4;
    return true;
  }
  return false;
}

bool IsUpperLatinCodepoint(std::uint32_t codepoint) {
  return (codepoint >= 'A' && codepoint <= 'Z') ||
         (codepoint >= 0x00c0 && codepoint <= 0x00d6) ||
         (codepoint >= 0x00d8 && codepoint <= 0x00de) ||
         codepoint == 0x0152 || codepoint == 0x0178 || codepoint == 0x1e9e;
}

std::uint32_t LowerLatinCodepoint(std::uint32_t codepoint) {
  if (codepoint >= 'A' && codepoint <= 'Z') { return codepoint + 0x20; }
  if ((codepoint >= 0x00c0 && codepoint <= 0x00d6) ||
      (codepoint >= 0x00d8 && codepoint <= 0x00de)) {
    return codepoint + 0x20;
  }
  if (codepoint == 0x0152) { return 0x0153; }
  if (codepoint == 0x0178) { return 0x00ff; }
  if (codepoint == 0x1e9e) { return 0x00df; }
  return codepoint;
}

bool AppendAccentFoldedLatin(std::uint32_t codepoint,
                             bool case_insensitive,
                             std::string* out) {
  const bool upper = IsUpperLatinCodepoint(codepoint);
  const auto append_char = [&](char lower, char upper_char) {
    out->push_back(case_insensitive || !upper ? lower : upper_char);
  };
  const auto append_text = [&](std::string_view lower, std::string_view upper_text) {
    out->append(case_insensitive || !upper ? lower : upper_text);
  };
  switch (codepoint) {
    case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3:
    case 0x00c4: case 0x00c5: case 0x00e0: case 0x00e1:
    case 0x00e2: case 0x00e3: case 0x00e4: case 0x00e5:
      append_char('a', 'A'); return true;
    case 0x00c6: case 0x00e6:
      append_text("ae", "AE"); return true;
    case 0x00c7: case 0x00e7:
      append_char('c', 'C'); return true;
    case 0x00d0: case 0x00f0:
      append_char('d', 'D'); return true;
    case 0x00c8: case 0x00c9: case 0x00ca: case 0x00cb:
    case 0x00e8: case 0x00e9: case 0x00ea: case 0x00eb:
      append_char('e', 'E'); return true;
    case 0x00cc: case 0x00cd: case 0x00ce: case 0x00cf:
    case 0x00ec: case 0x00ed: case 0x00ee: case 0x00ef:
      append_char('i', 'I'); return true;
    case 0x00d1: case 0x00f1:
      append_char('n', 'N'); return true;
    case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5:
    case 0x00d6: case 0x00d8: case 0x00f2: case 0x00f3:
    case 0x00f4: case 0x00f5: case 0x00f6: case 0x00f8:
      append_char('o', 'O'); return true;
    case 0x0152: case 0x0153:
      append_text("oe", "OE"); return true;
    case 0x00de: case 0x00fe:
      append_text("th", "TH"); return true;
    case 0x00d9: case 0x00da: case 0x00db: case 0x00dc:
    case 0x00f9: case 0x00fa: case 0x00fb: case 0x00fc:
      append_char('u', 'U'); return true;
    case 0x00dd: case 0x0178: case 0x00fd: case 0x00ff:
      append_char('y', 'Y'); return true;
    default:
      return false;
  }
}

std::string NormalizeLatinTextForCollation(const std::string& value,
                                           bool case_insensitive,
                                           bool accent_insensitive) {
  std::string out;
  out.reserve(value.size());
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::size_t start = offset;
    std::uint32_t codepoint = 0;
    if (!DecodeNextUtf8Codepoint(value, &offset, &codepoint)) {
      out.push_back(value[start]);
      offset = start + 1;
      continue;
    }
    if (codepoint == 0x00df && case_insensitive) {
      out.append("ss");
      continue;
    }
    if (accent_insensitive &&
        AppendAccentFoldedLatin(codepoint, case_insensitive, &out)) {
      continue;
    }
    if (case_insensitive) { codepoint = LowerLatinCodepoint(codepoint); }
    out.append(EncodeUtf8Codepoint(codepoint));
  }
  return out;
}

bool IsIntegerType(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::int8:
    case CanonicalTypeId::int16:
    case CanonicalTypeId::int32:
    case CanonicalTypeId::int64:
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint8:
    case CanonicalTypeId::uint16:
    case CanonicalTypeId::uint32:
    case CanonicalTypeId::uint64:
    case CanonicalTypeId::uint128:
      return true;
    default:
      return false;
  }
}

bool IsNumericType(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::int8:
    case CanonicalTypeId::int16:
    case CanonicalTypeId::int32:
    case CanonicalTypeId::int64:
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint8:
    case CanonicalTypeId::uint16:
    case CanonicalTypeId::uint32:
    case CanonicalTypeId::uint64:
    case CanonicalTypeId::uint128:
    case CanonicalTypeId::real16:
    case CanonicalTypeId::bfloat16:
    case CanonicalTypeId::real32:
    case CanonicalTypeId::real64:
    case CanonicalTypeId::real128:
    case CanonicalTypeId::decimal:
    case CanonicalTypeId::decimal_float:
      return true;
    default:
      return false;
  }
}

bool IsStableLexicalType(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::uuid:
    case CanonicalTypeId::date:
    case CanonicalTypeId::time:
    case CanonicalTypeId::timestamp:
    case CanonicalTypeId::interval:
    case CanonicalTypeId::ip_address:
    case CanonicalTypeId::network_prefix:
    case CanonicalTypeId::mac_address:
      return true;
    default:
      return false;
  }
}

bool UnsupportedRuntimeType(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::opaque_extension:
    case CanonicalTypeId::bridge_handle:
    case CanonicalTypeId::cursor_handle:
    case CanonicalTypeId::cursor:
    case CanonicalTypeId::result_set:
    case CanonicalTypeId::table_value:
    case CanonicalTypeId::unknown:
      return true;
    default:
      return false;
  }
}

bool TextSeedAuthorityRequested(const DatatypeTextSeedAuthority& authority,
                                bool case_insensitive_character_compare) {
  return case_insensitive_character_compare ||
         authority.active ||
         !authority.seed_pack_name.empty() ||
         !authority.seed_pack_version.empty() ||
         !authority.charset_name.empty() ||
         !authority.collation_name.empty() ||
         authority.collation_case_insensitive ||
         authority.collation_accent_insensitive;
}

bool TextSeedAuthorityReady(const DatatypeTextSeedAuthority& authority) {
  return authority.active &&
         !authority.seed_pack_name.empty() &&
         !authority.seed_pack_version.empty() &&
         !authority.charset_name.empty() &&
         !authority.collation_name.empty();
}

std::string TextSeedAuthorityDetail(const DatatypeTextSeedAuthority& authority) {
  std::ostringstream out;
  out << "seed_pack=" << authority.seed_pack_name
      << ";seed_version=" << authority.seed_pack_version
      << ";charset=" << authority.charset_name
      << ";collation=" << authority.collation_name
      << ";case_insensitive=" << (authority.collation_case_insensitive ? "true" : "false")
      << ";accent_insensitive=" << (authority.collation_accent_insensitive ? "true" : "false");
  return out.str();
}

std::string HexEncode(std::string_view value);

bool RuntimeNumericType(CanonicalTypeId type_id, numeric::NumericType* out) {
  switch (type_id) {
    case CanonicalTypeId::int128:
      *out = numeric::NumericType::int128;
      return true;
    case CanonicalTypeId::uint128:
      *out = numeric::NumericType::uint128;
      return true;
    case CanonicalTypeId::decimal:
      *out = numeric::NumericType::decimal;
      return true;
    case CanonicalTypeId::decimal_float:
      *out = numeric::NumericType::decimal_float;
      return true;
    case CanonicalTypeId::real128:
      *out = numeric::NumericType::real128;
      return true;
    default:
      return false;
  }
}

numeric::NumericContext RuntimeNumericContext(numeric::NumericType type,
                                              bool preserve_scale) {
  numeric::NumericContext context;
  context.precision = type == numeric::NumericType::decimal_float ? 34 : 38;
  context.scale = 0;
  context.allow_special_values =
      type == numeric::NumericType::decimal_float ||
      type == numeric::NumericType::real128;
  context.canonical_preserve_scale = preserve_scale;
  return context;
}

DatatypeComparisonResult NumericCompareFailure(std::string detail) {
  DatatypeComparisonResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(
      result.status,
      "SB_DATATYPE_COMPARE_NUMERIC_BACKEND_FAILED",
      "datatype.compare.numeric_backend",
      std::move(detail));
  return result;
}

DatatypeSortKeyResult RuntimeSortKeyFailure(std::string code,
                                            std::string reason) {
  DatatypeSortKeyResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(
      result.status,
      std::move(code),
      "datatype.sort_key.runtime_numeric",
      std::move(reason));
  return result;
}

DatatypeHashResult RuntimeHashFailure(std::string code, std::string reason) {
  DatatypeHashResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(
      result.status,
      std::move(code),
      "datatype.hash.runtime_numeric",
      std::move(reason));
  return result;
}

bool CompareRuntimeNumeric(const DatatypeOperationValue& left,
                           const DatatypeOperationValue& right,
                           int* comparison,
                           std::string* detail) {
  numeric::NumericType type = numeric::NumericType::decimal;
  if (!RuntimeNumericType(left.type_id, &type)) { return false; }
  numeric::NumericRequest request;
  request.type = type;
  request.operation = numeric::NumericOperation::compare;
  request.left = {type, left.encoded_value, left.is_null};
  request.right = {type, right.encoded_value, right.is_null};
  request.context = RuntimeNumericContext(type, false);
  const auto result = numeric::ApplyNumericOperation(request);
  if (result.status != numeric::NumericStatusCode::ok) {
    *detail = result.diagnostic_code.empty()
                  ? std::string(numeric::NumericStatusCodeName(result.status))
                  : result.diagnostic_code;
    return false;
  }
  *comparison = result.comparison;
  return true;
}

bool CanonicalRuntimeNumericValue(CanonicalTypeId type_id,
                                  const std::string& encoded,
                                  std::string* canonical,
                                  std::string* detail) {
  numeric::NumericType type = numeric::NumericType::decimal;
  if (!RuntimeNumericType(type_id, &type)) { return false; }
  numeric::NumericRequest request;
  request.type = type;
  request.operation = numeric::NumericOperation::canonicalize;
  request.left = {type, encoded, false};
  request.context = RuntimeNumericContext(type, true);
  const auto result = numeric::ApplyNumericOperation(request);
  if (result.status != numeric::NumericStatusCode::ok) {
    *detail = result.diagnostic_code.empty()
                  ? std::string(numeric::NumericStatusCodeName(result.status))
                  : result.diagnostic_code;
    return false;
  }
  *canonical = result.value.encoded;
  return true;
}

struct FiniteNumericParts {
  bool negative = false;
  bool zero = true;
  std::string digits = "0";
  long long exponent = 0;
};

bool ParseFiniteNumericText(std::string_view value, FiniteNumericParts* out) {
  if (value == "NaN" || value == "sNaN" || value == "Infinity" ||
      value == "-Infinity") {
    return false;
  }
  std::size_t pos = 0;
  bool negative = false;
  if (pos < value.size() && (value[pos] == '-' || value[pos] == '+')) {
    negative = value[pos] == '-';
    ++pos;
  }
  std::string digits;
  long long scale = 0;
  bool saw_digit = false;
  bool after_decimal = false;
  for (; pos < value.size(); ++pos) {
    const char c = value[pos];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      saw_digit = true;
      digits.push_back(c);
      if (after_decimal) { ++scale; }
      continue;
    }
    if (c == '.' && !after_decimal) {
      after_decimal = true;
      continue;
    }
    break;
  }
  if (!saw_digit) { return false; }
  long long exponent_adjust = 0;
  if (pos < value.size()) {
    if (value[pos] != 'e' && value[pos] != 'E') { return false; }
    ++pos;
    bool exponent_negative = false;
    if (pos < value.size() && (value[pos] == '-' || value[pos] == '+')) {
      exponent_negative = value[pos] == '-';
      ++pos;
    }
    if (pos == value.size()) { return false; }
    long long parsed = 0;
    for (; pos < value.size(); ++pos) {
      if (!std::isdigit(static_cast<unsigned char>(value[pos]))) { return false; }
      if (parsed > 1000000) { return false; }
      parsed = (parsed * 10) + static_cast<long long>(value[pos] - '0');
    }
    exponent_adjust = exponent_negative ? -parsed : parsed;
  }
  while (digits.size() > 1 && digits.front() == '0') { digits.erase(digits.begin()); }
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
    --scale;
  }
  if (digits == "0") {
    *out = {};
    return true;
  }
  const long long exponent =
      static_cast<long long>(digits.size()) - scale + exponent_adjust;
  if (exponent < -100000 || exponent > 100000) { return false; }
  out->negative = negative;
  out->zero = false;
  out->digits = std::move(digits);
  out->exponent = exponent;
  return true;
}

std::string EncodeFixedExponent(long long exponent) {
  std::ostringstream out;
  out << std::setw(6) << std::setfill('0') << (exponent + 100000);
  return out.str();
}

std::string HexEncodeLexBytes(std::string material, bool invert) {
  material.push_back('\0');
  if (invert) {
    for (char& c : material) {
      c = static_cast<char>(0xffu - static_cast<unsigned char>(c));
    }
  }
  return HexEncode(material);
}

std::optional<std::string> RuntimeNumericSortBody(CanonicalTypeId type_id,
                                                  const std::string& encoded,
                                                  std::string* detail) {
  std::string canonical;
  if (!CanonicalRuntimeNumericValue(type_id, encoded, &canonical, detail)) {
    return std::nullopt;
  }
  const std::string type_prefix =
      std::string("31:") + CanonicalTypeName(type_id) + ':';
  if (canonical == "-Infinity") { return type_prefix + "0"; }
  if (canonical == "Infinity") { return type_prefix + "4"; }
  if (canonical == "NaN" || canonical == "sNaN") {
    *detail = "numeric_sort_key_unordered:" + canonical;
    return std::nullopt;
  }
  FiniteNumericParts parts;
  if (!ParseFiniteNumericText(canonical, &parts)) {
    *detail = "numeric_sort_key_invalid:" + canonical;
    return std::nullopt;
  }
  if (parts.zero) { return type_prefix + "2"; }
  const std::string material = EncodeFixedExponent(parts.exponent) + ':' + parts.digits;
  if (parts.negative) { return type_prefix + "1:" + HexEncodeLexBytes(material, true); }
  return type_prefix + "3:" + HexEncodeLexBytes(material, false);
}

std::string StripSign(std::string value, bool* negative) {
  *negative = false;
  if (!value.empty() && (value.front() == '-' || value.front() == '+')) {
    *negative = value.front() == '-';
    value.erase(value.begin());
  }
  return value;
}

std::string TrimLeadingZeros(std::string value) {
  std::size_t first = 0;
  while (first + 1 < value.size() && value[first] == '0') { ++first; }
  return value.substr(first);
}

int CompareIntegerText(std::string left, std::string right) {
  bool left_negative = false;
  bool right_negative = false;
  left = TrimLeadingZeros(StripSign(std::move(left), &left_negative));
  right = TrimLeadingZeros(StripSign(std::move(right), &right_negative));
  if (left == "0") { left_negative = false; }
  if (right == "0") { right_negative = false; }
  if (left_negative != right_negative) { return left_negative ? -1 : 1; }
  int magnitude = 0;
  if (left.size() < right.size()) { magnitude = -1; }
  else if (left.size() > right.size()) { magnitude = 1; }
  else if (left < right) { magnitude = -1; }
  else if (left > right) { magnitude = 1; }
  if (left_negative) { magnitude = -magnitude; }
  return magnitude;
}

int CompareLongDoubleText(const std::string& left, const std::string& right) {
  try {
    const long double l = std::stold(left);
    const long double r = std::stold(right);
    if (l < r) { return -1; }
    if (l > r) { return 1; }
    return 0;
  } catch (...) {
    return left.compare(right) < 0 ? -1 : (left == right ? 0 : 1);
  }
}

std::string HexEncode(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

bool HexDecode(const std::string& value, std::string* out) {
  if ((value.size() % 2) != 0) { return false; }
  std::string decoded;
  decoded.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return false; }
    decoded.push_back(static_cast<char>((hi << 4) | lo));
  }
  *out = std::move(decoded);
  return true;
}

std::string FindEnvelopeField(const std::string& envelope, std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::size_t start = 0;
  while (start <= envelope.size()) {
    const std::size_t end = envelope.find('\n', start);
    const std::string_view line = std::string_view(envelope).substr(
        start, end == std::string::npos ? envelope.size() - start : end - start);
    if (line.substr(0, prefix.size()) == prefix) { return std::string(line.substr(prefix.size())); }
    if (end == std::string::npos) { break; }
    start = end + 1;
  }
  return {};
}

std::string SortBody(const DatatypeSortKeyRequest& request) {
  if (request.value.is_null) { return request.null_ordering == DatatypeNullOrdering::nulls_first ? "00" : "ff"; }
  const CanonicalTypeId type_id = request.value.type_id;
  std::string value = request.value.encoded_value;
  if (type_id == CanonicalTypeId::boolean) {
    return value == "true" || value == "1" ? "20:1" : "20:0";
  }
  if (IsIntegerType(type_id)) {
    bool negative = false;
    std::string digits = TrimLeadingZeros(StripSign(std::move(value), &negative));
    if (digits == "0") { negative = false; }
    std::ostringstream out;
    out << (negative ? "30:-:" : "30:+:") << std::setw(3) << std::setfill('0') << digits.size() << ':' << digits;
    return out.str();
  }
  if (IsNumericType(type_id)) { return "31:" + request.value.encoded_value; }
  if (type_id == CanonicalTypeId::character &&
      TextSeedAuthorityRequested(request.text_seed, request.case_insensitive_character_compare)) {
    value = NormalizeLatinTextForCollation(value,
                                           request.text_seed.collation_case_insensitive,
                                           request.text_seed.collation_accent_insensitive);
  }
  return std::string(CanonicalTypeName(type_id)) + ':' + HexEncode(value);
}

DatatypeComparisonResult Failure(std::string detail) {
  DatatypeComparisonResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      "SB_DATATYPE_COMPARE_UNSUPPORTED",
                                                      "datatype.compare.unsupported",
                                                      std::move(detail));
  return result;
}

DatatypeComparisonResult TextSeedFailure(std::string code, std::string reason) {
  DatatypeComparisonResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      std::move(code),
                                                      "datatype.compare.text_seed_authority",
                                                      std::move(reason));
  return result;
}

DatatypeSortKeyResult SortKeyFailure(std::string code, std::string reason) {
  DatatypeSortKeyResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      std::move(code),
                                                      "datatype.sort_key.text_seed_authority",
                                                      std::move(reason));
  return result;
}

}  // namespace

const char* DatatypeNullOrderingName(DatatypeNullOrdering null_ordering) {
  switch (null_ordering) {
    case DatatypeNullOrdering::nulls_first: return "nulls_first";
    case DatatypeNullOrdering::nulls_last: return "nulls_last";
  }
  return "nulls_first";
}

DatatypeComparisonResult CompareDatatypeValues(const DatatypeComparisonRequest& request) {
  DatatypeComparisonResult result;
  if (request.left.is_null || request.right.is_null) {
    result.status = OkStatus();
    if (request.left.is_null && request.right.is_null) { result.comparison = 0; }
    else {
      const bool left_first = request.null_ordering == DatatypeNullOrdering::nulls_first;
      result.comparison = request.left.is_null ? (left_first ? -1 : 1) : (left_first ? 1 : -1);
    }
    return result;
  }
  if (request.left.type_id != request.right.type_id) {
    return Failure("type_mismatch:" + std::string(CanonicalTypeName(request.left.type_id)) + ":" +
                   CanonicalTypeName(request.right.type_id));
  }
  const CanonicalTypeId type_id = request.left.type_id;
  if (UnsupportedRuntimeType(type_id)) { return Failure("unsupported_type:" + std::string(CanonicalTypeName(type_id))); }
  if (type_id == CanonicalTypeId::character &&
      TextSeedAuthorityRequested(request.text_seed, request.case_insensitive_character_compare)) {
    if (!TextSeedAuthorityReady(request.text_seed)) {
      return TextSeedFailure("SB_DATATYPE_TEXT_SEED_AUTHORITY_REQUIRED",
                             "character comparison requires durable charset/collation seed authority:" +
                                 TextSeedAuthorityDetail(request.text_seed));
    }
    if (request.case_insensitive_character_compare && !request.text_seed.collation_case_insensitive) {
      return TextSeedFailure("SB_DATATYPE_TEXT_COLLATION_MODE_MISMATCH",
                             "case-insensitive comparison requested without a case-insensitive seed collation:" +
                                 TextSeedAuthorityDetail(request.text_seed));
    }
  }
  result.status = OkStatus();
  if (type_id == CanonicalTypeId::boolean) {
    const bool l = request.left.encoded_value == "true" || request.left.encoded_value == "1";
    const bool r = request.right.encoded_value == "true" || request.right.encoded_value == "1";
    result.comparison = l == r ? 0 : (l ? 1 : -1);
    return result;
  }
  numeric::NumericType runtime_numeric_type = numeric::NumericType::decimal;
  if (RuntimeNumericType(type_id, &runtime_numeric_type)) {
    std::string detail;
    if (!CompareRuntimeNumeric(request.left, request.right, &result.comparison, &detail)) {
      return NumericCompareFailure(detail);
    }
    return result;
  }
  if (IsIntegerType(type_id)) {
    result.comparison = CompareIntegerText(request.left.encoded_value, request.right.encoded_value);
    return result;
  }
  if (IsNumericType(type_id)) {
    result.comparison = CompareLongDoubleText(request.left.encoded_value, request.right.encoded_value);
    return result;
  }
  std::string left = request.left.encoded_value;
  std::string right = request.right.encoded_value;
  if (type_id == CanonicalTypeId::character && request.case_insensitive_character_compare) {
    left = NormalizeLatinTextForCollation(left,
                                          request.text_seed.collation_case_insensitive,
                                          request.text_seed.collation_accent_insensitive);
    right = NormalizeLatinTextForCollation(right,
                                           request.text_seed.collation_case_insensitive,
                                           request.text_seed.collation_accent_insensitive);
  } else if (type_id == CanonicalTypeId::character && request.text_seed.active) {
    left = NormalizeLatinTextForCollation(left,
                                          request.text_seed.collation_case_insensitive,
                                          request.text_seed.collation_accent_insensitive);
    right = NormalizeLatinTextForCollation(right,
                                           request.text_seed.collation_case_insensitive,
                                           request.text_seed.collation_accent_insensitive);
  }
  if (type_id == CanonicalTypeId::character || type_id == CanonicalTypeId::binary ||
      type_id == CanonicalTypeId::blob || IsStableLexicalType(type_id) ||
      type_id == CanonicalTypeId::document || type_id == CanonicalTypeId::json_document ||
      type_id == CanonicalTypeId::xml_document || type_id == CanonicalTypeId::set_value ||
      type_id == CanonicalTypeId::array || type_id == CanonicalTypeId::list ||
      type_id == CanonicalTypeId::map || type_id == CanonicalTypeId::row ||
      type_id == CanonicalTypeId::composite || type_id == CanonicalTypeId::variant) {
    result.comparison = left < right ? -1 : (left == right ? 0 : 1);
    return result;
  }
  return Failure("comparison_not_defined:" + std::string(CanonicalTypeName(type_id)));
}

DatatypeSortKeyResult MakeDatatypeSortKey(const DatatypeSortKeyRequest& request) {
  DatatypeSortKeyResult result;
  if (UnsupportedRuntimeType(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_SORT_KEY_UNSUPPORTED",
                                                        "datatype.sort_key.unsupported",
                                                        CanonicalTypeName(request.value.type_id));
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::character &&
      TextSeedAuthorityRequested(request.text_seed, request.case_insensitive_character_compare)) {
    if (!TextSeedAuthorityReady(request.text_seed)) {
      return SortKeyFailure("SB_DATATYPE_SORT_KEY_TEXT_SEED_AUTHORITY_REQUIRED",
                            "character sort key requires durable charset/collation seed authority:" +
                                TextSeedAuthorityDetail(request.text_seed));
    }
    if (request.case_insensitive_character_compare && !request.text_seed.collation_case_insensitive) {
      return SortKeyFailure("SB_DATATYPE_SORT_KEY_COLLATION_MODE_MISMATCH",
                            "case-insensitive sort key requested without a case-insensitive seed collation:" +
                                TextSeedAuthorityDetail(request.text_seed));
    }
  }
  result.status = OkStatus();
  numeric::NumericType runtime_numeric_type = numeric::NumericType::decimal;
  if (!request.value.is_null && RuntimeNumericType(request.value.type_id, &runtime_numeric_type)) {
    std::string detail;
    const auto numeric_sort =
        RuntimeNumericSortBody(request.value.type_id, request.value.encoded_value, &detail);
    if (!numeric_sort.has_value()) {
      return RuntimeSortKeyFailure("SB_DATATYPE_SORT_KEY_NUMERIC_BACKEND_FAILED", detail);
    }
    result.sort_key = *numeric_sort;
    return result;
  }
  result.sort_key = SortBody(request);
  return result;
}

DatatypeHashResult HashDatatypeValue(const DatatypeHashRequest& request) {
  DatatypeHashResult result;
  if (UnsupportedRuntimeType(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_HASH_UNSUPPORTED",
                                                        "datatype.hash.unsupported",
                                                        CanonicalTypeName(request.value.type_id));
    return result;
  }
  std::uint64_t hash = 1469598103934665603ull;
  const auto feed = [&hash](std::string_view data) {
    for (unsigned char c : data) {
      hash ^= c;
      hash *= 1099511628211ull;
    }
  };
  feed(CanonicalTypeName(request.value.type_id));
  feed(request.value.is_null ? "\0null" : "\0value");
  if (!request.value.is_null) {
    numeric::NumericType runtime_numeric_type = numeric::NumericType::decimal;
    if (RuntimeNumericType(request.value.type_id, &runtime_numeric_type)) {
      std::string canonical;
      std::string detail;
      if (!CanonicalRuntimeNumericValue(request.value.type_id,
                                        request.value.encoded_value,
                                        &canonical,
                                        &detail)) {
        return RuntimeHashFailure("SB_DATATYPE_HASH_NUMERIC_BACKEND_FAILED", detail);
      }
      feed(canonical);
    } else {
      feed(request.value.encoded_value);
    }
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  result.status = OkStatus();
  result.stable_hash_hex = out.str();
  return result;
}

DatatypeSerializationResult SerializeDatatypeValue(const DatatypeSerializationRequest& request) {
  DatatypeSerializationResult result;
  if (request.value.type_id == CanonicalTypeId::unknown) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_SERIALIZE_TYPE_UNKNOWN",
                                                        "datatype.serialize.type_unknown");
    return result;
  }
  result.status = OkStatus();
  std::ostringstream out;
  out << "type=" << CanonicalTypeName(request.value.type_id) << "\n";
  out << "null=" << (request.value.is_null ? "true" : "false") << "\n";
  out << "value_hex=" << (request.value.is_null ? std::string{} : HexEncode(request.value.encoded_value)) << "\n";
  result.serialized_value = out.str();
  return result;
}

DatatypeDeserializationResult DeserializeDatatypeValue(const DatatypeDeserializationRequest& request) {
  DatatypeDeserializationResult result;
  const std::string type_name = FindEnvelopeField(request.serialized_value, "type");
  const std::string null_text = FindEnvelopeField(request.serialized_value, "null");
  const std::string value_hex = FindEnvelopeField(request.serialized_value, "value_hex");
  const CanonicalTypeId type_id = CanonicalTypeIdFromStableName(type_name);
  if (type_id == CanonicalTypeId::unknown ||
      (request.expected_type_id != CanonicalTypeId::unknown && request.expected_type_id != type_id) ||
      (null_text != "true" && null_text != "false")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_DESERIALIZE_INVALID",
                                                        "datatype.deserialize.invalid",
                                                        type_name);
    return result;
  }
  result.status = OkStatus();
  result.value.type_id = type_id;
  result.value.is_null = null_text == "true";
  if (!result.value.is_null && !HexDecode(value_hex, &result.value.encoded_value)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_DESERIALIZE_INVALID_HEX",
                                                        "datatype.deserialize.invalid_hex",
                                                        type_name);
  }
  return result;
}

}  // namespace scratchbird::core::datatypes
