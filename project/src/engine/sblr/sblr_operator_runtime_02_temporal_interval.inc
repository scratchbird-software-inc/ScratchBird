// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  std::int64_t days = epoch_seconds / kDaySeconds;
  std::int64_t seconds = epoch_seconds % kDaySeconds;
  if (seconds < 0) {
    seconds += kDaySeconds;
    --days;
  }
  return FormatDateFromDays(days) + "T" + FormatTimeFromSeconds(seconds) + (with_timezone ? "Z" : "");
}

bool ParseDate(std::string_view text, std::int64_t* days) {
  if (text.size() < 10 || text[4] != '-' || text[7] != '-') return false;
  const auto year = ParseNDigits(text, 0, 4);
  const auto month = ParseNDigits(text, 5, 2);
  const auto day = ParseNDigits(text, 8, 2);
  if (!year || !month || !day) return false;
  if (*month < 1 || *month > 12 || *day < 1 || *day > DaysInMonth(*year, *month)) return false;
  *days = DaysFromCivil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
  return true;
}

bool ParseTime(std::string_view text, std::size_t offset, std::int64_t* seconds, std::size_t* consumed) {
  if (offset + 8 > text.size() || text[offset + 2] != ':' || text[offset + 5] != ':') return false;
  const auto hour = ParseNDigits(text, offset, 2);
  const auto minute = ParseNDigits(text, offset + 3, 2);
  const auto second = ParseNDigits(text, offset + 6, 2);
  if (!hour || !minute || !second) return false;
  if (*hour > 23 || *minute > 59 || *second > 60) return false;
  std::size_t index = offset + 8;
  if (index < text.size() && text[index] == '.') {
    ++index;
    const std::size_t start = index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') ++index;
    if (index == start) return false;
  }
  *seconds = static_cast<std::int64_t>(*hour) * 3600 + static_cast<std::int64_t>(*minute) * 60 +
             static_cast<std::int64_t>(*second == 60 ? 59 : *second);
  *consumed = index - offset;
  return true;
}

bool ParseTimezoneOffset(std::string_view text, std::size_t offset, std::int64_t* offset_seconds) {
  if (offset == text.size()) {
    *offset_seconds = 0;
    return true;
  }
  if (offset + 1 == text.size() && text[offset] == 'Z') {
    *offset_seconds = 0;
    return true;
  }
  if (offset + 6 == text.size() && (text[offset] == '+' || text[offset] == '-') && text[offset + 3] == ':') {
    const auto hour = ParseNDigits(text, offset + 1, 2);
    const auto minute = ParseNDigits(text, offset + 4, 2);
    if (!hour || !minute || *hour > 23 || *minute > 59) return false;
    const std::int64_t seconds = static_cast<std::int64_t>(*hour) * 3600 + static_cast<std::int64_t>(*minute) * 60;
    *offset_seconds = text[offset] == '-' ? -seconds : seconds;
    return true;
  }
  return false;
}

bool ParseTimestamp(std::string_view text, std::int64_t* epoch_seconds, bool* has_timezone) {
  std::int64_t days = 0;
  if (!ParseDate(text, &days)) return false;
  if (text.size() < 19 || (text[10] != 'T' && text[10] != ' ')) return false;
  std::int64_t seconds = 0;
  std::size_t consumed = 0;
  if (!ParseTime(text, 11, &seconds, &consumed)) return false;
  const std::size_t tz_offset = 11 + consumed;
  std::int64_t offset_seconds = 0;
  if (!ParseTimezoneOffset(text, tz_offset, &offset_seconds)) return false;
  *has_timezone = tz_offset < text.size();
  *epoch_seconds = days * 86400 + seconds - offset_seconds;
  return true;
}

std::string EncodedForTemporal(const SblrValue& value) {
  if (!value.encoded_value.empty()) return value.encoded_value;
  if (!value.text_value.empty()) return value.text_value;
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  return {};
}

