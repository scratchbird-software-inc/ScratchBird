// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  request.right.type_id = backend_type;
  request.right.encoded_value = EncodedForDatatypeBackend(right);
  request.right.is_null = right.is_null;
  request.context.precision = 38;
  request.context.scale = 0;
  request.context.rounding = datatypes::DatatypeRoundingMode::half_even;
  request.context.allow_special_values = true;
  const auto result = datatypes::ApplyNumericOperation(request);
  if (!result.ok()) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     result.diagnostic.diagnostic_code.empty()
                                         ? "SBLR.NUMERIC_OPERATION_FAILED"
                                         : result.diagnostic.diagnostic_code,
                                     "datatype numeric backend rejected the operation");
  }
  if (result.value.is_null) {
    return ScalarResult(operator_id, NullValue(DescriptorNameForNumericBackendType(backend_type)));
  }
  return ScalarResult(operator_id,
                      EncodedNumericValue(DescriptorNameForNumericBackendType(backend_type),
                                          result.value.encoded_value));
}

struct NumericComparisonBackendResult {
  bool attempted = false;
  bool ok = false;
  int comparison = 0;
  SblrResult failure;
};

NumericComparisonBackendResult CompareDatatypeNumericValues(std::string_view operator_id,
                                                            const SblrValue& left,
                                                            const SblrValue& right,
                                                            const SblrExecutionContext& context) {
  NumericComparisonBackendResult out;
  if (!NeedsDatatypeNumericBackend(left, right)) return out;
  out.attempted = true;
  const auto backend_type = NumericBackendType(left, right);
  datatypes::DatatypeNumericOperationRequest request;
  request.operation = datatypes::DatatypeNumericOperationKind::compare;
  request.type_id = backend_type;
  request.left.type_id = backend_type;
  request.left.encoded_value = EncodedForDatatypeBackend(left);
  request.left.is_null = left.is_null;
  request.right.type_id = backend_type;
  request.right.encoded_value = EncodedForDatatypeBackend(right);
  request.right.is_null = right.is_null;
  request.context.precision = 38;
  request.context.scale = 0;
  request.context.rounding = datatypes::DatatypeRoundingMode::half_even;
  request.context.allow_special_values = true;
  const auto result = datatypes::ApplyNumericOperation(request);
  if (!result.ok()) {
    out.failure = OperatorDiagnosticFailure(operator_id,
                                            context,
                                            result.diagnostic.diagnostic_code.empty()
                                                ? "SBLR.NUMERIC_COMPARE_FAILED"
                                                : result.diagnostic.diagnostic_code,
                                            "datatype numeric backend rejected the comparison");
    return out;
  }
  out.ok = true;
  out.comparison = result.comparison;
  return out;
}

bool HasTextSeedMetadata(const SblrValue& value) {
  return !value.charset_name.empty() || !value.collation_name.empty();
}

bool CollationIsCaseInsensitive(std::string_view collation_name) {
  return collation_name.find("_ci") != std::string_view::npos ||
         collation_name.find("case_insensitive") != std::string_view::npos ||
         collation_name.find("nocase") != std::string_view::npos;
}

datatypes::DatatypeTextSeedAuthority TextSeedFromValues(const SblrValue& left, const SblrValue& right) {
  datatypes::DatatypeTextSeedAuthority seed;
  seed.active = HasTextSeedMetadata(left) || HasTextSeedMetadata(right);
  seed.seed_pack_name = "sblr_value_payload";
  seed.seed_pack_version = "runtime_context";
  seed.charset_name = !left.charset_name.empty() ? left.charset_name : right.charset_name;
  seed.collation_name = !left.collation_name.empty() ? left.collation_name : right.collation_name;
  seed.collation_case_insensitive = CollationIsCaseInsensitive(seed.collation_name);
  return seed;
}

