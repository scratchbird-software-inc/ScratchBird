// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  }
  return false;
}

FunctionCallResult UnaryReal(const FunctionCallRequest& request, double (*fn)(double), std::string name) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, name + " expects exactly one argument");
  const auto& value = request.arguments[0].value;
  if (IsSqlNull(value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
  double input = 0.0;
  if (!ValueAsReal64(value, &input)) return RefuseFunctionInvalidInput(request, name + " requires a numeric value");
  const double output = fn(input);
  if (!std::isfinite(output)) return RefuseFunctionOverflow(request, name + " produced a non-finite result");
  return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
}

std::string TwoDigit(std::int64_t value) {
  std::ostringstream out;
  out << std::setw(2) << std::setfill('0') << value;
  return out.str();
}

std::string FourDigit(std::int64_t value) {
  std::ostringstream out;
  out << std::setw(4) << std::setfill('0') << value;
  return out.str();
}

std::string CurrentTimestampText(const FunctionCallRequest& request) {
  if (!request.context.sblr_context.current_timestamp.empty()) return request.context.sblr_context.current_timestamp;
  if (!request.context.sblr_context.statement_timestamp.empty()) return request.context.sblr_context.statement_timestamp;
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return std::to_string(millis);
}

std::string DatePartFromTimestamp(std::string timestamp) {
  if (timestamp.size() >= 10 && timestamp[4] == '-' && timestamp[7] == '-') return timestamp.substr(0, 10);
  return timestamp;
}

std::string TimePartFromTimestamp(std::string timestamp) {
  const auto separator = timestamp.find_first_of("T ");
  if (separator == std::string::npos || separator + 1 >= timestamp.size()) return {};
  std::string time = timestamp.substr(separator + 1);
  const auto zone = time.find_first_of("Z+-");
  if (zone != std::string::npos && zone >= 8) time = time.substr(0, zone);
  return time;
}

std::string TruncateTimestamp(std::string timestamp, std::string field) {
  field = LowerAscii(std::move(field));
  if (timestamp.size() < 10) return timestamp;
  if (field == "year") return timestamp.substr(0, 4) + "-01-01T00:00:00";
  if (field == "month") return timestamp.substr(0, 7) + "-01T00:00:00";
  if (field == "day") return timestamp.substr(0, 10) + "T00:00:00";
  if (field == "hour" && timestamp.size() >= 13) return timestamp.substr(0, 13) + ":00:00";
  if (field == "minute" && timestamp.size() >= 16) return timestamp.substr(0, 16) + ":00";
  if (field == "second" && timestamp.size() >= 19) return timestamp.substr(0, 19);
  return timestamp;
}

// Days from the proleptic Gregorian calendar's epoch (1970-01-01) using
// Howard Hinnant's days_from_civil algorithm. Pure function, valid for
// year in [-(2^31), 2^31). Used by ExtractTimestampField for the epoch
// field (Unix seconds since 1970-01-01T00:00:00 UTC).
std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Inverse of DaysFromCivil — recovers (year, month, day) from a serial day
// count where day 0 is 1970-01-01. Howard Hinnant's civil_from_days algorithm.
// Pure function; valid over the same year domain as DaysFromCivil.
void CivilFromDays(std::int64_t z, std::int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = year + (*m <= 2 ? 1 : 0);
}

bool IsLeapYear(std::int64_t year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

unsigned DaysInMonth(std::int64_t year, unsigned month) {
  static constexpr unsigned kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && IsLeapYear(year)) return 29;
  return kDaysInMonth[month - 1];
}

struct ParsedTemporal {
  std::int64_t year{0};
  unsigned month{0};
  unsigned day{0};
  unsigned hour{0};
  unsigned minute{0};
  unsigned second{0};
  bool has_time{false};
  bool has_utc_zone{false};
};

bool ParseFixedInt(std::string_view text, std::size_t pos, std::size_t count, std::int64_t* out) {
  if (out == nullptr || pos + count > text.size()) return false;
  std::int64_t value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char ch = text[pos + i];
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    value = value * 10 + static_cast<std::int64_t>(ch - '0');
  }
  *out = value;
  return true;
}

bool ParseIsoLikeTemporal(std::string_view text, ParsedTemporal* out) {
  if (out == nullptr || text.size() < 10 || text[4] != '-' || text[7] != '-') return false;
  ParsedTemporal parsed;
  std::int64_t year = 0;
  std::int64_t month = 0;
  std::int64_t day = 0;
  if (!ParseFixedInt(text, 0, 4, &year) ||
      !ParseFixedInt(text, 5, 2, &month) ||
      !ParseFixedInt(text, 8, 2, &day)) {
    return false;
  }
  if (month < 1 || month > 12) return false;
  const unsigned month_days = DaysInMonth(year, static_cast<unsigned>(month));
  if (day < 1 || day > static_cast<std::int64_t>(month_days)) return false;
  parsed.year = year;
  parsed.month = static_cast<unsigned>(month);
  parsed.day = static_cast<unsigned>(day);
  if (text.size() == 10) {
    *out = parsed;
    return true;
  }
  if (text[10] != 'T' && text[10] != ' ') return false;
  if (text.size() < 19 || text[13] != ':' || text[16] != ':') return false;
  std::int64_t hour = 0;
  std::int64_t minute = 0;
  std::int64_t second = 0;
  if (!ParseFixedInt(text, 11, 2, &hour) ||
      !ParseFixedInt(text, 14, 2, &minute) ||
      !ParseFixedInt(text, 17, 2, &second)) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
  parsed.hour = static_cast<unsigned>(hour);
  parsed.minute = static_cast<unsigned>(minute);
  parsed.second = static_cast<unsigned>(second);
  parsed.has_time = true;
  std::size_t cursor = 19;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    const std::size_t fraction_begin = cursor;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) ++cursor;
    if (cursor == fraction_begin) return false;
  }
  if (cursor == text.size()) {
    *out = parsed;
    return true;
  }
  const std::string zone = LowerAscii(std::string(text.substr(cursor)));
  if (zone == "z" || zone == "+00:00" || zone == "-00:00" || zone == "+0000" || zone == "-0000") {
    parsed.has_utc_zone = true;
    *out = parsed;
    return true;
  }
  return false;
}

std::int64_t TemporalToEpochSeconds(const ParsedTemporal& temporal) {
  return DaysFromCivil(temporal.year, temporal.month, temporal.day) * 86400 +
         static_cast<std::int64_t>(temporal.hour) * 3600 +
         static_cast<std::int64_t>(temporal.minute) * 60 +
         static_cast<std::int64_t>(temporal.second);
}

int CompareTemporal(const ParsedTemporal& left, const ParsedTemporal& right) {
  if (left.year != right.year) return left.year < right.year ? -1 : 1;
  if (left.month != right.month) return left.month < right.month ? -1 : 1;
  if (left.day != right.day) return left.day < right.day ? -1 : 1;
  if (left.hour != right.hour) return left.hour < right.hour ? -1 : 1;
  if (left.minute != right.minute) return left.minute < right.minute ? -1 : 1;
  if (left.second != right.second) return left.second < right.second ? -1 : 1;
  return 0;
}

std::int64_t WholeMonthsBetweenForward(const ParsedTemporal& start, const ParsedTemporal& finish) {
  std::int64_t months = (finish.year - start.year) * 12 +
                        static_cast<std::int64_t>(finish.month) -
                        static_cast<std::int64_t>(start.month);
  if (months <= 0) return 0;
  const bool finish_before_start_day =
      finish.day < start.day ||
      (finish.day == start.day &&
       std::tie(finish.hour, finish.minute, finish.second) <
           std::tie(start.hour, start.minute, start.second));
  if (finish_before_start_day) --months;
  return months;
}

std::int64_t WholeMonthsBetween(const ParsedTemporal& start, const ParsedTemporal& finish) {
  if (CompareTemporal(start, finish) <= 0) return WholeMonthsBetweenForward(start, finish);
  return -WholeMonthsBetweenForward(finish, start);
}

