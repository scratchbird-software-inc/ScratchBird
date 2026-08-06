// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include "datatype_operations.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::executor {
namespace {

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

constexpr __int128 kPicosecondsPerSecond = 1000000000000LL;
constexpr __int128 kSecondsPerDay = 86400;
constexpr __int128 kPicosecondsPerDay =
    kSecondsPerDay * kPicosecondsPerSecond;

DescriptorRuntimeDiagnostic Refusal(std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-FRAME";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

bool IsNumeric(const dt::CanonicalTypeId type) {
  switch (type) {
    case dt::CanonicalTypeId::int8:
    case dt::CanonicalTypeId::int16:
    case dt::CanonicalTypeId::int32:
    case dt::CanonicalTypeId::int64:
    case dt::CanonicalTypeId::int128:
    case dt::CanonicalTypeId::uint8:
    case dt::CanonicalTypeId::uint16:
    case dt::CanonicalTypeId::uint32:
    case dt::CanonicalTypeId::uint64:
    case dt::CanonicalTypeId::uint128:
    case dt::CanonicalTypeId::bfloat16:
    case dt::CanonicalTypeId::real16:
    case dt::CanonicalTypeId::real32:
    case dt::CanonicalTypeId::real64:
    case dt::CanonicalTypeId::real128:
    case dt::CanonicalTypeId::decimal:
    case dt::CanonicalTypeId::decimal_float:
      return true;
    default:
      return false;
  }
}

bool IsTemporal(const dt::CanonicalTypeId type) {
  return type == dt::CanonicalTypeId::date ||
         type == dt::CanonicalTypeId::time ||
         type == dt::CanonicalTypeId::timestamp;
}

dt::CanonicalTypeId NumericWorkType(const dt::CanonicalTypeId type) {
  switch (type) {
    case dt::CanonicalTypeId::int8:
    case dt::CanonicalTypeId::int16:
    case dt::CanonicalTypeId::int32:
    case dt::CanonicalTypeId::int64:
    case dt::CanonicalTypeId::int128:
      return dt::CanonicalTypeId::int128;
    case dt::CanonicalTypeId::uint8:
    case dt::CanonicalTypeId::uint16:
    case dt::CanonicalTypeId::uint32:
    case dt::CanonicalTypeId::uint64:
    case dt::CanonicalTypeId::uint128:
      return dt::CanonicalTypeId::uint128;
    case dt::CanonicalTypeId::decimal:
      return dt::CanonicalTypeId::decimal;
    case dt::CanonicalTypeId::decimal_float:
      return dt::CanonicalTypeId::decimal_float;
    default:
      return dt::CanonicalTypeId::real128;
  }
}

std::string DescriptorField(const std::string& descriptor,
                            const std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::string result;
  bool found = false;
  std::size_t begin = 0;
  while (begin <= descriptor.size()) {
    const auto end = descriptor.find(';', begin);
    const auto field = descriptor.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (field.rfind(prefix, 0) == 0) {
      if (found) return {};
      result = field.substr(prefix.size());
      found = true;
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

bool ParseU32(const std::string& text, std::uint32_t* value) {
  if (value == nullptr || text.empty() ||
      (text.size() > 1 && text.front() == '0')) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

dt::DatatypeNumericContext NumericContext(const api::EngineDescriptor& descriptor,
                                          const dt::CanonicalTypeId type) {
  dt::DatatypeNumericContext context;
  context.precision = 38;
  context.scale = 0;
  if (type == dt::CanonicalTypeId::decimal ||
      type == dt::CanonicalTypeId::decimal_float) {
    std::uint32_t precision = 0;
    std::uint32_t scale = 0;
    if (ParseU32(DescriptorField(descriptor.encoded_descriptor, "precision"),
                 &precision) &&
        ParseU32(DescriptorField(descriptor.encoded_descriptor, "scale"),
                 &scale) &&
        precision != 0 && precision <= 38 && scale <= precision) {
      context.precision = precision;
      context.scale = scale;
    }
  }
  return context;
}

bool CastNumeric(const api::EngineTypedValue& source,
                 const dt::CanonicalTypeId target_type,
                 dt::DatatypeOperationValue* output) {
  dt::DatatypeCastRequest request;
  request.value.type_id = dt::CanonicalTypeIdFromStableName(
      source.descriptor.canonical_type_name);
  request.value.encoded_value = source.encoded_value;
  request.value.is_null = source.isSqlNull();
  request.target_type_id = target_type;
  request.explicit_cast = true;
  const auto cast = dt::CastDatatypeValue(request);
  if (!cast.ok() || cast.value.is_null) return false;
  *output = cast.value;
  return true;
}

bool ValidateNonnegativeNumeric(const api::EngineTypedValue& value) {
  const auto source_type = dt::CanonicalTypeIdFromStableName(
      value.descriptor.canonical_type_name);
  if (!IsNumeric(source_type) || value.isSqlNull() ||
      value.state != api::EngineValueState::value || !value.binary_value.empty()) {
    return false;
  }
  const auto work_type = NumericWorkType(source_type);
  dt::DatatypeOperationValue cast_value;
  if (!CastNumeric(value, work_type, &cast_value)) return false;
  dt::DatatypeNumericOperationRequest request;
  request.operation = dt::DatatypeNumericOperationKind::compare;
  request.type_id = work_type;
  request.left = cast_value;
  request.right = {work_type, "0", false};
  request.context = NumericContext(value.descriptor, source_type);
  const auto compared = dt::ApplyNumericOperation(request);
  return compared.ok() && compared.comparison >= 0;
}

bool NumericThreshold(const api::EngineTypedValue& current,
                      const api::EngineTypedValue& offset,
                      const bool add,
                      api::EngineTypedValue* threshold) {
  const auto order_type = dt::CanonicalTypeIdFromStableName(
      current.descriptor.canonical_type_name);
  const auto work_type = NumericWorkType(order_type);
  dt::DatatypeOperationValue left;
  dt::DatatypeOperationValue right;
  if (!CastNumeric(current, work_type, &left) ||
      !CastNumeric(offset, work_type, &right)) {
    return false;
  }
  dt::DatatypeNumericOperationRequest operation;
  operation.operation = add ? dt::DatatypeNumericOperationKind::add
                            : dt::DatatypeNumericOperationKind::subtract;
  operation.type_id = work_type;
  operation.left = left;
  operation.right = right;
  operation.context = NumericContext(current.descriptor, order_type);
  const auto calculated = dt::ApplyNumericOperation(operation);
  if (!calculated.ok() || calculated.value.is_null) return false;

  dt::DatatypeCastRequest cast_back;
  cast_back.value = calculated.value;
  cast_back.target_type_id = order_type;
  cast_back.explicit_cast = true;
  const auto cast = dt::CastDatatypeValue(cast_back);
  if (!cast.ok() || cast.value.is_null) return false;
  *threshold = current;
  threshold->encoded_value = cast.value.encoded_value;
  threshold->binary_value.clear();
  threshold->is_null = false;
  threshold->state = api::EngineValueState::value;
  return true;
}

bool ParseFixedDigits(const std::string_view text,
                      const std::size_t begin,
                      const std::size_t count,
                      int* value) {
  if (value == nullptr || begin + count > text.size()) return false;
  int parsed = 0;
  for (std::size_t index = begin; index < begin + count; ++index) {
    if (text[index] < '0' || text[index] > '9') return false;
    parsed = parsed * 10 + (text[index] - '0');
  }
  *value = parsed;
  return true;
}

bool LeapYear(const int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(const int year, const int month) {
  static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  return month == 2 && LeapYear(year) ? 29 : kDays[month];
}

std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

struct TemporalPoint {
  int year = 0;
  int month = 0;
  int day = 0;
  __int128 time_picoseconds = 0;
  int timezone_offset_seconds = 0;
  bool is_time_only = false;
};

bool ParseDatePart(const std::string_view text, TemporalPoint* point) {
  if (point == nullptr || text.size() < 10 || text[4] != '-' ||
      text[7] != '-') {
    return false;
  }
  if (!ParseFixedDigits(text, 0, 4, &point->year) ||
      !ParseFixedDigits(text, 5, 2, &point->month) ||
      !ParseFixedDigits(text, 8, 2, &point->day) || point->year < 1 ||
      point->year > 9999 || point->month < 1 || point->month > 12 ||
      point->day < 1 || point->day > DaysInMonth(point->year, point->month)) {
    return false;
  }
  return true;
}

bool ParseTimePart(const std::string_view text,
                   const std::size_t begin,
                   TemporalPoint* point) {
  if (point == nullptr || begin + 8 > text.size() ||
      text[begin + 2] != ':' || text[begin + 5] != ':') {
    return false;
  }
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!ParseFixedDigits(text, begin, 2, &hour) ||
      !ParseFixedDigits(text, begin + 3, 2, &minute) ||
      !ParseFixedDigits(text, begin + 6, 2, &second) || hour > 23 ||
      minute > 59 || second > 59) {
    return false;
  }
  std::size_t index = begin + 8;
  __int128 fraction = 0;
  std::size_t fraction_digits = 0;
  if (index < text.size() && text[index] == '.') {
    ++index;
    while (index < text.size() && std::isdigit(
                                     static_cast<unsigned char>(text[index]))) {
      if (fraction_digits == 12) return false;
      fraction = fraction * 10 + (text[index++] - '0');
      ++fraction_digits;
    }
    if (fraction_digits == 0) return false;
  }
  while (fraction_digits++ < 12) fraction *= 10;

  int timezone_offset = 0;
  if (index < text.size()) {
    if ((text[index] == 'Z' || text[index] == 'z') &&
        index + 1 == text.size()) {
      ++index;
    } else if ((text[index] == '+' || text[index] == '-') &&
               index + 6 == text.size() && text[index + 3] == ':') {
      int zone_hour = 0;
      int zone_minute = 0;
      if (!ParseFixedDigits(text, index + 1, 2, &zone_hour) ||
          !ParseFixedDigits(text, index + 4, 2, &zone_minute) ||
          zone_hour > 23 || zone_minute > 59) {
        return false;
      }
      timezone_offset = zone_hour * 3600 + zone_minute * 60;
      if (text[index] == '-') timezone_offset = -timezone_offset;
      index += 6;
    } else {
      return false;
    }
  }
  if (index != text.size()) return false;
  point->time_picoseconds =
      (static_cast<__int128>(hour) * 3600 + minute * 60 + second) *
          kPicosecondsPerSecond +
      fraction;
  point->timezone_offset_seconds = timezone_offset;
  return true;
}

bool ParseTemporalPoint(const api::EngineTypedValue& value,
                        TemporalPoint* point) {
  if (point == nullptr || value.isSqlNull() ||
      value.state != api::EngineValueState::value || !value.binary_value.empty()) {
    return false;
  }
  const auto type = dt::CanonicalTypeIdFromStableName(
      value.descriptor.canonical_type_name);
  const auto& text = value.encoded_value;
  *point = {};
  if (type == dt::CanonicalTypeId::date) {
    return text.size() == 10 && ParseDatePart(text, point);
  }
  if (type == dt::CanonicalTypeId::time) {
    point->year = 1970;
    point->month = 1;
    point->day = 1;
    point->is_time_only = true;
    return ParseTimePart(text, 0, point);
  }
  if (type != dt::CanonicalTypeId::timestamp || text.size() < 19 ||
      (text[10] != 'T' && text[10] != ' ')) {
    return false;
  }
  return ParseDatePart(text, point) && ParseTimePart(text, 11, point);
}

__int128 TemporalOrdinal(const TemporalPoint& point) {
  return static_cast<__int128>(
             DaysFromCivil(point.year, static_cast<unsigned>(point.month),
                           static_cast<unsigned>(point.day))) *
             kPicosecondsPerDay +
         point.time_picoseconds -
         static_cast<__int128>(point.timezone_offset_seconds) *
             kPicosecondsPerSecond;
}

struct TemporalInterval {
  std::int64_t months = 0;
  __int128 picoseconds = 0;
};

bool AccumulateIntervalComponent(const std::uint64_t amount,
                                 const __int128 multiplier,
                                 __int128* total) {
  if (total == nullptr) return false;
  const __int128 add = static_cast<__int128>(amount) * multiplier;
  if (add < 0 || *total > std::numeric_limits<__int128>::max() - add) {
    return false;
  }
  *total += add;
  return true;
}

bool ParseTemporalInterval(const api::EngineTypedValue& value,
                           TemporalInterval* interval) {
  if (interval == nullptr || value.isSqlNull() ||
      value.state != api::EngineValueState::value || !value.binary_value.empty() ||
      dt::CanonicalTypeIdFromStableName(value.descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::interval) {
    return false;
  }
  *interval = {};
  const auto& text = value.encoded_value;
  if (text.empty() || text.front() == '-') return false;
  if (text.front() != 'P') {
    std::int64_t seconds = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() ||
        seconds < 0) {
      return false;
    }
    interval->picoseconds =
        static_cast<__int128>(seconds) * kPicosecondsPerSecond;
    return true;
  }

  bool in_time = false;
  bool saw_time_component = false;
  bool saw_component = false;
  std::size_t index = 1;
  while (index < text.size()) {
    if (text[index] == 'T') {
      if (in_time) return false;
      in_time = true;
      ++index;
      continue;
    }
    const auto number_begin = index;
    while (index < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[index]))) {
      ++index;
    }
    if (number_begin == index) return false;
    std::uint64_t amount = 0;
    const auto [amount_end, amount_error] = std::from_chars(
        text.data() + number_begin, text.data() + index, amount);
    if (amount_error != std::errc{} ||
        amount_end != text.data() + index) {
      return false;
    }
    __int128 fraction = 0;
    std::size_t fraction_digits = 0;
    if (index < text.size() && text[index] == '.') {
      if (!in_time) return false;
      ++index;
      while (index < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[index]))) {
        if (fraction_digits == 12) return false;
        fraction = fraction * 10 + (text[index++] - '0');
        ++fraction_digits;
      }
      if (fraction_digits == 0) return false;
      while (fraction_digits++ < 12) fraction *= 10;
    }
    if (index >= text.size()) return false;
    const char designator = text[index++];
    if (!in_time && designator == 'Y') {
      if (amount > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max() / 12) ||
          interval->months >
              std::numeric_limits<std::int64_t>::max() -
                  static_cast<std::int64_t>(amount * 12)) {
        return false;
      }
      interval->months += static_cast<std::int64_t>(amount * 12);
    } else if (!in_time && designator == 'M') {
      if (amount > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()) ||
          interval->months >
              std::numeric_limits<std::int64_t>::max() -
                  static_cast<std::int64_t>(amount)) {
        return false;
      }
      interval->months += static_cast<std::int64_t>(amount);
    } else if (!in_time && designator == 'W') {
      if (!AccumulateIntervalComponent(amount, 7 * kPicosecondsPerDay,
                                       &interval->picoseconds)) {
        return false;
      }
    } else if (!in_time && designator == 'D') {
      if (!AccumulateIntervalComponent(amount, kPicosecondsPerDay,
                                       &interval->picoseconds)) {
        return false;
      }
    } else if (in_time && designator == 'H' && fraction == 0) {
      if (!AccumulateIntervalComponent(
              amount, 3600 * kPicosecondsPerSecond,
              &interval->picoseconds)) {
        return false;
      }
      saw_time_component = true;
    } else if (in_time && designator == 'M' && fraction == 0) {
      if (!AccumulateIntervalComponent(
              amount, 60 * kPicosecondsPerSecond,
              &interval->picoseconds)) {
        return false;
      }
      saw_time_component = true;
    } else if (in_time && designator == 'S') {
      if (!AccumulateIntervalComponent(amount, kPicosecondsPerSecond,
                                       &interval->picoseconds) ||
          interval->picoseconds >
              std::numeric_limits<__int128>::max() - fraction) {
        return false;
      }
      interval->picoseconds += fraction;
      saw_time_component = true;
    } else {
      return false;
    }
    saw_component = true;
  }
  return saw_component && (!in_time || saw_time_component);
}

