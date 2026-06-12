// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_temporal_wire.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

struct ParsedTemporalWire {
  CanonicalTypeId canonical_type_id = CanonicalTypeId::unknown;
  std::string local_value;
  std::string offset_text;
  std::string zone_name;
  int offset_minutes = 0;
  bool has_offset = false;
  bool has_zone_name = false;
};

struct WireProfilePolicy {
  CanonicalTypeId canonical_type_id = CanonicalTypeId::timestamp;
  bool requires_timezone = false;
  bool forbids_timezone = false;
};

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

std::string TrimAsciiWhitespace(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) { value.erase(value.begin()); }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) { value.pop_back(); }
  return value;
}

bool ParseFixedDigits(const std::string& value, std::size_t pos, std::size_t count, int* out) {
  if (pos + count > value.size()) { return false; }
  int parsed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = value[pos + i];
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    parsed = parsed * 10 + (c - '0');
  }
  *out = parsed;
  return true;
}

bool LeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
  switch (month) {
    case 1: return 31;
    case 2: return LeapYear(year) ? 29 : 28;
    case 3: return 31;
    case 4: return 30;
    case 5: return 31;
    case 6: return 30;
    case 7: return 31;
    case 8: return 31;
    case 9: return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 0;
  }
}

bool DateText(const std::string& value) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') { return false; }
  if (!ParseFixedDigits(value, 0, 4, &year) || !ParseFixedDigits(value, 5, 2, &month) ||
      !ParseFixedDigits(value, 8, 2, &day)) {
    return false;
  }
  return year >= 1 && month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month);
}

bool TimeText(const std::string& value, u32 fractional_second_precision) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (value.size() < 8 || value[2] != ':' || value[5] != ':') { return false; }
  if (!ParseFixedDigits(value, 0, 2, &hour) || !ParseFixedDigits(value, 3, 2, &minute) ||
      !ParseFixedDigits(value, 6, 2, &second)) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) { return false; }
  std::size_t pos = 8;
  if (pos < value.size() && value[pos] == '.') {
    ++pos;
    const std::size_t fraction_start = pos;
    while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) { ++pos; }
    const std::size_t fraction_digits = pos - fraction_start;
    if (fraction_digits == 0 || fraction_digits > fractional_second_precision || fraction_digits > 12) { return false; }
  }
  return pos == value.size();
}

bool ParseOffset(std::string_view suffix, std::string* normalized, int* minutes) {
  if (suffix == "Z" || suffix == "z") {
    *normalized = "Z";
    *minutes = 0;
    return true;
  }
  if (suffix.size() != 6 || (suffix[0] != '+' && suffix[0] != '-') || suffix[3] != ':') { return false; }
  const std::string text(suffix);
  int hour = 0;
  int minute = 0;
  if (!ParseFixedDigits(text, 1, 2, &hour) || !ParseFixedDigits(text, 4, 2, &minute)) { return false; }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) { return false; }
  *normalized = text;
  *minutes = ((hour * 60) + minute) * (suffix[0] == '-' ? -1 : 1);
  return true;
}

bool IsKnownTimezoneName(const TimezoneSeedAuthority& seed, const std::string& zone_name) {
  return std::find(seed.timezone_names.begin(), seed.timezone_names.end(), zone_name) != seed.timezone_names.end();
}