ParsedTemporal TemporalFromEpochSeconds(std::int64_t epoch_seconds) {
  std::int64_t days = epoch_seconds / 86400;
  std::int64_t seconds = epoch_seconds % 86400;
  if (seconds < 0) {
    seconds += 86400;
    --days;
  }
  ParsedTemporal out;
  CivilFromDays(days, &out.year, &out.month, &out.day);
  out.hour = static_cast<unsigned>(seconds / 3600);
  out.minute = static_cast<unsigned>((seconds % 3600) / 60);
  out.second = static_cast<unsigned>(seconds % 60);
  out.has_time = true;
  return out;
}

std::string FormatDate(const ParsedTemporal& temporal) {
  return FourDigit(temporal.year) + "-" + TwoDigit(temporal.month) + "-" + TwoDigit(temporal.day);
}

std::string FormatTimestamp(const ParsedTemporal& temporal) {
  return FormatDate(temporal) + "T" + TwoDigit(temporal.hour) + ":" +
         TwoDigit(temporal.minute) + ":" + TwoDigit(temporal.second);
}

std::string FormatTimestampTz(const ParsedTemporal& temporal) {
  return FormatTimestamp(temporal) + "Z";
}

std::string MonthNameEnglish(unsigned month) {
  static constexpr std::string_view kMonths[] = {"January", "February", "March", "April", "May", "June",
                                                 "July", "August", "September", "October", "November", "December"};
  return month >= 1 && month <= 12 ? std::string(kMonths[month - 1]) : std::string{};
}

std::string ShortMonthNameEnglish(unsigned month) {
  const auto full = MonthNameEnglish(month);
  return full.size() >= 3 ? full.substr(0, 3) : full;
}

std::string DayNameEnglish(int sunday_zero_dow) {
  static constexpr std::string_view kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                               "Thursday", "Friday", "Saturday"};
  return sunday_zero_dow >= 0 && sunday_zero_dow < 7 ? std::string(kDays[sunday_zero_dow]) : std::string{};
}

std::string ShortDayNameEnglish(int sunday_zero_dow) {
  const auto full = DayNameEnglish(sunday_zero_dow);
  return full.size() >= 3 ? full.substr(0, 3) : full;
}

int DayOfWeekSundayZero(const ParsedTemporal& temporal) {
  const std::int64_t days = DaysFromCivil(temporal.year, temporal.month, temporal.day);
  return static_cast<int>(((days % 7) + 11) % 7);
}

bool IsSupportedEnglishLocale(std::string locale) {
  locale = LowerAscii(TrimAscii(std::move(locale)));
  return locale.empty() || locale == "c" || locale == "posix" || locale == "en" ||
         locale == "en_us" || locale == "en-us" || locale == "english";
}

bool ParseMonthNameText(std::string value, unsigned* month) {
  if (month == nullptr) return false;
  value = LowerAscii(TrimAscii(std::move(value)));
  static constexpr std::string_view kMonths[] = {"january", "february", "march", "april", "may", "june",
                                                 "july", "august", "september", "october", "november", "december"};
  for (unsigned i = 0; i < 12; ++i) {
    if (value == kMonths[i] || value == kMonths[i].substr(0, 3)) {
      *month = i + 1;
      return true;
    }
  }
  return false;
}

bool ParseWeekdayText(std::string value, int* sunday_zero_dow) {
  if (sunday_zero_dow == nullptr) return false;
  value = LowerAscii(TrimAscii(std::move(value)));
  if (value.empty()) return false;
  if (std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
    const int numeric = std::stoi(value);
    if (numeric == 0 || numeric == 7) {
      *sunday_zero_dow = 0;
      return true;
    }
    if (numeric >= 1 && numeric <= 6) {
      *sunday_zero_dow = numeric;
      return true;
    }
    return false;
  }
  static constexpr std::string_view kDays[] = {"sunday", "monday", "tuesday", "wednesday",
                                               "thursday", "friday", "saturday"};
  for (int i = 0; i < 7; ++i) {
    if (value == kDays[i] || value == kDays[i].substr(0, 3)) {
      *sunday_zero_dow = i;
      return true;
    }
  }
  return false;
}

std::string FormatTemporalPattern(const ParsedTemporal& temporal, std::string_view format) {
  const int dow = DayOfWeekSundayZero(temporal);
  std::string out;
  for (std::size_t i = 0; i < format.size();) {
    const auto rest = format.substr(i);
    if (rest.rfind("YYYY", 0) == 0) {
      out += FourDigit(temporal.year);
      i += 4;
    } else if (rest.rfind("HH24", 0) == 0) {
      out += TwoDigit(temporal.hour);
      i += 4;
    } else if (rest.rfind("Month", 0) == 0) {
      out += MonthNameEnglish(temporal.month);
      i += 5;
    } else if (rest.rfind("MONTH", 0) == 0) {
      out += UpperAscii(MonthNameEnglish(temporal.month));
      i += 5;
    } else if (rest.rfind("month", 0) == 0) {
      out += LowerAscii(MonthNameEnglish(temporal.month));
      i += 5;
    } else if (rest.rfind("Mon", 0) == 0) {
      out += ShortMonthNameEnglish(temporal.month);
      i += 3;
    } else if (rest.rfind("MON", 0) == 0) {
      out += UpperAscii(ShortMonthNameEnglish(temporal.month));
      i += 3;
    } else if (rest.rfind("mon", 0) == 0) {
      out += LowerAscii(ShortMonthNameEnglish(temporal.month));
      i += 3;
    } else if (rest.rfind("Day", 0) == 0) {
      out += DayNameEnglish(dow);
      i += 3;
    } else if (rest.rfind("DAY", 0) == 0) {
      out += UpperAscii(DayNameEnglish(dow));
      i += 3;
    } else if (rest.rfind("day", 0) == 0) {
      out += LowerAscii(DayNameEnglish(dow));
      i += 3;
    } else if (rest.rfind("Dy", 0) == 0) {
      out += ShortDayNameEnglish(dow);
      i += 2;
    } else if (rest.rfind("DY", 0) == 0) {
      out += UpperAscii(ShortDayNameEnglish(dow));
      i += 2;
    } else if (rest.rfind("dy", 0) == 0) {
      out += LowerAscii(ShortDayNameEnglish(dow));
      i += 2;
    } else if (rest.rfind("MM", 0) == 0) {
      out += TwoDigit(temporal.month);
      i += 2;
    } else if (rest.rfind("DD", 0) == 0) {
      out += TwoDigit(temporal.day);
      i += 2;
    } else if (rest.rfind("MI", 0) == 0) {
      out += TwoDigit(temporal.minute);
      i += 2;
    } else if (rest.rfind("SS", 0) == 0) {
      out += TwoDigit(temporal.second);
      i += 2;
    } else {
      out.push_back(format[i]);
      ++i;
    }
  }
  return out;
}