bool TemporalThreshold(const api::EngineTypedValue& current,
                       const TemporalInterval& offset,
                       const bool add,
                       __int128* threshold) {
  TemporalPoint point;
  if (!ParseTemporalPoint(current, &point) || threshold == nullptr) return false;
  if (point.is_time_only && offset.months != 0) return false;

  const __int128 signed_months =
      add ? static_cast<__int128>(offset.months)
          : -static_cast<__int128>(offset.months);
  const __int128 absolute_month =
      static_cast<__int128>(point.year) * 12 + (point.month - 1) +
      signed_months;
  if (absolute_month < 12 || absolute_month > 9999 * 12 + 11) return false;
  point.year = static_cast<int>(absolute_month / 12);
  point.month = static_cast<int>(absolute_month % 12) + 1;
  point.day = std::min(point.day, DaysInMonth(point.year, point.month));

  const auto base = TemporalOrdinal(point);
  if ((!add && base < std::numeric_limits<__int128>::min() +
                          offset.picoseconds) ||
      (add && base > std::numeric_limits<__int128>::max() -
                         offset.picoseconds)) {
    return false;
  }
  *threshold = add ? base + offset.picoseconds
                   : base - offset.picoseconds;
  if (point.is_time_only && (*threshold < 0 || *threshold >= kPicosecondsPerDay)) {
    return false;
  }
  return true;
}