bool ParseIntervalSeconds(const SblrValue& value, std::int64_t* seconds) {
  const std::string text = EncodedForTemporal(value);
  if (TryInt64Text(text, seconds)) return true;
  if (text.empty() || text[0] != 'P') return false;
  std::size_t index = 1;
  bool in_time = false;
  bool saw_component = false;
  std::int64_t total = 0;
  while (index < text.size()) {
    if (text[index] == 'T') {
      if (in_time) return false;
      in_time = true;
      ++index;
      continue;
    }
    std::size_t start = index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') ++index;
    if (start == index || index >= text.size()) return false;
    std::int64_t amount = 0;
    if (!TryInt64Text(std::string_view(text).substr(start, index - start), &amount)) return false;
    const char designator = text[index++];
    std::int64_t multiplier = 0;
    if (!in_time && designator == 'D') multiplier = 86400;
    else if (in_time && designator == 'H') multiplier = 3600;
    else if (in_time && designator == 'M') multiplier = 60;
    else if (in_time && designator == 'S') multiplier = 1;
    else return false;
    if (amount > std::numeric_limits<std::int64_t>::max() / multiplier) return false;
    const std::int64_t add = amount * multiplier;
    if (total > std::numeric_limits<std::int64_t>::max() - add) return false;
    total += add;
    saw_component = true;
  }
  if (!saw_component) return false;
  *seconds = total;
  return true;
}

struct TemporalParsedValue {
  std::string descriptor_id;
  std::int64_t epoch_seconds = 0;
  std::int64_t day_number = 0;
  std::int64_t time_seconds = 0;
  bool has_timezone = false;
};

bool ParseTemporalValue(const SblrValue& value, TemporalParsedValue* out) {
  const std::string text = EncodedForTemporal(value);
  out->descriptor_id = value.descriptor_id.empty() ? "timestamp" : value.descriptor_id;
  if (value.descriptor_id == "timestamp_epoch_ms") {
    std::int64_t millis = 0;
    if (!TryInt64Text(text, &millis)) return false;
    out->epoch_seconds = millis / 1000;
    out->day_number = out->epoch_seconds / 86400;
    out->time_seconds = out->epoch_seconds % 86400;
    if (out->time_seconds < 0) {
      out->time_seconds += 86400;
      --out->day_number;
    }
    return true;
  }
  if (value.descriptor_id == "date") {
    return text.size() == 10 && ParseDate(text, &out->day_number);
  }
  if (value.descriptor_id == "time") {
    std::size_t consumed = 0;
    return ParseTime(text, 0, &out->time_seconds, &consumed) && consumed == text.size();
  }
  bool has_timezone = false;
  if (!ParseTimestamp(text, &out->epoch_seconds, &has_timezone)) return false;
  out->has_timezone = has_timezone || value.descriptor_id == "timestamp_tz";
  out->day_number = out->epoch_seconds / 86400;
  out->time_seconds = out->epoch_seconds % 86400;
  if (out->time_seconds < 0) {
    out->time_seconds += 86400;
    --out->day_number;
  }
  return true;
}

std::string TemporalResultDescriptor(const TemporalParsedValue& value) {
  if (value.descriptor_id == "timestamp_tz") return "timestamp_tz";
  if (value.descriptor_id == "timestamp_epoch_ms") return "timestamp";
  return value.descriptor_id;
}

SblrValue FormatTemporalResult(const TemporalParsedValue& source, std::int64_t add_seconds) {
  if (source.descriptor_id == "date") {
    if (add_seconds % 86400 != 0) {
      return TemporalValue("timestamp", FormatTimestampUtc(source.day_number * 86400 + add_seconds, false));
    }
    return TemporalValue("date", FormatDateFromDays(source.day_number + add_seconds / 86400));
  }
  if (source.descriptor_id == "time") {
    return TemporalValue("time", FormatTimeFromSeconds(source.time_seconds + add_seconds));
  }
  const bool with_timezone = source.descriptor_id == "timestamp_tz" || source.has_timezone;
  return TemporalValue(TemporalResultDescriptor(source), FormatTimestampUtc(source.epoch_seconds + add_seconds, with_timezone));
}