bool ParseDateTimeByFormat(std::string_view text, std::string_view format, ParsedTemporal* out) {
  if (out == nullptr) return false;
  ParsedTemporal parsed;
  bool saw_year = false;
  bool saw_month = false;
  bool saw_day = false;
  std::size_t text_pos = 0;
  auto parse_digits = [&](std::size_t count, std::int64_t* value) -> bool {
    if (text_pos + count > text.size()) return false;
    if (!ParseFixedInt(text, text_pos, count, value)) return false;
    text_pos += count;
    return true;
  };
  auto parse_alpha = [&](std::string* value) -> bool {
    const std::size_t begin = text_pos;
    while (text_pos < text.size() && std::isalpha(static_cast<unsigned char>(text[text_pos]))) ++text_pos;
    if (text_pos == begin) return false;
    *value = std::string(text.substr(begin, text_pos - begin));
    return true;
  };
  for (std::size_t fmt_pos = 0; fmt_pos < format.size();) {
    const auto rest = format.substr(fmt_pos);
    std::int64_t value = 0;
    if (rest.rfind("YYYY", 0) == 0) {
      if (!parse_digits(4, &value)) return false;
      parsed.year = value;
      saw_year = true;
      fmt_pos += 4;
    } else if (rest.rfind("HH24", 0) == 0) {
      if (!parse_digits(2, &value) || value < 0 || value > 23) return false;
      parsed.hour = static_cast<unsigned>(value);
      parsed.has_time = true;
      fmt_pos += 4;
    } else if (rest.rfind("Month", 0) == 0 || rest.rfind("MONTH", 0) == 0 ||
               rest.rfind("month", 0) == 0) {
      std::string month_text;
      if (!parse_alpha(&month_text) || !ParseMonthNameText(month_text, &parsed.month)) return false;
      saw_month = true;
      fmt_pos += 5;
    } else if (rest.rfind("Mon", 0) == 0 || rest.rfind("MON", 0) == 0 ||
               rest.rfind("mon", 0) == 0) {
      std::string month_text;
      if (!parse_alpha(&month_text) || !ParseMonthNameText(month_text, &parsed.month)) return false;
      saw_month = true;
      fmt_pos += 3;
    } else if (rest.rfind("MM", 0) == 0) {
      if (!parse_digits(2, &value) || value < 1 || value > 12) return false;
      parsed.month = static_cast<unsigned>(value);
      saw_month = true;
      fmt_pos += 2;
    } else if (rest.rfind("DD", 0) == 0) {
      if (!parse_digits(2, &value) || value < 1 || value > 31) return false;
      parsed.day = static_cast<unsigned>(value);
      saw_day = true;
      fmt_pos += 2;
    } else if (rest.rfind("MI", 0) == 0) {
      if (!parse_digits(2, &value) || value < 0 || value > 59) return false;
      parsed.minute = static_cast<unsigned>(value);
      parsed.has_time = true;
      fmt_pos += 2;
    } else if (rest.rfind("SS", 0) == 0) {
      if (!parse_digits(2, &value) || value < 0 || value > 59) return false;
      parsed.second = static_cast<unsigned>(value);
      parsed.has_time = true;
      fmt_pos += 2;
    } else {
      if (text_pos >= text.size() || text[text_pos] != format[fmt_pos]) return false;
      ++text_pos;
      ++fmt_pos;
    }
  }
  if (text_pos != text.size() || !saw_year || !saw_month || !saw_day) return false;
  if (parsed.day > DaysInMonth(parsed.year, parsed.month)) return false;
  *out = parsed;
  return true;
}

std::string FormatNumericPattern(const scratchbird::engine::sblr::SblrValue& value,
                                 std::string format) {
  const bool fill_mode = format.rfind("FM", 0) == 0 || format.rfind("fm", 0) == 0;
  if (fill_mode) format = format.substr(2);
  double numeric = 0.0;
  if (!ValueAsReal64(value, &numeric)) return {};
  std::size_t decimals = 0;
  const auto decimal_pos = format.find_first_of(".D");
  if (decimal_pos != std::string::npos) {
    for (std::size_t i = decimal_pos + 1; i < format.size(); ++i) {
      if (format[i] == '9' || format[i] == '0') ++decimals;
    }
  }
  std::ostringstream out;
  if (decimal_pos != std::string::npos) out << std::fixed << std::setprecision(static_cast<int>(decimals));
  out << numeric;
  std::string text = out.str();
  if (fill_mode) return TrimAscii(text);
  return text;
}

bool ParseNumberByFormat(std::string text, std::string_view format, double* out) {
  if (out == nullptr) return false;
  text = TrimAscii(std::move(text));
  bool negative = false;
  if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
    negative = true;
    text = text.substr(1, text.size() - 2);
  }
  const bool format_has_decimal = format.find('D') != std::string_view::npos ||
                                  format.find('.') != std::string_view::npos;
  std::size_t decimal_index = std::string::npos;
  if (format_has_decimal) {
    decimal_index = text.find_last_of(".,");
  }
  std::string normalized;
  normalized.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '+' || ch == '-') {
      normalized.push_back(ch);
    } else if (i == decimal_index && (ch == '.' || ch == ',')) {
      normalized.push_back('.');
    } else if (ch == ',' || ch == '.' || ch == '$' || std::isspace(static_cast<unsigned char>(ch))) {
      continue;
    } else {
      return false;
    }
  }
  if (normalized.empty()) return false;
  try {
    std::size_t consumed = 0;
    double parsed = std::stod(normalized, &consumed);
    if (consumed != normalized.size() || !std::isfinite(parsed)) return false;
    *out = negative ? -parsed : parsed;
    return true;
  } catch (...) {
    return false;
  }
}

struct ParsedInterval {
  std::int64_t years{0};
  std::int64_t months{0};
  std::int64_t days{0};
  std::int64_t hours{0};
  std::int64_t minutes{0};
  std::int64_t seconds{0};
};

bool ParseIsoIntervalText(std::string_view text, ParsedInterval* out) {
  if (out == nullptr || text.empty()) return false;
  std::size_t pos = 0;
  std::int64_t sign = 1;
  if (text[pos] == '+' || text[pos] == '-') {
    sign = text[pos] == '-' ? -1 : 1;
    ++pos;
  }
  if (pos >= text.size() || text[pos] != 'P') return false;
  ++pos;
  bool in_time = false;
  bool saw_component = false;
  ParsedInterval parsed;
  while (pos < text.size()) {
    if (text[pos] == 'T') {
      if (in_time) return false;
      in_time = true;
      ++pos;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(text[pos]))) return false;
    std::int64_t value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
      const std::int64_t digit = text[pos] - '0';
      if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return false;
      value = value * 10 + digit;
      ++pos;
    }
    if (pos >= text.size()) return false;
    const char unit = text[pos++];
    if (unit == 'Y' && !in_time) parsed.years = value;
    else if (unit == 'M' && !in_time) parsed.months = value;
    else if (unit == 'W' && !in_time) parsed.days += value * 7;
    else if (unit == 'D' && !in_time) parsed.days = value;
    else if (unit == 'H' && in_time) parsed.hours = value;
    else if (unit == 'M' && in_time) parsed.minutes = value;
    else if (unit == 'S' && in_time) parsed.seconds = value;
    else return false;
    saw_component = true;
  }
  if (!saw_component) return false;
  parsed.years *= sign;
  parsed.months *= sign;
  parsed.days *= sign;
  parsed.hours *= sign;
  parsed.minutes *= sign;
  parsed.seconds *= sign;
  *out = parsed;
  return true;
}

std::string FormatIntervalText(const ParsedInterval& interval) {
  const bool negative = interval.years < 0 || interval.months < 0 || interval.days < 0 ||
                        interval.hours < 0 || interval.minutes < 0 || interval.seconds < 0;
  const auto magnitude = [](std::int64_t value) -> std::uint64_t {
    if (value == std::numeric_limits<std::int64_t>::min()) {
      return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ull;
    }
    return static_cast<std::uint64_t>(value < 0 ? -value : value);
  };
  std::string out = negative ? "-P" : "P";
  bool emitted = false;
  if (interval.years != 0) {
    out += std::to_string(magnitude(interval.years)) + "Y";
    emitted = true;
  }
  if (interval.months != 0) {
    out += std::to_string(magnitude(interval.months)) + "M";
    emitted = true;
  }
  if (interval.days != 0) {
    out += std::to_string(magnitude(interval.days)) + "D";
    emitted = true;
  }
  if (interval.hours != 0 || interval.minutes != 0 || interval.seconds != 0) {
    out += "T";
    if (interval.hours != 0) out += std::to_string(magnitude(interval.hours)) + "H";
    if (interval.minutes != 0) out += std::to_string(magnitude(interval.minutes)) + "M";
    if (interval.seconds != 0) out += std::to_string(magnitude(interval.seconds)) + "S";
    emitted = true;
  }
  if (!emitted) out += "0D";
  return out;
}

ParsedTemporal AddIntervalToTemporal(ParsedTemporal temporal, const ParsedInterval& interval) {
  std::int64_t total_months = temporal.year * 12 + static_cast<std::int64_t>(temporal.month - 1) +
                              interval.years * 12 + interval.months;
  std::int64_t new_year = total_months / 12;
  std::int64_t new_month = total_months % 12 + 1;
  if (total_months < 0 && total_months % 12 != 0) {
    --new_year;
    new_month += 12;
  }
  temporal.year = new_year;
  temporal.month = static_cast<unsigned>(new_month);
  const unsigned month_days = DaysInMonth(temporal.year, temporal.month);
  if (temporal.day > month_days) temporal.day = month_days;
  const std::int64_t epoch = TemporalToEpochSeconds(temporal) +
                             interval.days * 86400 + interval.hours * 3600 +
                             interval.minutes * 60 + interval.seconds;
  ParsedTemporal out = TemporalFromEpochSeconds(epoch);
  out.has_time = temporal.has_time || interval.hours != 0 || interval.minutes != 0 || interval.seconds != 0;
  return out;
}