bool MetadataIsCanonical(const CanonicalWindowPartitionOrderResult& input) {
  const auto row_count = input.ordered_batch.rows.size();
  if (!input.diagnostic.ok || !input.explicit_peer_metadata ||
      !input.stable_ties_preserved ||
      !input.weaker_peer_recomputation_forbidden ||
      input.final_query_order_guaranteed ||
      !input.authority.engine_mga_snapshot_bound ||
      !PhysicalMgaStatementContextValid(input.mga_statement_context) ||
      !IsCanonicalUuid(input.window_property_uuid) ||
      (input.partition_terms.empty() != input.partition_property_uuid.empty()) ||
      (!input.partition_terms.empty() &&
       !IsCanonicalUuid(input.partition_property_uuid)) ||
      (input.order_terms.empty() != input.ordering_property_uuid.empty()) ||
      (!input.order_terms.empty() &&
       !IsCanonicalUuid(input.ordering_property_uuid)) ||
      ((!input.partition_terms.empty() || !input.order_terms.empty()) !=
       !input.term_binding_evidence_uuid.empty()) ||
      ((!input.partition_terms.empty() || !input.order_terms.empty()) &&
       !IsCanonicalUuid(input.term_binding_evidence_uuid)) ||
      !IsCanonicalUuid(input.deterministic_tie_evidence_uuid) ||
      !IsCanonicalUuid(input.selected_plan_uuid) ||
      input.executed_physical_node_id == 0 || input.causal_counter_id == 0 ||
      input.row_metadata.size() != row_count) {
    return false;
  }
  for (std::size_t row = 0; row < row_count; ++row) {
    const auto& metadata = input.row_metadata[row];
    if (metadata.ordered_row_index != row ||
        !metadata.partition_id.has_value() ||
        !metadata.peer_group_id.has_value() ||
        metadata.partition_begin > row ||
        metadata.partition_end_exclusive <= row ||
        metadata.partition_end_exclusive > row_count ||
        metadata.peer_begin > row || metadata.peer_end_exclusive <= row ||
        metadata.peer_begin < metadata.partition_begin ||
        metadata.peer_end_exclusive > metadata.partition_end_exclusive) {
      return false;
    }
    for (std::size_t peer = metadata.peer_begin;
         peer < metadata.peer_end_exclusive; ++peer) {
      const auto& candidate = input.row_metadata[peer];
      if (candidate.partition_id != metadata.partition_id ||
          candidate.peer_group_id != metadata.peer_group_id ||
          candidate.peer_begin != metadata.peer_begin ||
          candidate.peer_end_exclusive != metadata.peer_end_exclusive) {
        return false;
      }
    }
    for (std::size_t member = metadata.partition_begin;
         member < metadata.partition_end_exclusive; ++member) {
      const auto& candidate = input.row_metadata[member];
      if (candidate.partition_id != metadata.partition_id ||
          candidate.partition_begin != metadata.partition_begin ||
          candidate.partition_end_exclusive !=
              metadata.partition_end_exclusive) {
        return false;
      }
    }
  }
  std::size_t observed_partitions = 0;
  std::size_t observed_peers = 0;
  std::size_t row = 0;
  while (row < row_count) {
    const auto partition_end = input.row_metadata[row].partition_end_exclusive;
    ++observed_partitions;
    while (row < partition_end) {
      row = input.row_metadata[row].peer_end_exclusive;
      ++observed_peers;
    }
  }
  if (observed_partitions != input.partition_count ||
      observed_peers != input.peer_group_count) {
    return false;
  }
  return true;
}

