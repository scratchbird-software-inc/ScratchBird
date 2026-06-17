// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
      if (!ValueAsUint64Strict(value, &parsed)) return RefuseFunctionInvalidInput(request, "cast to uint64 requires exact non-negative integer text/value");
      if (target_descriptor == "uint32" &&
          parsed > std::numeric_limits<std::uint32_t>::max()) {
        return RefuseFunctionInvalidInput(request, "cast to uint32 is outside the uint32 range");
      }
      if (target_descriptor == "uint16" &&
          parsed > std::numeric_limits<std::uint16_t>::max()) {
        return RefuseFunctionInvalidInput(request, "cast to uint16 is outside the uint16 range");
      }
      if (target_descriptor == "uint8" &&
          parsed > std::numeric_limits<std::uint8_t>::max()) {
        return RefuseFunctionInvalidInput(request, "cast to uint8 is outside the uint8 range");
      }
      const std::string descriptor =
          target_descriptor == "unsigned_bigint" ? "uint64" : target_descriptor;
      return MakeFunctionSuccess(request, {MakeUint64Value(descriptor, parsed)});
    }
    if (target_descriptor == "real64" || target_descriptor == "double" ||
        target_descriptor == "float64") {
      double parsed = 0.0;
      if (!ValueAsReal64(value, &parsed)) return RefuseFunctionInvalidInput(request, "cast to real64 requires numeric text/value");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", parsed)});
    }
    if (target_descriptor == "boolean" || target_descriptor == "bool") {
      bool parsed = false;
      if (!ParseBooleanText(value, &parsed)) return RefuseFunctionInvalidInput(request, "cast to boolean requires boolean-compatible text/value");
      return MakeFunctionSuccess(request, {MakeInt64Value("boolean", parsed ? 1 : 0)});
    }
    if (target_descriptor == "uuid") {
      std::string canonical;
      if (!CanonicalizeUuidText(ValueAsText(value), &canonical)) {
        return RefuseFunctionInvalidInput(request, "cast to uuid expects canonical UUID text");
      }
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", std::move(canonical))});
    }
    if (target_descriptor == "date" || target_descriptor == "time" ||
        target_descriptor == "timestamp" || target_descriptor == "timestamp_tz" ||
        target_descriptor == "interval" || target_descriptor == "json" ||
        target_descriptor == "json_document") {
      return MakeFunctionSuccess(request, {MakeTextValue(target_descriptor, ValueAsText(value))});
    }
    return RefuseFunctionInvalidInput(request, "cast target descriptor id is not supported by the scalar runtime");
  }

  if (IdIs(id, {"data.scalar.safe_cast", "sb.scalar.safe_cast", "safe_cast",
                "data.scalar.try_cast", "sb.scalar.try_cast", "try_cast"})) {
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "safe_cast/try_cast expects value and target descriptor text");
    }
    if (IsSqlNull(request.arguments[1].value)) {
      return RefuseFunctionInvalidInput(request, "safe_cast/try_cast target descriptor is required");
    }
    const auto target_descriptor = LowerAscii(TrimAscii(ValueAsText(request.arguments[1].value)));
    if (target_descriptor.empty()) {
      return RefuseFunctionInvalidInput(request, "safe_cast/try_cast target descriptor is required");
    }
    const auto& value = request.arguments[0].value;
    auto null_result = [&](std::string descriptor) {
      return MakeFunctionSuccess(request, {MakeNullValue(std::move(descriptor))});
    };
    if (target_descriptor == "character" || target_descriptor == "varchar" ||
        target_descriptor == "text" || target_descriptor == "string") {
      if (IsSqlNull(value)) return null_result(target_descriptor);
      return MakeFunctionSuccess(request, {MakeTextValue(target_descriptor, ValueAsText(value))});
    }
    if (target_descriptor == "int64" || target_descriptor == "integer" ||
        target_descriptor == "bigint" || target_descriptor == "int32" ||
        target_descriptor == "int16" || target_descriptor == "int8") {
      const std::string descriptor =
          (target_descriptor == "integer" || target_descriptor == "bigint") ? "int64"
                                                                            : target_descriptor;
      if (IsSqlNull(value)) return null_result(descriptor);
      std::int64_t parsed = 0;
      if (!ValueAsInt64Strict(value, &parsed)) return null_result(descriptor);
      if (target_descriptor == "int32" &&
          (parsed < std::numeric_limits<std::int32_t>::min() ||
           parsed > std::numeric_limits<std::int32_t>::max())) {
        return null_result(descriptor);
      }
      if (target_descriptor == "int16" &&
          (parsed < std::numeric_limits<std::int16_t>::min() ||
           parsed > std::numeric_limits<std::int16_t>::max())) {
        return null_result(descriptor);
      }
      if (target_descriptor == "int8" &&
          (parsed < std::numeric_limits<std::int8_t>::min() ||
           parsed > std::numeric_limits<std::int8_t>::max())) {
        return null_result(descriptor);
      }
      return MakeFunctionSuccess(request, {MakeInt64Value(descriptor, parsed)});
    }
    if (target_descriptor == "uint64" || target_descriptor == "unsigned_bigint" ||
        target_descriptor == "uint32" || target_descriptor == "uint16" ||
        target_descriptor == "uint8") {
      const std::string descriptor =
          target_descriptor == "unsigned_bigint" ? "uint64" : target_descriptor;
      if (IsSqlNull(value)) return null_result(descriptor);
      std::uint64_t parsed = 0;
      if (!ValueAsUint64Strict(value, &parsed)) return null_result(descriptor);
      if (target_descriptor == "uint32" &&
          parsed > std::numeric_limits<std::uint32_t>::max()) {
        return null_result(descriptor);
      }
      if (target_descriptor == "uint16" &&
          parsed > std::numeric_limits<std::uint16_t>::max()) {
        return null_result(descriptor);
      }
      if (target_descriptor == "uint8" &&
          parsed > std::numeric_limits<std::uint8_t>::max()) {
        return null_result(descriptor);
      }
      return MakeFunctionSuccess(request, {MakeUint64Value(descriptor, parsed)});
    }
    if (target_descriptor == "real64" || target_descriptor == "double" ||
        target_descriptor == "float64") {
      if (IsSqlNull(value)) return null_result("real64");
      double parsed = 0.0;
      if (!ValueAsReal64(value, &parsed)) return null_result("real64");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", parsed)});
    }
    if (target_descriptor == "boolean" || target_descriptor == "bool") {
      if (IsSqlNull(value)) return null_result("boolean");
      bool parsed = false;
      if (!ParseBooleanText(value, &parsed)) return null_result("boolean");
      return MakeFunctionSuccess(request, {MakeInt64Value("boolean", parsed ? 1 : 0)});
    }
    if (target_descriptor == "uuid") {
      if (IsSqlNull(value)) return null_result("uuid");
      std::string canonical;
      if (!CanonicalizeUuidText(ValueAsText(value), &canonical)) return null_result("uuid");
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", std::move(canonical))});
    }
    if (target_descriptor == "date" || target_descriptor == "time" ||
        target_descriptor == "timestamp" || target_descriptor == "timestamp_tz" ||
        target_descriptor == "interval" || target_descriptor == "json" ||
        target_descriptor == "json_document") {
      if (IsSqlNull(value)) return null_result(target_descriptor);
      return MakeFunctionSuccess(request, {MakeTextValue(target_descriptor, ValueAsText(value))});
    }
    return RefuseFunctionInvalidInput(request, "safe_cast/try_cast target descriptor id is not supported by the scalar runtime");
  }

  if (IdIs(id, {"data.scalar.convert", "sb.scalar.convert", "convert"})) {
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "convert expects value and charset");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (!IsUtf8EncodingName(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "convert currently supports only UTF8/UTF-8 charset");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", ValueAsText(request.arguments[0].value))});
  }

  if (IdIs(id, {"data.scalar.convert_to", "sb.scalar.convert_to", "convert_to"})) {
    static constexpr std::size_t kTextLimit = 1048576;
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "convert_to expects text and encoding");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("binary")});
    }
    if (!IsUtf8EncodingName(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "convert_to currently supports only UTF8/UTF-8 encoding");
    }
    const std::string text = ValueAsText(request.arguments[0].value);
    if (text.size() > kTextLimit) {
      return RefuseFunctionInvalidInput(request, "convert_to input exceeded bounded text limit");
    }
    if (!IsValidUtf8(text)) {
      return RefuseFunctionInvalidInput(request, "convert_to expects valid UTF-8 text");
    }
    return MakeFunctionSuccess(request,
                               {MakeBinaryValue("binary", std::vector<std::uint8_t>(text.begin(), text.end()))});
  }

  if (IdIs(id, {"data.scalar.convert_from", "sb.scalar.convert_from", "convert_from"})) {
    static constexpr std::size_t kTextLimit = 1048576;
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "convert_from expects binary and encoding");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments[0].value.payload_kind != scratchbird::engine::sblr::SblrValuePayloadKind::binary) {
      return RefuseFunctionInvalidInput(request, "convert_from input must be binary");
    }
    if (!IsUtf8EncodingName(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "convert_from currently supports only UTF8/UTF-8 encoding");
    }
    const auto& bytes = request.arguments[0].value.binary_value;
    if (bytes.size() > kTextLimit) {
      return RefuseFunctionInvalidInput(request, "convert_from input exceeded bounded binary limit");
    }
    const std::string text(bytes.begin(), bytes.end());
    if (!IsValidUtf8(text)) {
      return RefuseFunctionInvalidInput(request, "convert_from byte stream is not valid UTF-8");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", text)});
  }

  if (IdIs(id, {"data.scalar.array_to_string", "sb.scalar.array_to_string", "array_to_string"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "array_to_string expects array, delimiter, and optional null string");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::vector<std::optional<std::string>> elements;
    if (!ParseJsonTextArray(ValueAsText(request.arguments[0].value), &elements)) {
      return RefuseFunctionInvalidInput(request, "array_to_string expects a JSON text array payload");
    }
    const std::string delimiter = ValueAsText(request.arguments[1].value);
    const bool has_null_string = request.arguments.size() == 3 && !IsSqlNull(request.arguments[2].value);
    const std::string null_string = has_null_string ? ValueAsText(request.arguments[2].value) : std::string();
    std::string out;
    bool emitted = false;
    for (const auto& element : elements) {
      if (!element.has_value() && !has_null_string) continue;
      if (emitted) out += delimiter;
      emitted = true;
      out += element.has_value() ? *element : null_string;
      if (out.size() > 1048576) {
        return RefuseFunctionInvalidInput(request, "array_to_string output exceeded bounded text limit");
      }
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", out)});
  }

  if (IdIs(id, {"data.scalar.to_ascii", "sb.scalar.to_ascii", "to_ascii"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "to_ascii expects text and optional encoding");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments.size() == 2 && !IsUtf8EncodingName(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "to_ascii currently supports only UTF8/UTF-8 encoding");
    }
    std::string folded;
    if (!FoldUtf8ToAscii(ValueAsText(request.arguments[0].value), &folded)) {
      return RefuseFunctionInvalidInput(request, "to_ascii expects valid UTF-8 text");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", folded)});
  }

  if (IdIs(id, {"data.scalar.format", "sb.scalar.format", "format"})) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "format expects template text");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    const std::string pattern = ValueAsText(request.arguments[0].value);
    std::string out;
    std::size_t arg_index = 1;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
      if (pattern[i] != '%') {
        out.push_back(pattern[i]);
        continue;
      }
      if (++i >= pattern.size()) return RefuseFunctionInvalidInput(request, "format directive is incomplete");
      const char directive = pattern[i];
      if (directive == '%') {
        out.push_back('%');
        continue;
      }
      if (directive != 's' && directive != 'I' && directive != 'L') {
        return RefuseFunctionInvalidInput(request, "format supports only %s, %I, %L, and %% directives");
      }
      if (arg_index >= request.arguments.size()) {
        return RefuseFunctionInvalidInput(request, "format template has more directives than arguments");
      }
      const auto& value = request.arguments[arg_index++].value;
      if (directive == 's') {
        if (!IsSqlNull(value)) out += ValueAsText(value);
      } else if (directive == 'I') {
        if (IsSqlNull(value)) return RefuseFunctionInvalidInput(request, "format %I cannot quote NULL");
        out += QuoteIdentifierText(ValueAsText(value));
      } else {
        out += IsSqlNull(value) ? std::string("NULL") : QuoteLiteralText(ValueAsText(value));
      }
      if (out.size() > 1048576) {
        return RefuseFunctionInvalidInput(request, "format output exceeded bounded text limit");
      }
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", out)});
  }

  if (IdIs(id, {"data.scalar.oracle_decode", "sb.scalar.oracle_decode", "sb.scalar.decode_conditional"}) ||
      (id == "decode" && request.arguments.size() != 2)) {
    if (request.arguments.size() < 3) {
      return RefuseFunctionInvalidInput(request, "oracle decode expects expression, search/result pair, and optional default");
    }
    const std::size_t remaining = request.arguments.size() - 1;
    const bool has_default = (remaining % 2) == 1;
    const std::size_t pair_arg_count = has_default ? remaining - 1 : remaining;
    if (pair_arg_count == 0 || (pair_arg_count % 2) != 0) {
      return RefuseFunctionInvalidInput(request, "oracle decode requires complete search/result pairs");
    }
    const auto sql_decode_equal = [](const scratchbird::engine::sblr::SblrValue& left,
                                     const scratchbird::engine::sblr::SblrValue& right) -> bool {
      if (IsSqlNull(left) || IsSqlNull(right)) return IsSqlNull(left) && IsSqlNull(right);
      double left_real = 0.0;
      double right_real = 0.0;
      if (ValueAsReal64(left, &left_real) && ValueAsReal64(right, &right_real)) {
        return left_real == right_real;
      }
      return ValueAsText(left) == ValueAsText(right);
    };
    const auto& expr = request.arguments[0].value;
    for (std::size_t i = 1; i <= pair_arg_count; i += 2) {
      if (sql_decode_equal(expr, request.arguments[i].value)) {
        return MakeFunctionSuccess(request, {request.arguments[i + 1].value});
      }
    }
    if (has_default) {
      return MakeFunctionSuccess(request, {request.arguments.back().value});
    }
    return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[2].value.descriptor_id)});
  }

  if (IdIs(id, {"data.scalar.extract", "data.scalar.date_part", "sb.temporal.date_part", "extract", "date_part"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "extract expects field and temporal value");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    std::int64_t extracted = 0;
    const std::string field = ValueAsText(request.arguments[0].value);
    const auto& temporal_value = request.arguments[1].value;
    if (LowerAscii(temporal_value.descriptor_id) == "interval") {
      const std::string interval_text = ValueAsText(temporal_value);
      const auto parse_interval_field = [](std::string requested_field,
                                           const std::string& text,
                                           std::int64_t* out) -> bool {
        requested_field = LowerAscii(std::move(requested_field));
        if (out == nullptr) return false;
        std::size_t pos = 0;
        std::int64_t sign = 1;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
          sign = text[pos] == '-' ? -1 : 1;
          ++pos;
        }
        if (pos >= text.size() || text[pos] != 'P') return false;
        ++pos;
        bool in_time = false;
        bool saw_component = false;
        std::int64_t years = 0;
        std::int64_t months = 0;
        std::int64_t days = 0;
        std::int64_t hours = 0;
        std::int64_t minutes = 0;
        std::int64_t seconds = 0;
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
          if (unit == 'Y' && !in_time) years = value;
          else if (unit == 'M' && !in_time) months = value;
          else if (unit == 'D' && !in_time) days = value;
          else if (unit == 'H' && in_time) hours = value;
          else if (unit == 'M' && in_time) minutes = value;
          else if (unit == 'S' && in_time) seconds = value;
          else return false;
          saw_component = true;
        }
        if (!saw_component) return false;
        if (requested_field == "year" || requested_field == "years") *out = sign * years;
        else if (requested_field == "month" || requested_field == "months") *out = sign * months;
        else if (requested_field == "day" || requested_field == "days") *out = sign * days;
        else if (requested_field == "hour" || requested_field == "hours") *out = sign * hours;
        else if (requested_field == "minute" || requested_field == "minutes") *out = sign * minutes;
        else if (requested_field == "second" || requested_field == "seconds") *out = sign * seconds;
        else if (requested_field == "epoch" || requested_field == "epochs") {
          if (years != 0 || months != 0) return false;
          *out = sign * (days * 86400 + hours * 3600 + minutes * 60 + seconds);
        } else {
          return false;
        }
        return true;
      };
      if (!parse_interval_field(field, interval_text, &extracted)) {
        return RefuseFunctionInvalidInput(request, "date_part interval text must match ISO-8601 duration grammar P[nY][nM][nD][T[nH][nM][nS]]");
      }
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", extracted)});
    }
    if (!ExtractTimestampField(ValueAsText(request.arguments[0].value),
                               ValueAsText(request.arguments[1].value),
                               &extracted)) {
      return RefuseFunctionInvalidInput(request, "extract field is unsupported or temporal value is not ISO-like text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", extracted)});
  }

  // dow/doy/quarter temporal sub-stream 2026-05-11. Each canonical takes a
  // single date/timestamp argument and returns the int64 field extracted via
  // ExtractTimestampField. ExtractTimestampField extended with dow (Sunday=0
  // per PostgreSQL convention via days_from_civil mod 7), doy (1..366), and
  // quarter (1..4) field handlers.
  if (IdIs(id, {"data.scalar.dow", "sb.fn.temporal.dow", "sb.temporal.dow", "dow", "day_of_week"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "dow expects one temporal argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t extracted = 0;
    if (!ExtractTimestampField("dow", ValueAsText(request.arguments[0].value), &extracted)) {
      return RefuseFunctionInvalidInput(request, "dow argument must be ISO-like temporal text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", extracted)});
  }
  if (IdIs(id, {"data.scalar.doy", "sb.fn.temporal.doy", "sb.temporal.doy", "doy", "day_of_year"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "doy expects one temporal argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t extracted = 0;
    if (!ExtractTimestampField("doy", ValueAsText(request.arguments[0].value), &extracted)) {
      return RefuseFunctionInvalidInput(request, "doy argument must be ISO-like temporal text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", extracted)});
  }
  if (IdIs(id, {"data.scalar.quarter", "sb.fn.temporal.quarter", "sb.temporal.quarter", "quarter"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "quarter expects one temporal argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t extracted = 0;
    if (!ExtractTimestampField("quarter", ValueAsText(request.arguments[0].value), &extracted)) {
      return RefuseFunctionInvalidInput(request, "quarter argument must be ISO-like temporal text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", extracted)});
  }
  if (IdIs(id, {"data.scalar.isodow", "sb.fn.temporal.isodow", "sb.temporal.isodow", "isodow"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "isodow expects one temporal argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t extracted = 0;
    if (!ExtractTimestampField("isodow", ValueAsText(request.arguments[0].value), &extracted)) {
      return RefuseFunctionInvalidInput(request, "isodow argument must be ISO-like temporal text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", extracted)});
  }
  if (IdIs(id, {"data.scalar.week", "sb.fn.temporal.week", "sb.temporal.week", "week"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "week expects one temporal argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t extracted = 0;
    if (!ExtractTimestampField("week", ValueAsText(request.arguments[0].value), &extracted)) {
      return RefuseFunctionInvalidInput(request, "week argument must be ISO-like temporal text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", extracted)});
  }

  // add_months + last_day temporal arithmetic sub-stream 2026-05-11. Both
  // build on the DaysFromCivil/CivilFromDays pair: add_months computes the
  // target (year, month) by adding the signed month offset then clamps the
  // day to the days-in-month for the target month; last_day finds the first
  // day of the following month via DaysFromCivil and subtracts one day then
  // recovers the (year, month, day) via CivilFromDays.
  if (IdIs(id, {"data.scalar.add_months", "sb.fn.temporal.add_months", "sb.temporal.add_months", "add_months"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "add_months expects date and integer month offset");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("date")});
    }
    const auto temporal = ValueAsText(request.arguments[0].value);
    if (temporal.size() < 10) return RefuseFunctionInvalidInput(request, "add_months requires ISO-like date text");
    std::int64_t months_offset = 0;
    if (!ValueAsInt64Strict(request.arguments[1].value, &months_offset)) {
      return RefuseFunctionInvalidInput(request, "add_months offset must be an int64 month count");
    }
    const std::int64_t y = std::stoll(temporal.substr(0, 4));
    const std::int64_t mo = std::stoll(temporal.substr(5, 2));
    const std::int64_t d = std::stoll(temporal.substr(8, 2));
    // Translate (y, mo) into 0-based total-months since year 0 to allow safe
    // negative arithmetic; mo - 1 + offset can be negative.
    std::int64_t total = (y * 12) + (mo - 1) + months_offset;
    std::int64_t new_y = total / 12;
    std::int64_t new_mo = total % 12 + 1;
    if (total < 0 && total % 12 != 0) {
      new_y -= 1;
      new_mo += 12;
    }
    // Clamp day to the new month's day count.
    static constexpr unsigned kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned days = kDaysInMonth[new_mo - 1];
    if (new_mo == 2) {
      const bool leap = (new_y % 4 == 0 && new_y % 100 != 0) || (new_y % 400 == 0);
      if (leap) days = 29;
    }
    std::int64_t new_d = d > static_cast<std::int64_t>(days) ? static_cast<std::int64_t>(days) : d;
    const std::string out = FourDigit(new_y) + "-" + TwoDigit(new_mo) + "-" + TwoDigit(new_d);
    return MakeFunctionSuccess(request, {MakeTextValue("date", out)});
  }
  if (IdIs(id, {"data.scalar.last_day", "sb.fn.temporal.last_day", "sb.temporal.last_day", "last_day"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "last_day expects one temporal argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("date")});
    const auto temporal = ValueAsText(request.arguments[0].value);
    if (temporal.size() < 10) return RefuseFunctionInvalidInput(request, "last_day requires ISO-like date text");
    const std::int64_t y = std::stoll(temporal.substr(0, 4));
    const std::int64_t mo = std::stoll(temporal.substr(5, 2));
    // First day of the following month then subtract one day.
    std::int64_t next_y = y;
    std::int64_t next_mo = mo + 1;
    if (next_mo == 13) {
      next_y += 1;
      next_mo = 1;
    }
    const std::int64_t last_days = DaysFromCivil(next_y, static_cast<unsigned>(next_mo), 1) - 1;
    std::int64_t out_y = 0;
    unsigned out_m = 0;
    unsigned out_d = 0;
    CivilFromDays(last_days, &out_y, &out_m, &out_d);
    const std::string out = FourDigit(out_y) + "-" + TwoDigit(out_m) + "-" + TwoDigit(out_d);
    return MakeFunctionSuccess(request, {MakeTextValue("date", out)});
  }

  if (IdIs(id, {"data.scalar.abs", "sb.fn.numeric.abs", "sb.scalar.abs", "abs"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "expected exactly one argument");
    const auto& value = request.arguments[0].value;
    if (IsSqlNull(value)) return MakeFunctionSuccess(request, {MakeNullValue(value.descriptor_id)});
    if (!value.has_int64_value) return RefuseFunctionInvalidInput(request, "abs requires int64 scalar in this slice");
    if (value.int64_value == std::numeric_limits<std::int64_t>::min()) {
      return RefuseFunctionOverflow(request, "abs(INT64_MIN) overflows int64");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value(value.descriptor_id, value.int64_value < 0 ? -value.int64_value
                                                                                                  : value.int64_value)});
  }

  if (IdIs(id, {"data.scalar.ceil", "data.scalar.ceiling", "sb.fn.numeric.ceil", "sb.scalar.ceil", "ceil", "ceiling"})) {
    return UnaryReal(request, std::ceil, "ceil");
  }
  if (IdIs(id, {"data.scalar.floor", "sb.fn.numeric.floor", "sb.scalar.floor", "floor"})) {
    return UnaryReal(request, std::floor, "floor");
  }
  if (IdIs(id, {"data.scalar.round", "sb.fn.numeric.round", "sb.scalar.round", "round"})) {
    if (request.arguments.size() == 1) return UnaryReal(request, std::round, "round");
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "round expects numeric value and optional integer scale");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    double input = 0.0;
    std::int64_t scale = 0;
    if (!ValueAsReal64(request.arguments[0].value, &input) ||
        !ValueAsInt64Strict(request.arguments[1].value, &scale)) {
      return RefuseFunctionInvalidInput(request, "round optional scale form requires numeric value and int64 scale");
    }
    if (scale < -308 || scale > 308) {
      return RefuseFunctionNumericDomain(request, "round scale must be in range -308 to 308 for real64 runtime");
    }
    const double factor = std::pow(10.0, static_cast<double>(scale < 0 ? -scale : scale));
    if (!std::isfinite(factor) || factor == 0.0) {
      return RefuseFunctionOverflow(request, "round scale factor is not finite");
    }
    const double output = scale >= 0 ? std::round(input * factor) / factor
                                    : std::round(input / factor) * factor;
    if (!std::isfinite(output)) {
      return RefuseFunctionOverflow(request, "round produced a non-finite result");
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
  }
  if (IdIs(id, {"data.scalar.trunc", "data.scalar.truncate", "sb.fn.numeric.trunc", "sb.scalar.trunc", "sb.scalar.truncate", "trunc", "truncate"})) {
    if (request.arguments.size() == 1) return UnaryReal(request, std::trunc, "trunc");
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "trunc expects numeric value and optional integer scale");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    double input = 0.0;
    std::int64_t scale = 0;
    if (!ValueAsReal64(request.arguments[0].value, &input) || !ValueAsInt64Strict(request.arguments[1].value, &scale)) {
      return RefuseFunctionInvalidInput(request, "trunc optional scale form requires numeric value and int64 scale");
    }
    if (scale < -308 || scale > 308) return RefuseFunctionNumericDomain(request, "trunc scale must be in range -308 to 308 for real64 runtime");
    const double factor = std::pow(10.0, static_cast<double>(scale < 0 ? -scale : scale));
    if (!std::isfinite(factor) || factor == 0.0) return RefuseFunctionOverflow(request, "trunc scale factor is not finite");
    const double output = scale >= 0 ? std::trunc(input * factor) / factor : std::trunc(input / factor) * factor;
    if (!std::isfinite(output)) return RefuseFunctionOverflow(request, "trunc produced a non-finite result");
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
  }
  if (IdIs(id, {"data.scalar.sqrt", "sb.fn.numeric.sqrt", "sb.scalar.sqrt", "sqrt"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "sqrt requires a numeric value");
      if (input < 0.0) return RefuseFunctionInvalidInput(request, "sqrt input must be non-negative");
    }
    return UnaryReal(request, std::sqrt, "sqrt");
  }
  if (IdIs(id, {"data.scalar.cbrt", "sb.fn.numeric.cbrt", "sb.scalar.cbrt", "cbrt"})) {
    return UnaryReal(request, std::cbrt, "cbrt");
  }
  if (IdIs(id, {"data.scalar.exp", "sb.fn.numeric.exp", "sb.scalar.exp", "exp"})) return UnaryReal(request, std::exp, "exp");
  if (IdIs(id, {"data.scalar.ln", "sb.fn.numeric.ln", "sb.scalar.ln", "ln"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "ln requires a numeric value");
      if (input <= 0.0) return RefuseFunctionNumericDomain(request, "ln input must be positive");
    }
    return UnaryReal(request, std::log, "ln");
  }
  if (IdIs(id, {"data.scalar.log10", "sb.fn.numeric.log10", "sb.scalar.log10", "log10"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "log10 requires a numeric value");
      if (input <= 0.0) return RefuseFunctionNumericDomain(request, "log10 input must be positive");
    }
    return UnaryReal(request, std::log10, "log10");
  }
  if (IdIs(id, {"data.scalar.log", "sb.fn.numeric.log", "sb.scalar.log", "log"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "log expects one numeric argument or base and numeric arguments");
    }
    for (const auto& arg : request.arguments) {
      if (IsSqlNull(arg.value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    if (request.arguments.size() == 1) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "log requires a numeric value");
      if (input <= 0.0) return RefuseFunctionNumericDomain(request, "log input must be positive");
      const double output = std::log10(input);
      if (!std::isfinite(output)) return RefuseFunctionOverflow(request, "log produced a non-finite result");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
    }
    double base = 0.0;
    double input = 0.0;
    if (!ValueAsReal64(request.arguments[0].value, &base) ||
        !ValueAsReal64(request.arguments[1].value, &input)) {
      return RefuseFunctionInvalidInput(request, "log requires numeric base and numeric value");
    }
    if (base <= 0.0 || base == 1.0 || input <= 0.0) {
      return RefuseFunctionNumericDomain(request, "log base must be positive and not 1; input must be positive");
    }
    const double output = std::log(input) / std::log(base);
    if (!std::isfinite(output)) return RefuseFunctionOverflow(request, "log produced a non-finite result");
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
  }
  if (IdIs(id, {"data.scalar.sin", "sb.fn.numeric.sin", "sb.scalar.sin", "sin"})) return UnaryReal(request, std::sin, "sin");
  if (IdIs(id, {"data.scalar.cos", "sb.fn.numeric.cos", "sb.scalar.cos", "cos"})) return UnaryReal(request, std::cos, "cos");
  if (IdIs(id, {"data.scalar.tan", "sb.fn.numeric.tan", "sb.scalar.tan", "tan"})) return UnaryReal(request, std::tan, "tan");
  if (IdIs(id, {"data.scalar.asin", "sb.fn.numeric.asin", "sb.scalar.asin", "asin"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "asin requires a numeric value");
      if (input < -1.0 || input > 1.0) return RefuseFunctionNumericDomain(request, "asin input must be in [-1, 1]");
    }
    return UnaryReal(request, std::asin, "asin");
  }
  if (IdIs(id, {"data.scalar.acos", "sb.fn.numeric.acos", "sb.scalar.acos", "acos"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "acos requires a numeric value");
      if (input < -1.0 || input > 1.0) return RefuseFunctionNumericDomain(request, "acos input must be in [-1, 1]");
    }
    return UnaryReal(request, std::acos, "acos");
  }
  if (IdIs(id, {"data.scalar.atan", "sb.fn.numeric.atan", "sb.scalar.atan", "atan"})) return UnaryReal(request, std::atan, "atan");
  if (IdIs(id, {"data.scalar.cot", "sb.fn.numeric.cot", "sb.scalar.cot", "cot", "cot(numeric)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "cot expects exactly one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    double input = 0.0;
    if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "cot requires a numeric value");
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", 1.0 / std::tan(input))});
  }
  if (IdIs(id, {"data.scalar.sinh", "sb.fn.numeric.sinh", "sb.scalar.sinh", "sinh"})) return UnaryReal(request, std::sinh, "sinh");
  if (IdIs(id, {"data.scalar.cosh", "sb.fn.numeric.cosh", "sb.scalar.cosh", "cosh"})) return UnaryReal(request, std::cosh, "cosh");
  if (IdIs(id, {"data.scalar.tanh", "sb.fn.numeric.tanh", "sb.scalar.tanh", "tanh"})) return UnaryReal(request, std::tanh, "tanh");
  if (IdIs(id, {"data.scalar.asinh", "sb.fn.numeric.asinh", "sb.scalar.asinh", "asinh"})) return UnaryReal(request, std::asinh, "asinh");
  if (IdIs(id, {"data.scalar.acosh", "sb.fn.numeric.acosh", "sb.scalar.acosh", "acosh"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "acosh requires a numeric value");
      if (input < 1.0) return RefuseFunctionNumericDomain(request, "acosh input must be >= 1");
    }
    return UnaryReal(request, std::acosh, "acosh");
  }
  if (IdIs(id, {"data.scalar.atanh", "sb.fn.numeric.atanh", "sb.scalar.atanh", "atanh"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "atanh requires a numeric value");
      if (input <= -1.0 || input >= 1.0) return RefuseFunctionNumericDomain(request, "atanh input must be in exclusive (-1, 1)");
    }
    return UnaryReal(request, std::atanh, "atanh");
  }
  if (IdIs(id, {"data.scalar.log2", "sb.fn.numeric.log2", "sb.scalar.log2", "log2"})) {
    if (request.arguments.size() == 1 && !IsSqlNull(request.arguments[0].value)) {
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "log2 requires a numeric value");
      if (input <= 0.0) return RefuseFunctionNumericDomain(request, "log2 input must be positive");
    }
    return UnaryReal(request, std::log2, "log2");
  }
  if (IdIs(id, {"data.scalar.atan2", "sb.fn.numeric.atan2", "sb.scalar.atan2", "atan2"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "atan2 expects two arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    double y = 0.0;
    double x = 0.0;
    if (!ValueAsReal64(request.arguments[0].value, &y) || !ValueAsReal64(request.arguments[1].value, &x)) {
      return RefuseFunctionInvalidInput(request, "atan2 requires numeric arguments");
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::atan2(y, x))});
  }
  if (IdIs(id, {"data.scalar.pi", "sb.fn.numeric.pi", "sb.scalar.pi", "pi"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "pi expects no arguments");
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::acos(-1.0))});
  }
  // Degrees-mode trig sub-stream 2026-05-11. Forward trig (sind/cosd/tand)
  // scales input by pi/180 before calling std::sin/cos/tan. Inverse trig
  // (asind/acosd/atand) calls std::asin/acos/atan and scales output by
  // 180/pi. asind/acosd preserve the |input| > 1 domain refusal.
  // Using std::acos(-1.0) for pi keeps the constant identical to what
  // std::sin/cos use internally without introducing M_PI macro reliance.
  {
    const double pi = std::acos(-1.0);
    const double deg_to_rad = pi / 180.0;
    const double rad_to_deg = 180.0 / pi;
    if (IdIs(id, {"data.scalar.degrees", "sb.fn.numeric.degrees", "sb.scalar.degrees", "degrees"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "degrees expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "degrees requires a numeric radians value");
      const double output = input * rad_to_deg;
      if (!std::isfinite(output)) return RefuseFunctionOverflow(request, "degrees produced a non-finite result");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
    }
    if (IdIs(id, {"data.scalar.radians", "sb.fn.numeric.radians", "sb.scalar.radians", "radians"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "radians expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "radians requires a numeric degrees value");
      const double output = input * deg_to_rad;
      if (!std::isfinite(output)) return RefuseFunctionOverflow(request, "radians produced a non-finite result");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
    }
    if (IdIs(id, {"data.scalar.sind", "sb.fn.numeric.sind", "sb.scalar.sind", "sind"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "sind expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "sind requires a numeric value");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::sin(input * deg_to_rad))});
    }
    if (IdIs(id, {"data.scalar.cosd", "sb.fn.numeric.cosd", "sb.scalar.cosd", "cosd"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "cosd expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "cosd requires a numeric value");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::cos(input * deg_to_rad))});
    }
    if (IdIs(id, {"data.scalar.tand", "sb.fn.numeric.tand", "sb.scalar.tand", "tand"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "tand expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "tand requires a numeric value");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::tan(input * deg_to_rad))});
    }
    if (IdIs(id, {"data.scalar.asind", "sb.fn.numeric.asind", "sb.scalar.asind", "asind"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "asind expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "asind requires a numeric value");
      if (input < -1.0 || input > 1.0) return RefuseFunctionNumericDomain(request, "asind input must be in [-1, 1]");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::asin(input) * rad_to_deg)});
    }
    if (IdIs(id, {"data.scalar.acosd", "sb.fn.numeric.acosd", "sb.scalar.acosd", "acosd"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "acosd expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "acosd requires a numeric value");
      if (input < -1.0 || input > 1.0) return RefuseFunctionNumericDomain(request, "acosd input must be in [-1, 1]");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::acos(input) * rad_to_deg)});
    }
    if (IdIs(id, {"data.scalar.atand", "sb.fn.numeric.atand", "sb.scalar.atand", "atand"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "atand expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "atand requires a numeric value");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::atan(input) * rad_to_deg)});
    }
    if (IdIs(id, {"data.scalar.atan2d", "sb.fn.numeric.atan2d", "sb.scalar.atan2d", "atan2d"})) {
      if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "atan2d expects exactly two arguments");
      if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      }
      double y = 0.0;
      double x = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &y) ||
          !ValueAsReal64(request.arguments[1].value, &x)) {
        return RefuseFunctionInvalidInput(request, "atan2d requires numeric arguments");
      }
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::atan2(y, x) * rad_to_deg)});
    }
    if (IdIs(id, {"data.scalar.cotd", "sb.fn.numeric.cotd", "sb.scalar.cotd", "cotd"})) {
      if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "cotd expects exactly one argument");
      if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
      double input = 0.0;
      if (!ValueAsReal64(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "cotd requires a numeric value");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", 1.0 / std::tan(input * deg_to_rad))});
    }
  }

  if (IdIs(id, {"data.scalar.power", "data.scalar.pow", "sb.fn.numeric.power", "sb.scalar.power", "power", "pow"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "power expects base and exponent");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    double base = 0.0;
    double exponent = 0.0;
    if (!ValueAsReal64(request.arguments[0].value, &base) || !ValueAsReal64(request.arguments[1].value, &exponent)) {
      return RefuseFunctionInvalidInput(request, "power arguments must be numeric");
    }
    const double output = std::pow(base, exponent);
    if (!std::isfinite(output)) return RefuseFunctionOverflow(request, "power produced a non-finite result");
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", output)});
  }

  if (IdIs(id, {"data.scalar.mod", "sb.fn.numeric.mod", "sb.scalar.mod", "mod"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "mod expects dividend and divisor");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    if (request.arguments[0].value.has_int64_value && request.arguments[1].value.has_int64_value) {
      const std::int64_t left = request.arguments[0].value.int64_value;
      const std::int64_t right = request.arguments[1].value.int64_value;
      if (right == 0) return RefuseFunctionNumericDivisionByZero(request, "mod divisor must not be zero");
      if (left == std::numeric_limits<std::int64_t>::min() && right == -1) {
        return RefuseFunctionOverflow(request, "mod INT64_MIN by -1 overflows int64");
      }
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", left % right)});
    }
    double left = 0.0;
    double right = 0.0;
    if (!ValueAsReal64(request.arguments[0].value, &left) || !ValueAsReal64(request.arguments[1].value, &right)) {
      return RefuseFunctionInvalidInput(request, "mod arguments must be numeric");
    }
    if (right == 0.0) return RefuseFunctionNumericDivisionByZero(request, "mod divisor must not be zero");
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::fmod(left, right))});
  }

  if (IdIs(id, {"data.scalar.div", "sb.fn.numeric.div", "sb.scalar.div", "div"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "div expects dividend and divisor");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &left) || !ValueAsInt64Strict(request.arguments[1].value, &right)) {
      return RefuseFunctionInvalidInput(request, "div arguments must be int64");
    }
    if (right == 0) return RefuseFunctionNumericDivisionByZero(request, "div divisor must not be zero");
    if (left == std::numeric_limits<std::int64_t>::min() && right == -1) {
      return RefuseFunctionOverflow(request, "div INT64_MIN by -1 overflows int64");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", left / right)});
  }

  if (IdIs(id, {"data.scalar.factorial", "sb.fn.numeric.factorial", "sb.scalar.factorial", "factorial"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "factorial expects exactly one integer argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t input = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &input)) return RefuseFunctionInvalidInput(request, "factorial argument must be int64");
    if (input < 0) return RefuseFunctionNumericDomain(request, "factorial input must be non-negative");
    if (input > 20) return RefuseFunctionOverflow(request, "factorial result overflows int64 above 20");
    std::int64_t output = 1;
    for (std::int64_t i = 2; i <= input; ++i) output *= i;
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", output)});
  }

  if (IdIs(id, {"data.scalar.gcd", "sb.fn.numeric.gcd", "sb.scalar.gcd", "gcd"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "gcd expects two integer arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &left) || !ValueAsInt64Strict(request.arguments[1].value, &right)) {
      return RefuseFunctionInvalidInput(request, "gcd arguments must be int64");
    }
    const std::uint64_t output = GcdMagnitude(Int64Magnitude(left), Int64Magnitude(right));
    if (!Uint64FitsInt64(output)) return RefuseFunctionOverflow(request, "gcd result overflows int64");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(output))});
  }

  if (IdIs(id, {"data.scalar.lcm", "sb.fn.numeric.lcm", "sb.scalar.lcm", "lcm"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "lcm expects two integer arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &left) || !ValueAsInt64Strict(request.arguments[1].value, &right)) {
      return RefuseFunctionInvalidInput(request, "lcm arguments must be int64");
    }
    const std::uint64_t left_abs = Int64Magnitude(left);
    const std::uint64_t right_abs = Int64Magnitude(right);
    if (left_abs == 0 || right_abs == 0) return MakeFunctionSuccess(request, {MakeInt64Value("int64", 0)});
    const std::uint64_t gcd = GcdMagnitude(left_abs, right_abs);
    const std::uint64_t scaled = left_abs / gcd;
    const std::uint64_t max_int64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (scaled > max_int64 / right_abs) return RefuseFunctionOverflow(request, "lcm result overflows int64");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(scaled * right_abs))});
  }

  if (IdIs(id, {"data.scalar.width_bucket", "sb.fn.numeric.width_bucket", "sb.scalar.width_bucket", "width_bucket"})) {
    if (request.arguments.size() != 4) return RefuseFunctionInvalidInput(request, "width_bucket expects operand, low, high, and count");
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    double operand = 0.0;
    double low = 0.0;
    double high = 0.0;
    std::int64_t count = 0;
    if (!ValueAsReal64(request.arguments[0].value, &operand) ||
        !ValueAsReal64(request.arguments[1].value, &low) ||
        !ValueAsReal64(request.arguments[2].value, &high) ||
        !ValueAsInt64Strict(request.arguments[3].value, &count)) {
      return RefuseFunctionInvalidInput(request, "width_bucket requires numeric operand/bounds and int64 count");
    }
    if (!std::isfinite(operand) || !std::isfinite(low) || !std::isfinite(high)) {
      return RefuseFunctionOverflow(request, "width_bucket numeric operands must be finite real64 values");
    }
    if (count <= 0) return RefuseFunctionNumericDomain(request, "width_bucket count must be positive");
    if (low == high) return RefuseFunctionNumericDomain(request, "width_bucket low and high bounds must differ");
    if (count == std::numeric_limits<std::int64_t>::max()) return RefuseFunctionOverflow(request, "width_bucket count plus one overflows int64");
    std::int64_t bucket = 0;
    if (low < high) {
      if (operand < low) bucket = 0;
      else if (operand >= high) bucket = count + 1;
      else bucket = static_cast<std::int64_t>(std::floor(((operand - low) / (high - low)) * static_cast<double>(count))) + 1;
    } else {
      if (operand > low) bucket = 0;
      else if (operand <= high) bucket = count + 1;
      else bucket = static_cast<std::int64_t>(std::floor(((low - operand) / (low - high)) * static_cast<double>(count))) + 1;
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", bucket)});
  }

  if (IdIs(id, {"data.scalar.bit_count", "sb.fn.numeric.bit_count", "sb.scalar.bit_count", "bit_count"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "bit_count expects one integer argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t value = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &value)) return RefuseFunctionInvalidInput(request, "bit_count argument must be int64");
    std::uint64_t bits = static_cast<std::uint64_t>(value);
    std::int64_t count = 0;
    while (bits != 0) {
      bits &= (bits - 1);
      ++count;
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", count)});
  }

  if (IdIs(id, {"data.scalar.bit_and", "sb.fn.numeric.bit_and", "sb.scalar.bit_and"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "bit_and expects two integer arguments");
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &left) || !ValueAsInt64Strict(request.arguments[1].value, &right)) {
      return RefuseFunctionInvalidInput(request, "bit_and arguments must be int64");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", left & right)});
  }
  if (IdIs(id, {"data.scalar.bit_or", "sb.fn.numeric.bit_or", "sb.scalar.bit_or"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "bit_or expects two integer arguments");
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &left) || !ValueAsInt64Strict(request.arguments[1].value, &right)) {
      return RefuseFunctionInvalidInput(request, "bit_or arguments must be int64");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", left | right)});
  }
  if (IdIs(id, {"data.scalar.bit_xor", "sb.fn.numeric.bit_xor", "sb.scalar.bit_xor"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "bit_xor expects two integer arguments");
    std::int64_t left = 0;
    std::int64_t right = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &left) || !ValueAsInt64Strict(request.arguments[1].value, &right)) {