bool IntervalToSeconds(const ParsedInterval& interval, std::int64_t* out) {
  if (out == nullptr || interval.years != 0 || interval.months != 0) return false;
  *out = interval.days * 86400 + interval.hours * 3600 + interval.minutes * 60 + interval.seconds;
  return true;
}

bool IntervalHasMixedSigns(const ParsedInterval& interval) {
  bool has_positive = false;
  bool has_negative = false;
  for (const std::int64_t value : {interval.years, interval.months, interval.days,
                                  interval.hours, interval.minutes, interval.seconds}) {
    has_positive = has_positive || value > 0;
    has_negative = has_negative || value < 0;
  }
  return has_positive && has_negative;
}

bool ParseTimezoneOffsetSeconds(std::string zone, std::int64_t* out) {
  if (out == nullptr) return false;
  zone = LowerAscii(TrimAscii(std::move(zone)));
  if (zone == "utc" || zone == "z" || zone == "+00:00" || zone == "-00:00" ||
      zone == "+0000" || zone == "-0000" || zone == "etc/utc" || zone == "gmt") {
    *out = 0;
    return true;
  }
  if ((zone.rfind("utc", 0) == 0 || zone.rfind("gmt", 0) == 0) && zone.size() > 3) {
    zone = zone.substr(3);
  }
  if (zone.size() != 3 && zone.size() != 5 && zone.size() != 6) return false;
  if (zone[0] != '+' && zone[0] != '-') return false;
  const std::int64_t sign = zone[0] == '-' ? -1 : 1;
  std::int64_t hours = 0;
  std::int64_t minutes = 0;
  if (!ParseFixedInt(zone, 1, 2, &hours)) return false;
  if (zone.size() == 3) {
    minutes = 0;
  } else if (zone.size() == 5) {
    if (!ParseFixedInt(zone, 3, 2, &minutes)) return false;
  } else {
    if (zone[3] != ':' || !ParseFixedInt(zone, 4, 2, &minutes)) return false;
  }
  if (hours > 23 || minutes > 59) return false;
  *out = sign * (hours * 3600 + minutes * 60);
  return true;
}

bool ExtractTimestampField(std::string field, const std::string& temporal, std::int64_t* out) {
  field = LowerAscii(std::move(field));
  if (out == nullptr) return false;
  try {
    if ((field == "year" || field == "years") && temporal.size() >= 4) {
      *out = std::stoll(temporal.substr(0, 4));
      return true;
    }
    if ((field == "month" || field == "months") && temporal.size() >= 7) {
      *out = std::stoll(temporal.substr(5, 2));
      return true;
    }
    if ((field == "day" || field == "days") && temporal.size() >= 10) {
      *out = std::stoll(temporal.substr(8, 2));
      return true;
    }
    if (field == "epoch" || field == "epochs") {
      if (temporal.size() < 10) return false;
      const std::int64_t y = std::stoll(temporal.substr(0, 4));
      const unsigned mo = static_cast<unsigned>(std::stoll(temporal.substr(5, 2)));
      const unsigned d = static_cast<unsigned>(std::stoll(temporal.substr(8, 2)));
      std::int64_t hh = 0;
      std::int64_t mm = 0;
      std::int64_t ss = 0;
      const auto separator = temporal.find_first_of("T ");
      if (separator != std::string::npos && separator + 8 < temporal.size()) {
        const std::string time = temporal.substr(separator + 1);
        hh = std::stoll(time.substr(0, 2));
        mm = std::stoll(time.substr(3, 2));
        ss = std::stoll(time.substr(6, 2));
      }
      *out = DaysFromCivil(y, mo, d) * 86400 + hh * 3600 + mm * 60 + ss;
      return true;
    }
    if (field == "dow" || field == "day_of_week") {
      if (temporal.size() < 10) return false;
      const std::int64_t y = std::stoll(temporal.substr(0, 4));
      const unsigned mo = static_cast<unsigned>(std::stoll(temporal.substr(5, 2)));
      const unsigned d = static_cast<unsigned>(std::stoll(temporal.substr(8, 2)));
      const std::int64_t days = DaysFromCivil(y, mo, d);
      // 1970-01-01 (days=0) was a Thursday. PostgreSQL convention: Sunday=0..Saturday=6.
      *out = ((days % 7) + 11) % 7;
      return true;
    }
    if (field == "doy" || field == "day_of_year") {
      if (temporal.size() < 10) return false;
      const std::int64_t y = std::stoll(temporal.substr(0, 4));
      const unsigned mo = static_cast<unsigned>(std::stoll(temporal.substr(5, 2)));
      const unsigned d = static_cast<unsigned>(std::stoll(temporal.substr(8, 2)));
      *out = DaysFromCivil(y, mo, d) - DaysFromCivil(y, 1, 1) + 1;
      return true;
    }
    if (field == "quarter" || field == "quarters") {
      if (temporal.size() < 7) return false;
      const std::int64_t mo = std::stoll(temporal.substr(5, 2));
      *out = (mo - 1) / 3 + 1;
      return true;
    }
    if (field == "isodow") {
      if (temporal.size() < 10) return false;
      const std::int64_t y = std::stoll(temporal.substr(0, 4));
      const unsigned mo = static_cast<unsigned>(std::stoll(temporal.substr(5, 2)));
      const unsigned d = static_cast<unsigned>(std::stoll(temporal.substr(8, 2)));
      const std::int64_t days = DaysFromCivil(y, mo, d);
      // ISO 8601: Monday=1..Sunday=7. 1970-01-01 (days=0) is Thursday=4.
      *out = (((days % 7) + 10) % 7) + 1;
      return true;
    }
    if (field == "week" || field == "weeks" || field == "isoweek") {
      if (temporal.size() < 10) return false;
      const std::int64_t y = std::stoll(temporal.substr(0, 4));
      const unsigned mo = static_cast<unsigned>(std::stoll(temporal.substr(5, 2)));
      const unsigned d = static_cast<unsigned>(std::stoll(temporal.substr(8, 2)));
      const std::int64_t days = DaysFromCivil(y, mo, d);
      const std::int64_t iso_dow = (((days % 7) + 10) % 7) + 1;
      // ISO 8601 algorithm: take the Thursday of the same ISO week; its year
      // is the ISO week-numbering year; week number is its day-of-year minus
      // 1 divided by 7 plus 1.
      const std::int64_t thursday_days = days + (4 - iso_dow);
      std::int64_t thursday_year = 0;
      unsigned thursday_month = 0;
      unsigned thursday_day = 0;
      CivilFromDays(thursday_days, &thursday_year, &thursday_month, &thursday_day);
      const std::int64_t jan1_days = DaysFromCivil(thursday_year, 1, 1);
      const std::int64_t thursday_doy = thursday_days - jan1_days + 1;
      *out = (thursday_doy - 1) / 7 + 1;
      return true;
    }
    const auto separator = temporal.find_first_of("T ");
    if (separator == std::string::npos || separator + 8 >= temporal.size()) return false;
    const std::string time = temporal.substr(separator + 1);
    if (field == "hour" || field == "hours") {
      *out = std::stoll(time.substr(0, 2));
      return true;
    }
    if (field == "minute" || field == "minutes") {
      *out = std::stoll(time.substr(3, 2));
      return true;
    }
    if (field == "second" || field == "seconds") {
      *out = std::stoll(time.substr(6, 2));
      return true;
    }
  } catch (...) {
    return false;
  }
  return false;
}