bool IsOffsetBound(const CanonicalWindowFrameBoundKind kind) {
  return kind == CanonicalWindowFrameBoundKind::offset_preceding ||
         kind == CanonicalWindowFrameBoundKind::offset_following;
}

bool ValidBoundKind(const CanonicalWindowFrameBoundKind kind) {
  return kind == CanonicalWindowFrameBoundKind::unbounded_preceding ||
         kind == CanonicalWindowFrameBoundKind::offset_preceding ||
         kind == CanonicalWindowFrameBoundKind::current_row ||
         kind == CanonicalWindowFrameBoundKind::offset_following ||
         kind == CanonicalWindowFrameBoundKind::unbounded_following;
}

bool ValidUnit(const CanonicalWindowFrameUnit unit) {
  return unit == CanonicalWindowFrameUnit::rows ||
         unit == CanonicalWindowFrameUnit::range ||
         unit == CanonicalWindowFrameUnit::groups;
}

bool ValidExclusion(const CanonicalWindowFrameExclusion exclusion) {
  return exclusion == CanonicalWindowFrameExclusion::no_others ||
         exclusion == CanonicalWindowFrameExclusion::current_row ||
         exclusion == CanonicalWindowFrameExclusion::group ||
         exclusion == CanonicalWindowFrameExclusion::ties;
}