WireProfilePolicy PolicyForProfile(const std::string& profile) {
  const std::string lower = LowerAscii(profile);
  WireProfilePolicy policy;
  if (lower == "date_wire") {
    policy.canonical_type_id = CanonicalTypeId::date;
    policy.forbids_timezone = true;
    return policy;
  }
  if (lower == "time_wire") {
    policy.canonical_type_id = CanonicalTypeId::time;
    policy.forbids_timezone = true;
    return policy;
  }
  if (lower == "time_with_time_zone" || lower == "timetz_wire" || lower == "time_timezone_profile") {
    policy.canonical_type_id = CanonicalTypeId::time;
    policy.requires_timezone = true;
    return policy;
  }
  if (lower == "timestamp_timezone_profile" || lower == "timestamp_with_time_zone" ||
      lower == "timestamptz_wire" || lower == "tds_datetimeoffset_wire" ||
      lower == "oracle_timestamp_tz_wire") {
    policy.canonical_type_id = CanonicalTypeId::timestamp;
    policy.requires_timezone = true;
    return policy;
  }
  if (lower == "timestamp_wire" || lower == "datetime_wire_profile" || lower == "firebird_temporal_wire" ||
      lower == "tds_datetime_wire") {
    policy.canonical_type_id = CanonicalTypeId::timestamp;
    policy.forbids_timezone = true;
    return policy;
  }
  policy.canonical_type_id = CanonicalTypeId::unknown;
  return policy;
}

ReferenceTemporalWireProfileResult Failure(std::string detail) {
  ReferenceTemporalWireProfileResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeTemporalWireDiagnostic(result.status,
                                                 "SB_DATATYPE_WIRE_CONVERSION_REJECTED",
                                                 "datatype.temporal_wire.rejected",
                                                 std::move(detail));
  return result;
}

bool SplitNamedZoneSuffix(std::string* value, std::string* zone_name) {
  const std::string text = TrimAsciiWhitespace(*value);
  if (text.size() > 2 && text.back() == ']') {
    const auto open = text.rfind('[');
    if (open != std::string::npos && open > 0) {
      *zone_name = text.substr(open + 1, text.size() - open - 2);
      *value = TrimAsciiWhitespace(text.substr(0, open));
      return true;
    }
  }
  const auto space = text.rfind(' ');
  if (space != std::string::npos && space + 1 < text.size()) {
    const std::string candidate = text.substr(space + 1);
    if (candidate.find('/') != std::string::npos) {
      *zone_name = candidate;
      *value = TrimAsciiWhitespace(text.substr(0, space));
      return true;
    }
  }
  return false;
}

bool SplitOffsetSuffix(std::string* value, std::string* offset_text, int* offset_minutes) {
  const std::string text = TrimAsciiWhitespace(*value);
  if (!text.empty() && (text.back() == 'Z' || text.back() == 'z')) {
    std::string offset;
    int minutes = 0;
    if (!ParseOffset(std::string_view(text).substr(text.size() - 1), &offset, &minutes)) { return false; }
    *value = text.substr(0, text.size() - 1);
    *offset_text = offset;
    *offset_minutes = minutes;
    return true;
  }
  if (text.size() >= 6) {
    const std::size_t pos = text.size() - 6;
    if ((text[pos] == '+' || text[pos] == '-') && text[pos + 3] == ':') {
      std::string offset;
      int minutes = 0;
      if (!ParseOffset(std::string_view(text).substr(pos), &offset, &minutes)) { return false; }
      *value = text.substr(0, pos);
      *offset_text = offset;
      *offset_minutes = minutes;
      return true;
    }
  }
  return false;
}