std::vector<std::string> SplitCollectionText(std::string text) {
  text = TrimAscii(std::move(text));
  if (text.size() >= 2 && ((text.front() == '[' && text.back() == ']') || (text.front() == '(' && text.back() == ')'))) {
    text = text.substr(1, text.size() - 2);
  }
  std::vector<std::string> out;
  std::string current;
  bool in_string = false;
  for (char ch : text) {
    if (ch == '"') in_string = !in_string;
    if (ch == ',' && !in_string) {
      out.push_back(TrimAscii(current));
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty() || !text.empty()) out.push_back(TrimAscii(current));
  return out;
}

std::string JsonArrayFromArguments(const std::vector<FunctionArgument>& arguments) {
  std::string out = "[";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) out += ",";
    if (IsSqlNull(arguments[i].value)) {
      out += "null";
    } else {
      out += ValueAsText(arguments[i].value);
    }
  }
  out += "]";
  return out;
}

std::string AlphaUpper(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if (std::isalpha(ch)) out.push_back(static_cast<char>(std::toupper(ch)));
  }
  return out;
}

bool IsVowel(char ch) {
  ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return ch == 'A' || ch == 'E' || ch == 'I' || ch == 'O' || ch == 'U' || ch == 'Y';
}

char SoundexCode(char ch) {
  switch (std::toupper(static_cast<unsigned char>(ch))) {
    case 'B':
    case 'F':
    case 'P':
    case 'V':
      return '1';
    case 'C':
    case 'G':
    case 'J':
    case 'K':
    case 'Q':
    case 'S':
    case 'X':
    case 'Z':
      return '2';
    case 'D':
    case 'T':
      return '3';
    case 'L':
      return '4';
    case 'M':
    case 'N':
      return '5';
    case 'R':
      return '6';
    default:
      return '0';
  }
}

std::string SoundexText(const std::string& input) {
  std::size_t i = 0;
  while (i < input.size() && !std::isalpha(static_cast<unsigned char>(input[i]))) ++i;
  if (i == input.size()) return {};

  std::string out(1, static_cast<char>(std::toupper(static_cast<unsigned char>(input[i]))));
  ++i;
  for (; i < input.size() && out.size() < 4; ++i) {
    const unsigned char ch = static_cast<unsigned char>(input[i]);
    if (!std::isalpha(ch)) continue;
    const char code = SoundexCode(input[i]);
    const char previous = SoundexCode(input[i - 1]);
    if (code != previous && code != '0') out.push_back(code);
  }
  while (out.size() < 4) out.push_back('0');
  return out;
}