bool ResolveFrame(const CanonicalWindowFrameRequest& request,
                  CanonicalWindowFrameDescriptor* resolved,
                  bool* defaulted_with_order,
                  bool* defaulted_without_order,
                  std::string* detail) {
  if (resolved == nullptr || defaulted_with_order == nullptr ||
      defaulted_without_order == nullptr || detail == nullptr) {
    return false;
  }
  *resolved = request.frame;
  *defaulted_with_order = false;
  *defaulted_without_order = false;
  if (!IsCanonicalUuid(resolved->frame_descriptor_uuid)) {
    *detail = "frame descriptor UUID is missing or malformed";
    return false;
  }
  if (!resolved->frame_specified) {
    if (resolved->start.has_value() || resolved->end.has_value() ||
        resolved->unit != CanonicalWindowFrameUnit::rows ||
        resolved->exclusion != CanonicalWindowFrameExclusion::no_others) {
      *detail = "omitted frame carries contradictory explicit state";
      return false;
    }
    CanonicalWindowFrameBound start;
    start.kind = CanonicalWindowFrameBoundKind::unbounded_preceding;
    CanonicalWindowFrameBound end;
    if (request.partition_order.order_terms.empty()) {
      resolved->unit = CanonicalWindowFrameUnit::rows;
      end.kind = CanonicalWindowFrameBoundKind::unbounded_following;
      *defaulted_without_order = true;
    } else {
      resolved->unit = CanonicalWindowFrameUnit::range;
      end.kind = CanonicalWindowFrameBoundKind::current_row;
      *defaulted_with_order = true;
    }
    resolved->start = std::move(start);
    resolved->end = std::move(end);
    return true;
  }
  if (!ValidUnit(resolved->unit) || !ValidExclusion(resolved->exclusion) ||
      !resolved->start.has_value() || !resolved->end.has_value()) {
    *detail = "explicit frame unit, bounds, or exclusion is invalid";
    return false;
  }
  if (resolved->start->kind ==
          CanonicalWindowFrameBoundKind::unbounded_following ||
      resolved->end->kind ==
          CanonicalWindowFrameBoundKind::unbounded_preceding) {
    *detail = "unbounded frame bound is illegal in this bound position";
    return false;
  }
  for (const auto* bound : {&*resolved->start, &*resolved->end}) {
    if (!ValidBoundKind(bound->kind)) {
      *detail = "frame bound kind is invalid";
      return false;
    }
    if (IsOffsetBound(bound->kind) != bound->offset.has_value()) {
      *detail = "frame offset presence does not match its bound kind";
      return false;
    }
    if (!IsOffsetBound(bound->kind)) continue;
    const auto& offset = *bound->offset;
    if (resolved->unit == CanonicalWindowFrameUnit::rows ||
        resolved->unit == CanonicalWindowFrameUnit::groups) {
      const auto decoded = DecodeInt64Value(offset);
      if (!decoded.ok() || offset.state != api::EngineValueState::value ||
          !offset.binary_value.empty() || decoded.value < 0) {
        *detail = "ROWS and GROUPS offsets require a non-negative int64";
        return false;
      }
    } else {
      if (request.partition_order.order_terms.size() != 1) {
        *detail = "RANGE offset requires exactly one typed order term";
        return false;
      }
      const auto order_type = dt::CanonicalTypeIdFromStableName(
          request.partition_order
              .ordered_batch
              .columns[request.partition_order.order_terms.front().column]
              .descriptor.canonical_type_name);
      if (IsNumeric(order_type)) {
        if (!ValidateNonnegativeNumeric(offset)) {
          *detail = "numeric RANGE offset is NULL, negative, or incompatible";
          return false;
        }
      } else if (IsTemporal(order_type)) {
        TemporalInterval interval;
        if (!ParseTemporalInterval(offset, &interval)) {
          *detail = "temporal RANGE offset requires a non-negative interval";
          return false;
        }
      } else {
        *detail = "RANGE offset order type is not numeric or temporal";
        return false;
      }
    }
  }
  return true;
}

struct BaseBounds {
  CanonicalWindowFrameState state = CanonicalWindowFrameState::empty;
  std::size_t begin = 0;
  std::size_t end = 0;
};

std::size_t ClampBoundary(const __int128 value,
                          const std::size_t begin,
                          const std::size_t end) {
  if (value <= static_cast<__int128>(begin)) return begin;
  if (value >= static_cast<__int128>(end)) return end;
  return static_cast<std::size_t>(value);
}