SblrResult EvaluateSblrTemporalArithmetic(std::string_view operator_id,
                                          const SblrValue& left,
                                          const SblrValue& right,
                                          const SblrExecutionContext& context) {
  const bool add = IsAnyOperator(operator_id, "op_add", "operator.numeric.add", "operator.arithmetic.add") ||
                   operator_id == "operator.temporal.add";
  const bool subtract = IsAnyOperator(operator_id, "op_sub", "operator.numeric.subtract", "operator.arithmetic.subtract") ||
                        operator_id == "operator.temporal.subtract";
  if (!add && !subtract) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SB_DIAG_OPERATOR_INVALID_INPUT",
                                     "temporal arithmetic only supports add and subtract");
  }
  if (IsIntervalPayload(left) && IsIntervalPayload(right)) {
    std::int64_t left_seconds = 0;
    std::int64_t right_seconds = 0;
    if (!ParseIntervalSeconds(left, &left_seconds) || !ParseIntervalSeconds(right, &right_seconds)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.INTERVAL_INVALID", "interval operand is not canonical seconds or supported ISO day/time duration");
    }
    if ((add && ((right_seconds > 0 && left_seconds > std::numeric_limits<std::int64_t>::max() - right_seconds) ||
                 (right_seconds < 0 && left_seconds < std::numeric_limits<std::int64_t>::min() - right_seconds))) ||
        (subtract && ((right_seconds > 0 && left_seconds < std::numeric_limits<std::int64_t>::min() + right_seconds) ||
                      (right_seconds < 0 && left_seconds > std::numeric_limits<std::int64_t>::max() + right_seconds)))) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.INTERVAL_OVERFLOW", "interval arithmetic overflow");
    }
    const std::int64_t result = add ? left_seconds + right_seconds : left_seconds - right_seconds;
    return ScalarResult(operator_id, TemporalValue("interval", std::to_string(result)));
  }
  if (IsTemporalPayload(left) && IsIntervalPayload(right)) {
    TemporalParsedValue temporal;
    std::int64_t interval_seconds = 0;
    if (!ParseTemporalValue(left, &temporal)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.TEMPORAL_INVALID", "left temporal operand is not a supported canonical temporal value");
    }
    if (!ParseIntervalSeconds(right, &interval_seconds)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.INTERVAL_INVALID", "right interval operand is not canonical seconds or supported ISO day/time duration");
    }
    if (subtract && interval_seconds == std::numeric_limits<std::int64_t>::min()) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.INTERVAL_OVERFLOW", "interval negation overflow");
    }
    return ScalarResult(operator_id, FormatTemporalResult(temporal, add ? interval_seconds : -interval_seconds));
  }
  if (add && IsIntervalPayload(left) && IsTemporalPayload(right)) {
    TemporalParsedValue temporal;
    std::int64_t interval_seconds = 0;
    if (!ParseIntervalSeconds(left, &interval_seconds)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.INTERVAL_INVALID", "left interval operand is not canonical seconds or supported ISO day/time duration");
    }
    if (!ParseTemporalValue(right, &temporal)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.TEMPORAL_INVALID", "right temporal operand is not a supported canonical temporal value");
    }
    return ScalarResult(operator_id, FormatTemporalResult(temporal, interval_seconds));
  }
  if (subtract && IsTemporalPayload(left) && IsTemporalPayload(right)) {
    TemporalParsedValue left_temporal;
    TemporalParsedValue right_temporal;
    if (!ParseTemporalValue(left, &left_temporal) || !ParseTemporalValue(right, &right_temporal)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.TEMPORAL_INVALID", "temporal operands are not supported canonical temporal values");
    }
    std::int64_t left_seconds = 0;
    std::int64_t right_seconds = 0;
    if (left.descriptor_id == "date") left_seconds = left_temporal.day_number * 86400;
    else if (left.descriptor_id == "time") left_seconds = left_temporal.time_seconds;
    else left_seconds = left_temporal.epoch_seconds;
    if (right.descriptor_id == "date") right_seconds = right_temporal.day_number * 86400;
    else if (right.descriptor_id == "time") right_seconds = right_temporal.time_seconds;
    else right_seconds = right_temporal.epoch_seconds;
    if ((right_seconds > 0 && left_seconds < std::numeric_limits<std::int64_t>::min() + right_seconds) ||
        (right_seconds < 0 && left_seconds > std::numeric_limits<std::int64_t>::max() + right_seconds)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.INTERVAL_OVERFLOW", "temporal difference overflow");
    }
    return ScalarResult(operator_id, TemporalValue("interval", std::to_string(left_seconds - right_seconds)));