bool StartsWithText(const std::string& text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool NextIs(const std::string& text, std::size_t index, char ch) {
  return index + 1 < text.size() && text[index + 1] == ch;
}

char LookAhead(const std::string& text, std::size_t index, std::size_t distance) {
  return index + distance < text.size() ? text[index + distance] : '\0';
}

char LookBack(const std::string& text, std::size_t index, std::size_t distance) {
  return index >= distance ? text[index - distance] : '\0';
}

bool IsBreak(char ch) {
  return ch == '\0' || !std::isalpha(static_cast<unsigned char>(ch));
}

std::string MetaphoneText(const std::string& input, std::size_t max_length = 0) {
  std::string text = AlphaUpper(input);
  if (text.empty()) return {};
  if (StartsWithText(text, "KN") || StartsWithText(text, "GN") ||
      StartsWithText(text, "PN") || StartsWithText(text, "AE") ||
      StartsWithText(text, "WR") || StartsWithText(text, "PS")) {
    text.erase(text.begin());
  } else if (StartsWithText(text, "WH")) {
    text.erase(text.begin());
  } else if (text.front() == 'X') {
    text.front() = 'S';
  }

  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    const char prev = i == 0 ? '\0' : text[i - 1];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    const char after_next = LookAhead(text, i, 2);
    if (i > 0 && ch == prev && ch != 'C') continue;
    if (max_length != 0 && out.size() >= max_length) break;

    switch (ch) {
      case 'A':
      case 'E':
      case 'I':
      case 'O':
      case 'U':
        if (i == 0) out.push_back(ch);
        break;
      case 'B':
        if (prev != 'M') out.push_back('B');
        break;
      case 'C':
        if (next == 'I' && after_next == 'A') {
          out.push_back('X');
        } else if ((next == 'I' || next == 'E' || next == 'Y') && prev == 'S') {
          break;
        } else if (next == 'H') {
          out.push_back((after_next == 'R' || prev == 'S') ? 'K' : 'X');
          ++i;
        } else if (next == 'I' || next == 'E' || next == 'Y') {
          out.push_back('S');
        } else {
          out.push_back('K');
        }
        break;
      case 'D':
        if (next == 'G' && i + 2 < text.size() &&
            (text[i + 2] == 'E' || text[i + 2] == 'I' || text[i + 2] == 'Y')) {
          out.push_back('J');
          i += 2;
        } else {
          out.push_back('T');
        }
        break;
      case 'G':
        if (next == 'H') {
          if (!((i >= 3 && (LookBack(text, i, 3) == 'B' ||
                            LookBack(text, i, 3) == 'D' ||
                            LookBack(text, i, 3) == 'H')) ||
                (i >= 4 && LookBack(text, i, 4) == 'H'))) {
            out.push_back('F');
          }
          ++i;
        } else if (next == 'N' && (IsBreak(after_next) || (after_next == 'E' && LookAhead(text, i, 3) == 'D'))) {
          break;
        } else if (next == 'E' || next == 'I' || next == 'Y') {
          out.push_back('J');
        } else {
          out.push_back('K');
        }
        break;
      case 'H':
        if ((i == 0 || !IsVowel(prev)) && IsVowel(next)) out.push_back('H');
        break;
      case 'K':
        if (prev != 'C') out.push_back('K');
        break;
      case 'P':
        out.push_back(next == 'H' ? 'F' : 'P');
        if (next == 'H') ++i;
        break;
      case 'Q':
        out.push_back('K');
        break;
      case 'S':
        if (next == 'H') {
          out.push_back('X');
          ++i;
        } else if (next == 'I' && i + 2 < text.size() && (text[i + 2] == 'O' || text[i + 2] == 'A')) {
          out.push_back('X');
        } else {
          out.push_back('S');
        }
        break;
      case 'T':
        if (next == 'H') {
          out.push_back('0');
          ++i;
        } else if (next == 'I' && (after_next == 'O' || after_next == 'A')) {
          out.push_back('X');
        } else {
          out.push_back('T');
        }
        break;
      case 'V':
        out.push_back('F');
        break;
      case 'W':
      case 'Y':
        if (IsVowel(next)) out.push_back(ch);
        break;
      case 'X':
        out += "KS";
        break;
      case 'Z':
        out.push_back('S');
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  if (max_length != 0 && out.size() > max_length) out.resize(max_length);
  return out;
}

struct DoubleMetaphoneCodes {
  std::string primary;
  std::string alternate;
};

bool DmAt(const std::string& text, std::size_t pos, std::size_t length, std::initializer_list<std::string_view> choices) {
  if (pos >= text.size() || length > text.size() - pos) return false;
  for (const auto choice : choices) {
    if (choice.size() == length && text.compare(pos, length, choice) == 0) return true;
  }
  return false;
}

char DmChar(const std::string& text, std::size_t pos) {
  return pos < text.size() ? text[pos] : '\0';
}

void DmAdd(DoubleMetaphoneCodes* codes, std::string_view primary, std::string_view alternate) {
  if (codes->primary.size() < 4) codes->primary.append(primary.substr(0, 4 - codes->primary.size()));
  if (codes->alternate.size() < 4) codes->alternate.append(alternate.substr(0, 4 - codes->alternate.size()));
}

void DmAddSame(DoubleMetaphoneCodes* codes, std::string_view value) {
  DmAdd(codes, value, value);
}

bool DmSlavoGermanic(const std::string& text) {
  return text.find('W') != std::string::npos || text.find('K') != std::string::npos ||
         text.find("CZ") != std::string::npos || text.find("WITZ") != std::string::npos;
}

DoubleMetaphoneCodes DoubleMetaphoneCodesForText(const std::string& input) {
  std::string text = AlphaUpper(input);
  DoubleMetaphoneCodes codes;
  if (text.empty()) return codes;

  const bool slavo_germanic = DmSlavoGermanic(text);
  std::size_t current = 0;
  const std::size_t last = text.size() - 1;

  if (DmAt(text, 0, 2, {"GN", "KN", "PN", "WR", "PS"})) current = 1;
  if (DmChar(text, 0) == 'X') {
    DmAddSame(&codes, "S");
    current = 1;
  }

  while ((codes.primary.size() < 4 || codes.alternate.size() < 4) && current < text.size()) {
    const char ch = DmChar(text, current);
    switch (ch) {
      case 'A':
      case 'E':
      case 'I':
      case 'O':
      case 'U':
      case 'Y':
        if (current == 0) DmAddSame(&codes, "A");
        ++current;
        break;
      case 'B':
        DmAddSame(&codes, "P");
        current += DmChar(text, current + 1) == 'B' ? 2 : 1;
        break;
      case 'C':
        if (current > 1 && !IsVowel(DmChar(text, current - 2)) && DmAt(text, current - 1, 3, {"ACH"}) &&
            DmChar(text, current + 2) != 'I' &&
            (DmChar(text, current + 2) != 'E' || DmAt(text, current - 2, 6, {"BACHER", "MACHER"}))) {
          DmAddSame(&codes, "K");
          current += 2;
        } else if (current == 0 && DmAt(text, current, 6, {"CAESAR"})) {
          DmAddSame(&codes, "S");
          current += 2;
        } else if (DmAt(text, current, 4, {"CHIA"})) {
          DmAddSame(&codes, "K");
          current += 2;
        } else if (DmAt(text, current, 2, {"CH"})) {
          if (current > 0 && DmAt(text, current, 4, {"CHAE"})) {
            DmAdd(&codes, "K", "X");
          } else if ((current == 0 &&
                      (DmAt(text, current + 1, 5, {"HARAC", "HARIS"}) ||
                       DmAt(text, current + 1, 3, {"HOR", "HYM", "HIA", "HEM"})) &&
                      !DmAt(text, 0, 5, {"CHORE"})) ||
                     DmAt(text, 0, 4, {"VAN ", "VON "}) || DmAt(text, 0, 3, {"SCH"}) ||
                     DmAt(text, current - (current >= 2 ? 2 : 0), 6, {"ORCHES", "ARCHIT", "ORCHID"}) ||
                     DmAt(text, current + 2, 1, {"T", "S"})) {
            DmAddSame(&codes, "K");
          } else {
            DmAdd(&codes, current == 0 ? "X" : "X", current == 0 ? "X" : "K");
          }
          current += 2;
        } else if (DmAt(text, current, 2, {"CZ"}) && !(current >= 2 && DmAt(text, current - 2, 4, {"WICZ"}))) {
          DmAdd(&codes, "S", "X");
          current += 2;
        } else if (DmAt(text, current + 1, 3, {"CIA"})) {
          DmAddSame(&codes, "X");
          current += 3;
        } else if (DmAt(text, current, 2, {"CC"}) && !(current == 1 && DmChar(text, 0) == 'M')) {
          if (DmAt(text, current + 2, 1, {"I", "E", "H"}) && !DmAt(text, current + 2, 2, {"HU"})) {
            if ((current == 1 && DmChar(text, current - 1) == 'A') ||
                DmAt(text, current - 1, 5, {"UCCEE", "UCCES"})) {
              DmAddSame(&codes, "KS");
            } else {
              DmAddSame(&codes, "X");
            }
            current += 3;
          } else {
            DmAddSame(&codes, "K");
            current += 2;
          }
        } else if (DmAt(text, current, 2, {"CK", "CG", "CQ"})) {
          DmAddSame(&codes, "K");
          current += 2;
        } else if (DmAt(text, current, 2, {"CI", "CE", "CY"})) {
          DmAt(text, current, 3, {"CIO", "CIE", "CIA"}) ? DmAdd(&codes, "S", "X") : DmAddSame(&codes, "S");
          current += 2;
        } else {
          DmAddSame(&codes, "K");
          current += (DmAt(text, current + 1, 2, {" C", " Q", " G"}) ? 3 :
                      (DmAt(text, current + 1, 1, {"C", "K", "Q"}) &&
                       !DmAt(text, current + 1, 2, {"CE", "CI"}) ? 2 : 1));
        }
        break;
      case 'D':
        if (DmAt(text, current, 2, {"DG"})) {
          if (DmAt(text, current + 2, 1, {"I", "E", "Y"})) {
            DmAddSame(&codes, "J");
            current += 3;
          } else {
            DmAddSame(&codes, "TK");
            current += 2;
          }
        } else {
          DmAddSame(&codes, "T");
          current += DmAt(text, current, 2, {"DT", "DD"}) ? 2 : 1;
        }
        break;
      case 'F':
        DmAddSame(&codes, "F");
        current += DmChar(text, current + 1) == 'F' ? 2 : 1;
        break;
      case 'G':
        if (DmChar(text, current + 1) == 'H') {
          if (current > 0 && !IsVowel(DmChar(text, current - 1))) {
            DmAddSame(&codes, "K");
          } else if (current == 0) {
            DmAddSame(&codes, DmChar(text, current + 2) == 'I' ? "J" : "K");
          } else if (!((current > 1 && DmAt(text, current - 2, 1, {"B", "H", "D"})) ||
                       (current > 2 && DmAt(text, current - 3, 1, {"B", "H", "D"})) ||
                       (current > 3 && DmAt(text, current - 4, 1, {"B", "H"})))) {
            if (current > 2 && DmChar(text, current - 1) == 'U' &&
                DmAt(text, current - 3, 1, {"C", "G", "L", "R", "T"})) {
              DmAddSame(&codes, "F");
            } else if (current > 0 && DmChar(text, current - 1) != 'I') {
              DmAddSame(&codes, "K");
            }
          }
          current += 2;
        } else if (DmChar(text, current + 1) == 'N') {
          if (current == 1 && IsVowel(DmChar(text, 0)) && !slavo_germanic) {
            DmAdd(&codes, "KN", "N");
          } else if (!DmAt(text, current + 2, 2, {"EY"}) && DmChar(text, current + 1) != 'Y' && !slavo_germanic) {
            DmAdd(&codes, "N", "KN");
          } else {
            DmAddSame(&codes, "KN");
          }
          current += 2;
        } else if (DmAt(text, current + 1, 2, {"LI"}) && !slavo_germanic) {
          DmAdd(&codes, "KL", "L");
          current += 2;
        } else if (DmAt(text, current + 1, 1, {"E", "I", "Y"}) ||
                   (current >= 1 && DmAt(text, current - 1, 4, {"AGGI", "OGGI"}))) {
          if (DmAt(text, 0, 4, {"VAN ", "VON "}) || DmAt(text, 0, 3, {"SCH"}) ||
              DmAt(text, current + 1, 2, {"ET"})) {
            DmAddSame(&codes, "K");
          } else {
            DmAdd(&codes, DmAt(text, current + 1, 4, {"IER "}) ? "J" : "J", "K");
          }
          current += 2;
        } else {
          DmAddSame(&codes, "K");
          current += DmChar(text, current + 1) == 'G' ? 2 : 1;
        }
        break;
      case 'H':
        if ((current == 0 || IsVowel(DmChar(text, current - 1))) && IsVowel(DmChar(text, current + 1))) {
          DmAddSame(&codes, "H");
          current += 2;
        } else {
          ++current;
        }
        break;
      case 'J':
        if (DmAt(text, current, 4, {"JOSE"}) || DmAt(text, 0, 4, {"SAN "})) {
          ((current == 0 && DmChar(text, current + 4) == ' ') || DmAt(text, 0, 4, {"SAN "})) ? DmAddSame(&codes, "H")
                                                                                              : DmAdd(&codes, "J", "H");
        } else if (current == 0) {
          DmAdd(&codes, "J", "A");
        } else if (IsVowel(DmChar(text, current - 1)) && !slavo_germanic && DmAt(text, current + 1, 1, {"A", "O"})) {
          DmAdd(&codes, "J", "H");
        } else if (current == last) {
          DmAdd(&codes, "J", "");
        } else if (!DmAt(text, current + 1, 1, {"L", "T", "K", "S", "N", "M", "B", "Z"}) &&
                   !(current > 0 && DmAt(text, current - 1, 1, {"S", "K", "L"}))) {
          DmAddSame(&codes, "J");
        }
        current += DmChar(text, current + 1) == 'J' ? 2 : 1;
        break;
      case 'K':
        DmAddSame(&codes, "K");
        current += DmChar(text, current + 1) == 'K' ? 2 : 1;
        break;
      case 'L':
        DmAddSame(&codes, "L");
        current += DmChar(text, current + 1) == 'L' ? 2 : 1;
        break;
      case 'M':
        DmAddSame(&codes, "M");
        current += (DmAt(text, current - (current > 0 ? 1 : 0), 3, {"UMB"}) &&
                    (current + 1 == last || DmAt(text, current + 2, 2, {"ER"}))) ||
                   DmChar(text, current + 1) == 'M'
                       ? 2
                       : 1;
        break;
      case 'N':
        DmAddSame(&codes, "N");
        current += DmChar(text, current + 1) == 'N' ? 2 : 1;
        break;
      case 'P':
        DmAddSame(&codes, DmChar(text, current + 1) == 'H' ? "F" : "P");
        current += DmAt(text, current + 1, 1, {"P", "B"}) || DmChar(text, current + 1) == 'H' ? 2 : 1;
        break;
      case 'Q':
        DmAddSame(&codes, "K");
        current += DmChar(text, current + 1) == 'Q' ? 2 : 1;
        break;
      case 'R':
        DmAddSame(&codes, "R");
        current += DmChar(text, current + 1) == 'R' ? 2 : 1;
        break;
      case 'S':
        if (DmAt(text, current - (current >= 1 ? 1 : 0), 3, {"ISL", "YSL"})) {
          ++current;
        } else if (current == 0 && DmAt(text, current, 5, {"SUGAR"})) {
          DmAdd(&codes, "X", "S");
          ++current;
        } else if (DmAt(text, current, 2, {"SH"})) {
          DmAddSame(&codes, DmAt(text, current + 1, 4, {"HEIM", "HOEK", "HOLM", "HOLZ"}) ? "S" : "X");
          current += 2;
        } else if (DmAt(text, current, 3, {"SIO", "SIA"}) || DmAt(text, current, 4, {"SIAN"})) {
          slavo_germanic ? DmAddSame(&codes, "S") : DmAdd(&codes, "S", "X");
          current += 3;
        } else if (DmAt(text, current, 3, {"SCH"})) {
          if (DmAt(text, current + 3, 2, {"OO", "ER", "EN", "UY", "ED", "EM"})) {
            DmAdd(&codes, "SK", DmAt(text, current + 3, 2, {"ER", "EN"}) ? "X" : "SK");
          } else {
            DmAdd(&codes, current == 0 && !IsVowel(DmChar(text, 3)) && DmChar(text, 3) != 'W' ? "S" : "X", "S");
          }
          current += 3;
        } else {
          DmAddSame(&codes, "S");
          current += DmAt(text, current + 1, 1, {"S", "Z"}) ? 2 : 1;
        }
        break;
      case 'T':
        if (DmAt(text, current, 4, {"TION"})) {
          DmAddSame(&codes, "X");
          current += 3;
        } else if (DmAt(text, current, 3, {"TIA", "TCH"})) {
          DmAddSame(&codes, "X");
          current += 3;
        } else if (DmAt(text, current, 2, {"TH"}) || DmAt(text, current, 3, {"TTH"})) {
          if (DmAt(text, current + 2, 2, {"OM", "AM"}) || DmAt(text, 0, 4, {"VAN ", "VON "}) || DmAt(text, 0, 3, {"SCH"})) {
            DmAddSame(&codes, "T");
          } else {
            DmAdd(&codes, "0", "T");
          }
          current += 2;
        } else {
          DmAddSame(&codes, "T");
          current += DmAt(text, current + 1, 1, {"T", "D"}) ? 2 : 1;
        }
        break;
      case 'V':
        DmAddSame(&codes, "F");
        current += DmChar(text, current + 1) == 'V' ? 2 : 1;
        break;
      case 'W':
        if (DmAt(text, current, 2, {"WR"})) {
          DmAddSame(&codes, "R");
          current += 2;
        } else if (current == 0 && (IsVowel(DmChar(text, current + 1)) || DmAt(text, current, 2, {"WH"}))) {
          IsVowel(DmChar(text, current + 1)) ? DmAdd(&codes, "A", "F") : DmAddSame(&codes, "A");
          ++current;
        } else {
          if ((current == last && IsVowel(DmChar(text, current - 1))) ||
              DmAt(text, current - (current >= 1 ? 1 : 0), 5, {"EWSKI", "EWSKY", "OWSKI", "OWSKY"}) ||
              DmAt(text, 0, 3, {"SCH"})) {
            DmAdd(&codes, "", "F");
          } else if (DmAt(text, current, 4, {"WICZ", "WITZ"})) {
            DmAdd(&codes, "TS", "FX");
            current += 4;
            break;
          }
          ++current;
        }
        break;
      case 'X':
        DmAddSame(&codes, "KS");
        current += DmAt(text, current + 1, 1, {"C", "X"}) ? 2 : 1;
        break;
      case 'Z':
        if (DmChar(text, current + 1) == 'H') {
          DmAddSame(&codes, "J");
          current += 2;
        } else {
          (DmAt(text, current + 1, 2, {"ZO", "ZI", "ZA"}) || slavo_germanic) ? DmAdd(&codes, "S", "TS")
                                                                               : DmAddSame(&codes, "S");
          current += DmChar(text, current + 1) == 'Z' ? 2 : 1;
        }
        break;
      default:
        ++current;
        break;
    }
  }
  return codes;
}

std::string DoubleMetaphoneText(const std::string& input, bool alternate) {
  const auto codes = DoubleMetaphoneCodesForText(input);
  return alternate ? codes.alternate : codes.primary;
}

bool EditDistanceSizeAllowed(std::size_t left, std::size_t right) {
  return (left + 1) <= 1000001 / (right + 1);
}

std::int64_t LevenshteinDistance(const std::string& left,
                                 const std::string& right,
                                 std::int64_t insert_cost,
                                 std::int64_t delete_cost,
                                 std::int64_t substitute_cost,
                                 std::int64_t max_distance = -1) {
  const std::size_t n = left.size();
  const std::size_t m = right.size();
  std::vector<std::int64_t> previous(m + 1, 0);
  std::vector<std::int64_t> current(m + 1, 0);
  for (std::size_t j = 1; j <= m; ++j) previous[j] = previous[j - 1] + insert_cost;
  for (std::size_t i = 1; i <= n; ++i) {
    current[0] = previous[0] + delete_cost;
    std::int64_t row_min = current[0];
    for (std::size_t j = 1; j <= m; ++j) {
      const std::int64_t substitution = left[i - 1] == right[j - 1] ? 0 : substitute_cost;
      current[j] = std::min({previous[j] + delete_cost,
                             current[j - 1] + insert_cost,
                             previous[j - 1] + substitution});
      row_min = std::min(row_min, current[j]);
    }
    if (max_distance >= 0 && row_min > max_distance) return max_distance + 1;
    std::swap(previous, current);
  }
  if (max_distance >= 0 && previous[m] > max_distance) return max_distance + 1;
  return previous[m];
}

std::int64_t DamerauLevenshteinDistance(const std::string& left, const std::string& right) {
  const std::size_t n = left.size();
  const std::size_t m = right.size();
  std::vector<std::vector<std::int64_t>> distance(n + 1, std::vector<std::int64_t>(m + 1, 0));
  for (std::size_t i = 0; i <= n; ++i) distance[i][0] = static_cast<std::int64_t>(i);
  for (std::size_t j = 0; j <= m; ++j) distance[0][j] = static_cast<std::int64_t>(j);
  for (std::size_t i = 1; i <= n; ++i) {
    for (std::size_t j = 1; j <= m; ++j) {
      const std::int64_t cost = left[i - 1] == right[j - 1] ? 0 : 1;
      distance[i][j] = std::min({distance[i - 1][j] + 1,
                                 distance[i][j - 1] + 1,
                                 distance[i - 1][j - 1] + cost});
      if (i > 1 && j > 1 && left[i - 1] == right[j - 2] && left[i - 2] == right[j - 1]) {
        distance[i][j] = std::min(distance[i][j], distance[i - 2][j - 2] + 1);
      }
    }
  }
  return distance[n][m];
}

double JaroSimilarity(const std::string& left, const std::string& right) {
  if (left == right) return 1.0;
  if (left.empty() || right.empty()) return 0.0;
  const std::size_t match_distance = std::max(left.size(), right.size()) / 2;
  const std::size_t window = match_distance == 0 ? 0 : match_distance - 1;
  std::vector<int> left_matches(left.size(), 0);
  std::vector<int> right_matches(right.size(), 0);
  std::size_t matches = 0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const std::size_t start = i > window ? i - window : 0;
    const std::size_t end = std::min(i + window + 1, right.size());
    for (std::size_t j = start; j < end; ++j) {
      if (right_matches[j] || left[i] != right[j]) continue;
      left_matches[i] = 1;
      right_matches[j] = 1;
      ++matches;
      break;
    }
  }
  if (matches == 0) return 0.0;
  std::size_t k = 0;
  std::size_t transpositions = 0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (!left_matches[i]) continue;
    while (!right_matches[k]) ++k;
    if (left[i] != right[k]) ++transpositions;
    ++k;
  }
  const double m = static_cast<double>(matches);
  return (m / static_cast<double>(left.size()) +
          m / static_cast<double>(right.size()) +
          (m - static_cast<double>(transpositions) / 2.0) / m) / 3.0;
}