__int128 RowBoundary(const CanonicalWindowFrameBound& bound,
                     const bool start,
                     const std::size_t current,
                     const std::size_t partition_begin,
                     const std::size_t partition_end) {
  switch (bound.kind) {
    case CanonicalWindowFrameBoundKind::unbounded_preceding:
      return std::numeric_limits<__int128>::min();
    case CanonicalWindowFrameBoundKind::unbounded_following:
      return std::numeric_limits<__int128>::max();
    case CanonicalWindowFrameBoundKind::current_row:
      return static_cast<__int128>(current) + (start ? 0 : 1);
    case CanonicalWindowFrameBoundKind::offset_preceding: {
      const auto offset = DecodeInt64Value(*bound.offset).value;
      return static_cast<__int128>(current) - offset + (start ? 0 : 1);
    }
    case CanonicalWindowFrameBoundKind::offset_following: {
      const auto offset = DecodeInt64Value(*bound.offset).value;
      return static_cast<__int128>(current) + offset + (start ? 0 : 1);
    }
  }
  return partition_begin;
}

BaseBounds RowsBounds(const CanonicalWindowFrameDescriptor& frame,
                      const std::size_t current,
                      const CanonicalWindowRowPeerMetadata& metadata) {
  const auto raw_begin = RowBoundary(*frame.start, true, current,
                                     metadata.partition_begin,
                                     metadata.partition_end_exclusive);
  const auto raw_end = RowBoundary(*frame.end, false, current,
                                   metadata.partition_begin,
                                   metadata.partition_end_exclusive);
  BaseBounds bounds;
  bounds.begin = ClampBoundary(raw_begin, metadata.partition_begin,
                               metadata.partition_end_exclusive);
  bounds.end = ClampBoundary(raw_end, metadata.partition_begin,
                             metadata.partition_end_exclusive);
  if (raw_begin > raw_end) {
    bounds.state = CanonicalWindowFrameState::reversed_to_empty;
  } else if (bounds.begin >= bounds.end) {
    bounds.state = CanonicalWindowFrameState::empty;
  } else {
    bounds.state = CanonicalWindowFrameState::nonempty;
  }
  return bounds;
}

std::vector<std::pair<std::size_t, std::size_t>> PeerRanges(
    const CanonicalWindowPartitionOrderResult& input,
    const CanonicalWindowRowPeerMetadata& metadata) {
  std::vector<std::pair<std::size_t, std::size_t>> groups;
  std::size_t row = metadata.partition_begin;
  while (row < metadata.partition_end_exclusive) {
    const auto& peer = input.row_metadata[row];
    groups.emplace_back(peer.peer_begin, peer.peer_end_exclusive);
    row = peer.peer_end_exclusive;
  }
  return groups;
}

__int128 GroupBoundary(const CanonicalWindowFrameBound& bound,
                       const bool start,
                       const std::size_t current_group,
                       const std::size_t group_count) {
  switch (bound.kind) {
    case CanonicalWindowFrameBoundKind::unbounded_preceding:
      return std::numeric_limits<__int128>::min();
    case CanonicalWindowFrameBoundKind::unbounded_following:
      return std::numeric_limits<__int128>::max();
    case CanonicalWindowFrameBoundKind::current_row:
      return static_cast<__int128>(current_group) + (start ? 0 : 1);
    case CanonicalWindowFrameBoundKind::offset_preceding: {
      const auto offset = DecodeInt64Value(*bound.offset).value;
      return static_cast<__int128>(current_group) - offset + (start ? 0 : 1);
    }
    case CanonicalWindowFrameBoundKind::offset_following: {
      const auto offset = DecodeInt64Value(*bound.offset).value;
      return static_cast<__int128>(current_group) + offset + (start ? 0 : 1);
    }
  }
  return 0;
}

BaseBounds GroupsBounds(const CanonicalWindowPartitionOrderResult& input,
                        const CanonicalWindowFrameDescriptor& frame,
                        const CanonicalWindowRowPeerMetadata& metadata) {
  const auto groups = PeerRanges(input, metadata);
  std::size_t current_group = 0;
  while (current_group < groups.size() &&
         groups[current_group].first != metadata.peer_begin) {
    ++current_group;
  }
  const auto raw_begin =
      GroupBoundary(*frame.start, true, current_group, groups.size());
  const auto raw_end =
      GroupBoundary(*frame.end, false, current_group, groups.size());
  const auto begin_group = ClampBoundary(raw_begin, 0, groups.size());
  const auto end_group = ClampBoundary(raw_end, 0, groups.size());
  BaseBounds bounds;
  bounds.begin = begin_group == groups.size()
                     ? metadata.partition_end_exclusive
                     : groups[begin_group].first;
  bounds.end = end_group == groups.size()
                   ? metadata.partition_end_exclusive
                   : groups[end_group].first;
  if (raw_begin > raw_end) {
    bounds.state = CanonicalWindowFrameState::reversed_to_empty;
  } else if (bounds.begin >= bounds.end) {
    bounds.state = CanonicalWindowFrameState::empty;
  } else {
    bounds.state = CanonicalWindowFrameState::nonempty;
  }
  return bounds;
}