bool ParseTemporalValue(const ReferenceTemporalWireProfileRequest& request,
                        const WireProfilePolicy& policy,
                        ParsedTemporalWire* out) {
  ParsedTemporalWire parsed;
  parsed.canonical_type_id = policy.canonical_type_id;
  std::string value = TrimAsciiWhitespace(request.encoded_value);
  std::string zone_name;
  if (SplitNamedZoneSuffix(&value, &zone_name)) {
    parsed.has_zone_name = true;
    parsed.zone_name = zone_name;
  } else {
    std::string offset_text;
    int offset_minutes = 0;
    const std::string before_offset = value;
    if (SplitOffsetSuffix(&value, &offset_text, &offset_minutes)) {
      parsed.has_offset = true;
      parsed.offset_text = offset_text;
      parsed.offset_minutes = offset_minutes;
    } else {
      value = before_offset;
    }
  }

  if (parsed.has_offset && parsed.has_zone_name) { return false; }
  if (policy.canonical_type_id == CanonicalTypeId::date) {
    if (!DateText(value)) { return false; }
  } else if (policy.canonical_type_id == CanonicalTypeId::time) {
    if (!TimeText(value, request.fractional_second_precision)) { return false; }
  } else if (policy.canonical_type_id == CanonicalTypeId::timestamp) {
    const auto t_pos = value.find('T');
    const auto space_pos = value.find(' ');
    const auto pos = t_pos == std::string::npos ? space_pos : t_pos;
    if (pos == std::string::npos) { return false; }
    const std::string date = value.substr(0, pos);
    const std::string time = value.substr(pos + 1);
    if (!DateText(date) || !TimeText(time, request.fractional_second_precision)) { return false; }
    value = date + "T" + time;
  } else {
    return false;
  }
  parsed.local_value = value;
  *out = parsed;
  return true;
}

std::string NormalizedEnvelope(const ReferenceTemporalWireProfileRequest& request,
                               const ParsedTemporalWire& parsed) {
  std::ostringstream out;
  out << "type=" << CanonicalTypeName(parsed.canonical_type_id)
      << ";local=" << parsed.local_value
      << ";reference=" << request.reference_engine
      << ";reference_type=" << request.reference_type_or_family;
  if (parsed.has_offset) {
    out << ";offset=" << parsed.offset_text << ";offset_minutes=" << parsed.offset_minutes;
  }
  if (parsed.has_zone_name) {
    out << ";zone=" << parsed.zone_name
        << ";timezone_seed=" << request.timezone_seed.seed_pack_name
        << ";timezone_version=" << request.timezone_seed.seed_pack_version;
  }
  return out.str();
}

}  // namespace

ReferenceTemporalWireProfileResult ValidateReferenceTemporalWireProfile(const ReferenceTemporalWireProfileRequest& request) {
  const WireProfilePolicy policy = PolicyForProfile(request.wire_profile);
  if (policy.canonical_type_id == CanonicalTypeId::unknown) { return Failure("temporal_wire_profile_unknown"); }
  if (request.fractional_second_precision > 12) { return Failure("fractional_precision_unsupported"); }
  if (request.require_timezone_seed &&
      (!request.timezone_seed.active || request.timezone_seed.timezone_records == 0 ||
       request.timezone_seed.seed_pack_version.empty())) {
    return Failure("timezone_seed_authority_missing");
  }

  ParsedTemporalWire parsed;
  if (!ParseTemporalValue(request, policy, &parsed)) { return Failure("temporal_wire_value_invalid"); }
  const bool has_timezone = parsed.has_offset || parsed.has_zone_name;
  if (policy.requires_timezone && !has_timezone) { return Failure("timezone_required_by_wire_profile"); }
  if (policy.forbids_timezone && has_timezone) { return Failure("timezone_forbidden_by_wire_profile"); }
  if (parsed.has_zone_name) {
    if (!request.timezone_seed.active || !IsKnownTimezoneName(request.timezone_seed, parsed.zone_name)) {
      return Failure("timezone_name_not_in_seed_catalog:" + parsed.zone_name);
    }
  }

  ReferenceTemporalWireProfileResult result;
  result.status = OkStatus();
  result.canonical_type_id = parsed.canonical_type_id;
  result.normalized_value = NormalizedEnvelope(request, parsed);
  result.timezone_identifier = parsed.has_zone_name ? parsed.zone_name : parsed.offset_text;
  result.timezone_offset_minutes = parsed.offset_minutes;
  result.used_timezone_seed = parsed.has_zone_name;
  result.diagnostic = MakeTemporalWireDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  return result;
}

DiagnosticRecord MakeTemporalWireDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) { arguments.push_back({"detail", std::move(detail)}); }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.temporal_wire");
}

}  // namespace scratchbird::core::datatypes