double JaroWinklerSimilarity(const std::string& left, const std::string& right) {
  const double jaro = JaroSimilarity(left, right);
  std::size_t prefix = 0;
  while (prefix < 4 && prefix < left.size() && prefix < right.size() && left[prefix] == right[prefix]) ++prefix;
  return jaro + static_cast<double>(prefix) * 0.1 * (1.0 - jaro);
}

std::string TrigramNormalizedText(const std::string& input) {
  std::string out;
  bool in_space = true;
  for (unsigned char ch : input) {
    if (std::isalnum(ch)) {
      out.push_back(static_cast<char>(std::tolower(ch)));
      in_space = false;
    } else if (!in_space) {
      out.push_back(' ');
      in_space = true;
    }
  }
  return RTrimAscii(std::move(out));
}

void AddUnique(std::vector<std::string>* values, std::string value) {
  if (std::find(values->begin(), values->end(), value) == values->end()) values->push_back(std::move(value));
}

std::vector<std::string> TrigramsForText(const std::string& input) {
  const std::string normalized = TrigramNormalizedText(input);
  std::vector<std::string> trigrams;
  std::string word;
  for (std::size_t i = 0; i <= normalized.size(); ++i) {
    const char ch = i < normalized.size() ? normalized[i] : ' ';
    if (ch != ' ') {
      word.push_back(ch);
      continue;
    }
    if (!word.empty()) {
      const std::string padded = "  " + word + " ";
      for (std::size_t j = 0; j + 3 <= padded.size(); ++j) AddUnique(&trigrams, padded.substr(j, 3));
      word.clear();
    }
  }
  return trigrams;
}