bool RangeThreshold(const CanonicalWindowPartitionOrderResult& input,
                    const CanonicalWindowFrameBound& bound,
                    const std::size_t current,
                    api::EngineTypedValue* numeric_threshold,
                    __int128* temporal_threshold,
                    bool* temporal,
                    std::string* detail) {
  const auto& term = input.order_terms.front();
  const auto& current_value =
      input.ordered_batch.rows[current].values[term.column];
  const auto type = dt::CanonicalTypeIdFromStableName(
      current_value.descriptor.canonical_type_name);
  const bool preceding =
      bound.kind == CanonicalWindowFrameBoundKind::offset_preceding;
  bool add = !preceding;
  if (term.direction == CanonicalDescriptorOrderDirection::descending) {
    add = !add;
  }
  *temporal = IsTemporal(type);
  if (IsNumeric(type)) {
    if (!NumericThreshold(current_value, *bound.offset, add,
                          numeric_threshold)) {
      *detail = "numeric RANGE threshold overflowed or is incompatible";
      return false;
    }
    return true;
  }
  TemporalInterval interval;
  if (!ParseTemporalInterval(*bound.offset, &interval) ||
      !TemporalThreshold(current_value, interval, add, temporal_threshold)) {
    *detail = "temporal RANGE threshold overflowed or is incompatible";
    return false;
  }
  return true;
}

bool CompareRangeRowToThreshold(
    const CanonicalWindowPartitionOrderResult& input,
    const std::size_t row,
    const api::EngineTypedValue& numeric_threshold,
    const __int128 temporal_threshold,
    const bool temporal,
    int* comparison,
    std::string* detail) {
  const auto& term = input.order_terms.front();
  const auto& value = input.ordered_batch.rows[row].values[term.column];
  if (temporal) {
    TemporalPoint point;
    if (!ParseTemporalPoint(value, &point)) {
      *detail = "temporal RANGE order value is invalid";
      return false;
    }
    const auto ordinal = TemporalOrdinal(point);
    *comparison = ordinal < temporal_threshold
                      ? -1
                      : (ordinal > temporal_threshold ? 1 : 0);
    if (term.direction == CanonicalDescriptorOrderDirection::descending) {
      *comparison = -*comparison;
    }
    return true;
  }
  const auto compared = CompareCanonicalDescriptorOrderValues(
      value, numeric_threshold, term);
  if (!compared.diagnostic.ok) {
    *detail = compared.diagnostic.detail;
    return false;
  }
  *comparison = compared.comparison;
  return true;
}

bool RangeBoundary(const CanonicalWindowPartitionOrderResult& input,
                   const CanonicalWindowFrameBound& bound,
                   const bool start,
                   const std::size_t current,
                   const CanonicalWindowRowPeerMetadata& metadata,
                   std::size_t* boundary,
                   std::string* detail) {
  switch (bound.kind) {
    case CanonicalWindowFrameBoundKind::unbounded_preceding:
      *boundary = metadata.partition_begin;
      return true;
    case CanonicalWindowFrameBoundKind::unbounded_following:
      *boundary = metadata.partition_end_exclusive;
      return true;
    case CanonicalWindowFrameBoundKind::current_row:
      *boundary = start ? metadata.peer_begin : metadata.peer_end_exclusive;
      return true;
    case CanonicalWindowFrameBoundKind::offset_preceding:
    case CanonicalWindowFrameBoundKind::offset_following:
      break;
  }
  const auto& term = input.order_terms.front();
  const auto& current_value =
      input.ordered_batch.rows[current].values[term.column];
  if (current_value.isSqlNull()) {
    *boundary = start ? metadata.peer_begin : metadata.peer_end_exclusive;
    return true;
  }
  api::EngineTypedValue numeric_threshold;
  __int128 temporal_threshold = 0;
  bool temporal = false;
  if (!RangeThreshold(input, bound, current, &numeric_threshold,
                      &temporal_threshold, &temporal, detail)) {
    return false;
  }
  std::size_t candidate = metadata.partition_begin;
  for (; candidate < metadata.partition_end_exclusive; ++candidate) {
    const auto& value = input.ordered_batch.rows[candidate].values[term.column];
    if (value.isSqlNull()) {
      const auto compared = CompareCanonicalDescriptorOrderValues(
          value, current_value, term);
      if (!compared.diagnostic.ok) {
        *detail = compared.diagnostic.detail;
        return false;
      }
      if ((start && compared.comparison >= 0) ||
          (!start && compared.comparison > 0)) {
        break;
      }
      continue;
    }
    int comparison = 0;
    if (!CompareRangeRowToThreshold(input, candidate, numeric_threshold,
                                    temporal_threshold, temporal, &comparison,
                                    detail)) {
      return false;
    }
    if ((start && comparison >= 0) || (!start && comparison > 0)) break;
  }
  *boundary = candidate;
  return true;
}

bool RangeBounds(const CanonicalWindowPartitionOrderResult& input,
                 const CanonicalWindowFrameDescriptor& frame,
                 const std::size_t current,
                 const CanonicalWindowRowPeerMetadata& metadata,
                 BaseBounds* bounds,
                 std::string* detail) {
  if (!RangeBoundary(input, *frame.start, true, current, metadata,
                     &bounds->begin, detail) ||
      !RangeBoundary(input, *frame.end, false, current, metadata,
                     &bounds->end, detail)) {
    return false;
  }
  if (bounds->begin > bounds->end) {
    bounds->state = CanonicalWindowFrameState::reversed_to_empty;
  } else if (bounds->begin == bounds->end) {
    bounds->state = CanonicalWindowFrameState::empty;
  } else {
    bounds->state = CanonicalWindowFrameState::nonempty;
  }
  return true;
}