NumericComparisonBackendResult CompareDatatypeCharacterValues(std::string_view operator_id,
                                                              const SblrValue& left,
                                                              const SblrValue& right,
                                                              const SblrExecutionContext& context) {
  NumericComparisonBackendResult out;
  if (!HasTextSeedMetadata(left) && !HasTextSeedMetadata(right)) return out;
  out.attempted = true;
  datatypes::DatatypeComparisonRequest request;
  request.left.type_id = datatypes::CanonicalTypeId::character;
  request.left.encoded_value = ComparableText(left);
  request.left.is_null = left.is_null;
  request.right.type_id = datatypes::CanonicalTypeId::character;
  request.right.encoded_value = ComparableText(right);
  request.right.is_null = right.is_null;
  request.null_ordering = datatypes::DatatypeNullOrdering::nulls_first;
  request.case_insensitive_character_compare = false;
  request.text_seed = TextSeedFromValues(left, right);
  const auto result = datatypes::CompareDatatypeValues(request);
  if (!result.ok()) {
    out.failure = OperatorDiagnosticFailure(operator_id,
                                            context,
                                            result.diagnostic.diagnostic_code.empty()
                                                ? "SBLR.CHARSET_COLLATION_COMPARE_FAILED"
                                                : result.diagnostic.diagnostic_code,
                                            "datatype character comparison rejected the operation");
    return out;
  }
  out.ok = true;
  out.comparison = result.comparison;
  return out;
}

bool TryLongDouble(const SblrValue& value, long double* out) {
  if (value.has_int64_value) {
    *out = static_cast<long double>(value.int64_value);
    return true;
  }
  if (value.has_uint64_value) {
    *out = static_cast<long double>(value.uint64_value);
    return true;
  }
  if (value.has_real64_value) {
    *out = static_cast<long double>(value.real64_value);
    return true;
  }
  const std::string& text = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (text.empty()) return false;
  try {
    std::size_t parsed = 0;
    const long double parsed_value = std::stold(text, &parsed);
    if (parsed != text.size()) return false;
    *out = parsed_value;
    return true;
  } catch (...) {
    return false;
  }
}

bool TryInt64Text(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  std::size_t index = 0;
  bool negative = false;
  if (text[index] == '+' || text[index] == '-') {
    negative = text[index] == '-';
    ++index;
  }
  if (index == text.size()) return false;
  std::uint64_t magnitude = 0;
  for (; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
    magnitude = magnitude * 10 + digit;
  }
  if (negative) {
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    if (magnitude > limit) return false;
    *out = magnitude == limit ? std::numeric_limits<std::int64_t>::min()
                              : -static_cast<std::int64_t>(magnitude);
    return true;
  }
  if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
  *out = static_cast<std::int64_t>(magnitude);
  return true;
}

bool IsLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && IsLeapYear(year)) return 29;
  return kDays[month - 1];
}

std::optional<int> ParseNDigits(std::string_view text, std::size_t offset, std::size_t count) {
  if (offset + count > text.size()) return std::nullopt;
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char ch = text[offset + i];
    if (ch < '0' || ch > '9') return std::nullopt;
    value = value * 10 + (ch - '0');
  }
  return value;
}

std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void CivilFromDays(std::int64_t z, int* year, unsigned* month, unsigned* day) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
  *year = y;
  *month = m;
  *day = d;
}

std::string TwoDigits(unsigned value) {
  std::ostringstream out;
  out << std::setw(2) << std::setfill('0') << value;
  return out.str();
}

std::string FourDigits(int value) {
  std::ostringstream out;
  out << std::setw(4) << std::setfill('0') << value;
  return out.str();
}

std::string FormatDateFromDays(std::int64_t days) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CivilFromDays(days, &year, &month, &day);
  return FourDigits(year) + "-" + TwoDigits(month) + "-" + TwoDigits(day);
}

std::string FormatTimeFromSeconds(std::int64_t seconds) {
  constexpr std::int64_t kDaySeconds = 86400;
  seconds %= kDaySeconds;
  if (seconds < 0) seconds += kDaySeconds;
  const auto hour = static_cast<unsigned>(seconds / 3600);
  seconds %= 3600;
  const auto minute = static_cast<unsigned>(seconds / 60);
  const auto second = static_cast<unsigned>(seconds % 60);
  return TwoDigits(hour) + ":" + TwoDigits(minute) + ":" + TwoDigits(second);
}

std::string FormatTimestampUtc(std::int64_t epoch_seconds, bool with_timezone) {
  constexpr std::int64_t kDaySeconds = 86400;