double TrigramJaccard(const std::vector<std::string>& left, const std::vector<std::string>& right) {
  if (left.empty() && right.empty()) return 1.0;
  std::size_t intersection = 0;
  for (const auto& trigram : left) {
    if (std::find(right.begin(), right.end(), trigram) != right.end()) ++intersection;
  }
  const std::size_t union_count = left.size() + right.size() - intersection;
  return union_count == 0 ? 0.0 : static_cast<double>(intersection) / static_cast<double>(union_count);
}

double TrigramSimilarity(const std::string& left, const std::string& right) {
  return TrigramJaccard(TrigramsForText(left), TrigramsForText(right));
}

double WordTrigramSimilarity(const std::string& left, const std::string& right) {
  const auto left_trigrams = TrigramsForText(left);
  const auto right_trigrams = TrigramsForText(right);
  if (left_trigrams.empty() || right_trigrams.empty()) return left_trigrams.empty() && right_trigrams.empty() ? 1.0 : 0.0;
  double best = 0.0;
  for (std::size_t start = 0; start < right_trigrams.size(); ++start) {
    std::vector<std::string> extent;
    for (std::size_t end = start; end < right_trigrams.size(); ++end) {
      AddUnique(&extent, right_trigrams[end]);
      best = std::max(best, TrigramJaccard(left_trigrams, extent));
    }
  }
  return best;
}

std::string LeftTextSlice(const std::string& value, std::int64_t count) {
  if (count == 0) return {};
  if (count > 0) return value.substr(0, static_cast<std::size_t>(std::min<std::int64_t>(count, value.size())));
  const std::uint64_t drop = Int64Magnitude(count);
  if (drop >= value.size()) return {};
  return value.substr(0, value.size() - static_cast<std::size_t>(drop));
}

std::string RightTextSlice(const std::string& value, std::int64_t count) {
  if (count == 0) return {};
  if (count > 0) {
    const std::size_t take = static_cast<std::size_t>(std::min<std::int64_t>(count, value.size()));
    return value.substr(value.size() - take);
  }
  const std::uint64_t drop = Int64Magnitude(count);
  if (drop >= value.size()) return {};
  return value.substr(static_cast<std::size_t>(drop));
}

std::string EscapeEncodeBytes(const std::vector<std::uint8_t>& bytes) {
  std::string out;
  for (const auto byte : bytes) {
    if (byte == '\\') {
      out += "\\\\";
    } else if (byte >= 0x20 && byte <= 0x7e) {
      out.push_back(static_cast<char>(byte));
    } else {
      out.push_back('\\');
      out.push_back(static_cast<char>('0' + ((byte >> 6) & 0x07)));
      out.push_back(static_cast<char>('0' + ((byte >> 3) & 0x07)));
      out.push_back(static_cast<char>('0' + (byte & 0x07)));
    }
  }
  return out;
}

bool EscapeDecodeBytes(const std::string& text, std::vector<std::uint8_t>* out) {
  out->clear();
  for (std::size_t i = 0; i < text.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch != '\\') {
      out->push_back(ch);
      continue;
    }
    if (i + 1 >= text.size()) return false;
    if (text[i + 1] == '\\') {
      out->push_back('\\');
      ++i;
      continue;
    }
    if (i + 3 >= text.size()) return false;
    const int a = text[i + 1] >= '0' && text[i + 1] <= '7' ? text[i + 1] - '0' : -1;
    const int b = text[i + 2] >= '0' && text[i + 2] <= '7' ? text[i + 2] - '0' : -1;
    const int c = text[i + 3] >= '0' && text[i + 3] <= '7' ? text[i + 3] - '0' : -1;
    if (a < 0 || b < 0 || c < 0) return false;
    out->push_back(static_cast<std::uint8_t>((a << 6) | (b << 3) | c));
    i += 3;
  }
  return true;
}

bool CanonicalBase64Text(const std::string& text) {
  if ((text.size() % 4) != 0) return false;
  std::size_t padding = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '=') {
      ++padding;
      if (padding > 2) return false;
      const std::size_t pos = i % 4;
      if (pos < 2) return false;
      continue;
    }
    if (padding != 0 || Base64Value(ch) < 0) return false;
  }
  return true;
}

bool CanonicalizeUuidText(const std::string& text, std::string* out) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return false;
  }
  std::string canonical;
  canonical.reserve(36);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      canonical.push_back('-');
      continue;
    }
    if (HexValue(ch) < 0) return false;
    canonical.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  *out = std::move(canonical);
  return true;
}

bool LooksIpv4(const std::string& value) {
  int dots = 0;
  int digits = 0;
  for (char ch : value) {
    if (ch == '.') {
      if (digits == 0) return false;
      ++dots;
      digits = 0;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    ++digits;