bool Excluded(const CanonicalWindowFrameExclusion exclusion,
              const std::size_t candidate,
              const std::size_t current,
              const CanonicalWindowRowPeerMetadata& metadata) {
  const bool peer = candidate >= metadata.peer_begin &&
                    candidate < metadata.peer_end_exclusive;
  switch (exclusion) {
    case CanonicalWindowFrameExclusion::no_others:
      return false;
    case CanonicalWindowFrameExclusion::current_row:
      return candidate == current;
    case CanonicalWindowFrameExclusion::group:
      return peer;
    case CanonicalWindowFrameExclusion::ties:
      return peer && candidate != current;
  }
  return false;
}

}  // namespace

// QOW-SOURCE-WIN-003-V1
// QOW-SOURCE-WIN-013-V1
// QOW-SOURCE-IAS-004-V1
// Construct every base frame and exclusion from the typed partition/order/peer
// evidence produced by QOW-401. Empty bounds are represented by disengaged
// optionals, never by row zero or another valid row index.
CanonicalWindowFrameResult ExecuteCanonicalWindowFrames(
    const CanonicalWindowFrameRequest& request) {
  CanonicalWindowFrameResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic = Refusal(std::move(detail));
    return result;
  };
  if (!MetadataIsCanonical(request.partition_order)) {
    return refuse("window frame input is not canonical QOW-401 evidence");
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.partition_order.physical_dag);
  if (!authority_validation.ok ||
      !PhysicalMgaStatementContextEqual(
          request.mga_authority.statement_context,
          request.partition_order.mga_statement_context)) {
    return refuse(authority_validation.ok
                      ? "window frame and partition MGA contexts diverge"
                      : authority_validation.detail);
  }
  if (request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse("window frame attempted to claim engine MGA authority");
  }
  if (request.maximum_effective_row_references == 0) {
    return refuse("window frame effective-row resource bound is zero");
  }
  for (const auto& term : request.partition_order.order_terms) {
    if (term.column >= request.partition_order.ordered_batch.columns.size()) {
      return refuse("retained window order term is outside the schema");
    }
    const auto validation = ValidateCanonicalDescriptorOrderTerm(
        term, request.partition_order.ordered_batch.columns[term.column]);
    if (!validation.ok) {
      return refuse("retained window order term is no longer valid");
    }
  }

  std::string detail;
  if (!ResolveFrame(request, &result.resolved_frame,
                    &result.defaulted_with_order,
                    &result.defaulted_without_order, &detail)) {
    return refuse(std::move(detail));
  }

  result.ordered_batch = request.partition_order.ordered_batch;
  result.row_metadata = request.partition_order.row_metadata;
  result.effective_frames.reserve(result.row_metadata.size());
  std::size_t effective_row_references = 0;
  for (std::size_t current = 0; current < result.row_metadata.size();
       ++current) {
    const auto& metadata = result.row_metadata[current];
    BaseBounds bounds;
    switch (result.resolved_frame.unit) {
      case CanonicalWindowFrameUnit::rows:
        bounds = RowsBounds(result.resolved_frame, current, metadata);
        break;
      case CanonicalWindowFrameUnit::groups:
        bounds = GroupsBounds(request.partition_order, result.resolved_frame,
                              metadata);
        break;
      case CanonicalWindowFrameUnit::range:
        if (!RangeBounds(request.partition_order, result.resolved_frame,
                         current, metadata, &bounds, &detail)) {
          return refuse(std::move(detail));
        }
        break;
    }

    CanonicalWindowEffectiveFrame effective;
    effective.ordered_row_index = current;
    effective.partition_id = metadata.partition_id;
    effective.base_state = bounds.state;
    effective.exclusion_applied =
        result.resolved_frame.exclusion !=
        CanonicalWindowFrameExclusion::no_others;
    if (bounds.state == CanonicalWindowFrameState::nonempty) {
      effective.base_begin = bounds.begin;
      effective.base_end_exclusive = bounds.end;
      for (std::size_t candidate = bounds.begin; candidate < bounds.end;
           ++candidate) {
        if (Excluded(result.resolved_frame.exclusion, candidate, current,
                     metadata)) {
          continue;
        }
        if (effective_row_references ==
            request.maximum_effective_row_references) {
          return refuse("window frame effective-row resource bound was exceeded");
        }
        effective.effective_row_indices.push_back(candidate);
        ++effective_row_references;
      }
    }
    result.effective_frames.push_back(std::move(effective));
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.partition_order.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.detail);
  }

  result.diagnostic = {};
  result.window_property_uuid = request.partition_order.window_property_uuid;
  result.ordering_property_uuid = request.partition_order.ordering_property_uuid;
  result.every_frame_operand_consumed = true;
  result.empty_state_uses_optional_bounds = true;
  result.authority = request.partition_order.authority;
  result.mga_authority = request.mga_authority;
  result.mga_statement_context = request.mga_authority.statement_context;
  result.selected_plan_uuid = request.partition_order.selected_plan_uuid;
  result.executed_physical_node_id =
      request.partition_order.executed_physical_node_id;
  result.causal_counter_id = request.partition_order.causal_counter_id;
  result.physical_dag = request.partition_order.physical_dag;
  return result;
}

}  // namespace scratchbird::engine::executor
