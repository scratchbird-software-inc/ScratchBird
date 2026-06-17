// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
    if (start >= source.size()) {
      return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id, "")});
    }
    std::size_t count = std::string::npos;
    if (request.arguments.size() == 3) {
      if (request.arguments[2].value.int64_value < 0) {
        return RefuseFunctionInvalidInput(request, "substring length must be >= 0");
      }
      count = static_cast<std::size_t>(request.arguments[2].value.int64_value);
    }
    return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id,
                                                      source.substr(start, count))});
  }

  if (IdIs(id, {"data.scalar.stuff", "sb.fn.string.stuff", "sb.scalar.stuff", "stuff"})) {
    if (request.arguments.size() != 4) {
      return RefuseFunctionInvalidInput(request, "stuff expects (string, start, length, replacement)");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) {
        return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
      }
    }
    if (!request.arguments[1].value.has_int64_value || !request.arguments[2].value.has_int64_value) {
      return RefuseFunctionInvalidInput(request, "stuff start and length must be int64 scalar values");
    }
    const std::int64_t one_based_start = request.arguments[1].value.int64_value;
    const std::int64_t cut_length = request.arguments[2].value.int64_value;
    if (one_based_start < 1) {
      return RefuseFunctionInvalidInput(request, "stuff start is one-based and must be >= 1");
    }
    if (cut_length < 0) {
      return RefuseFunctionInvalidInput(request, "stuff length must be >= 0");
    }
    const auto stuff_source = ValueAsText(request.arguments[0].value);
    const auto stuff_replacement = ValueAsText(request.arguments[3].value);
    const std::size_t stuff_start = static_cast<std::size_t>(one_based_start - 1);
    if (stuff_start > stuff_source.size()) {
      return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id, stuff_source)});
    }
    const std::size_t stuff_remove = std::min(static_cast<std::size_t>(cut_length), stuff_source.size() - stuff_start);
    std::string stuff_result;
    stuff_result.reserve(stuff_start + stuff_replacement.size() + (stuff_source.size() - stuff_start - stuff_remove));
    stuff_result.append(stuff_source, 0, stuff_start);
    stuff_result.append(stuff_replacement);
    stuff_result.append(stuff_source, stuff_start + stuff_remove, std::string::npos);
    return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id, std::move(stuff_result))});
  }

  if (IdIs(id, {"data.scalar.position", "sb.fn.string.position", "sb.scalar.position",
                "sb.scalar.bit_string_position", "bit_string_position", "position"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "position expects needle and haystack");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    if (is_bit_string_value(request.arguments[0].value) || is_bit_string_value(request.arguments[1].value)) {
      if (!is_bit_string_value(request.arguments[0].value) || !is_bit_string_value(request.arguments[1].value)) {
        return RefuseFunctionInvalidInput(request, "bit-string position requires bit-string needle and haystack");
      }
      const auto& needle_bits = request.arguments[0].value.binary_value;
      const auto& haystack_bits = request.arguments[1].value.binary_value;
      const std::size_t needle_count = needle_bits.size() * 8u;
      const std::size_t haystack_count = haystack_bits.size() * 8u;
      if (needle_count == 0) return MakeFunctionSuccess(request, {MakeInt64Value("int64", 1)});
      if (needle_count > haystack_count) return MakeFunctionSuccess(request, {MakeInt64Value("int64", 0)});
      for (std::size_t start = 0; start + needle_count <= haystack_count; ++start) {
        bool matched = true;
        for (std::size_t bit = 0; bit < needle_count; ++bit) {
          if (bit_at(haystack_bits, start + bit) != bit_at(needle_bits, bit)) {
            matched = false;
            break;
          }
        }
        if (matched) return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(start + 1))});
      }
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", 0)});
    }
    const auto needle = ValueAsText(request.arguments[0].value);
    const auto haystack = ValueAsText(request.arguments[1].value);
    const auto pos = haystack.find(needle);
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", pos == std::string::npos ? 0 : static_cast<std::int64_t>(pos + 1))});
  }

  if (IdIs(id, {"sb.scalar.instr", "sb.scalar.strpos", "instr", "strpos"})) {
    const bool is_strpos = IdIs(id, {"sb.scalar.strpos", "strpos"});
    if ((is_strpos && request.arguments.size() != 2) ||
        (!is_strpos && (request.arguments.size() < 2 || request.arguments.size() > 4))) {
      return RefuseFunctionInvalidInput(request, is_strpos ? "strpos expects string and substring"
                                                           : "instr expects string, substring, optional start, and optional occurrence");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    if (request.arguments.size() >= 3 && IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    if (request.arguments.size() == 4 && IsSqlNull(request.arguments[3].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const auto string_arg = ValueAsText(request.arguments[0].value);
    const auto substring = ValueAsText(request.arguments[1].value);
    std::int64_t start = 1;
    std::int64_t occurrence = 1;
    if (request.arguments.size() >= 3) {
      if (!request.arguments[2].value.has_int64_value) return RefuseFunctionInvalidInput(request, "instr start must be int64");
      start = request.arguments[2].value.int64_value;
    }
    if (request.arguments.size() == 4) {
      if (!request.arguments[3].value.has_int64_value) return RefuseFunctionInvalidInput(request, "instr occurrence must be int64");
      occurrence = request.arguments[3].value.int64_value;
    }
    if (start < 1) return RefuseFunctionInvalidInput(request, "instr start must be >= 1");
    if (occurrence < 1) return RefuseFunctionInvalidInput(request, "instr occurrence must be >= 1");
    std::size_t search_pos = static_cast<std::size_t>(start - 1);
    std::int64_t seen = 0;
    while (search_pos <= string_arg.size()) {
      const auto pos = string_arg.find(substring, search_pos);
      if (pos == std::string::npos) break;
      ++seen;
      if (seen == occurrence) {
        return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(pos + 1))});
      }
      search_pos = pos + 1;
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", 0)});
  }

  if (IdIs(id, {"data.scalar.concat", "sb.fn.string.concat", "sb.scalar.concat", "concat"})) {
    std::string out;
    for (const auto& argument : request.arguments) {
      if (!IsSqlNull(argument.value)) out += ValueAsText(argument.value);
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.concat_ws", "sb.fn.string.concat_ws", "sb.scalar.concat_ws", "concat_ws"})) {
    static constexpr std::size_t kTextLimit = 1048576;
    if (request.arguments.empty()) {
      return RefuseFunctionInvalidInput(request, "concat_ws expects separator and optional values");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    const std::string separator = ValueAsText(request.arguments[0].value);
    if (separator.size() > kTextLimit) {
      return RefuseFunctionInvalidInput(request, "concat_ws separator exceeded bounded text limit");
    }
    std::string out;
    bool emitted = false;
    for (std::size_t i = 1; i < request.arguments.size(); ++i) {
      const auto& value = request.arguments[i].value;
      if (IsSqlNull(value)) continue;
      const std::string part = ValueAsText(value);
      const std::size_t extra = part.size() + (emitted ? separator.size() : 0);
      if (extra > kTextLimit || out.size() > kTextLimit - extra) {
        return RefuseFunctionInvalidInput(request, "concat_ws output exceeded bounded text limit");
      }
      if (emitted) out += separator;
      out += part;
      emitted = true;
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.split_part", "sb.fn.string.split_part", "sb.scalar.split_part", "split_part"})) {
    static constexpr std::size_t kTextLimit = 1048576;
    if (request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "split_part expects text, delimiter, and one-based field index");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::int64_t field_index = 0;
    if (!ValueAsInt64Strict(request.arguments[2].value, &field_index) || field_index <= 0) {
      return RefuseFunctionInvalidInput(request, "split_part field index must be a positive one-based int64");
    }
    const std::string text = ValueAsText(request.arguments[0].value);
    const std::string delimiter = ValueAsText(request.arguments[1].value);
    if (text.size() > kTextLimit || delimiter.size() > kTextLimit) {
      return RefuseFunctionInvalidInput(request, "split_part input exceeded bounded text limit");
    }
    if (delimiter.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", field_index == 1 ? text : std::string())});
    }
    std::int64_t current = 1;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto pos = text.find(delimiter, start);
      if (current == field_index) {
        const auto count = pos == std::string::npos ? std::string::npos : pos - start;
        return MakeFunctionSuccess(request, {MakeTextValue("character", text.substr(start, count))});
      }
      if (pos == std::string::npos) break;
      start = pos + delimiter.size();
      ++current;
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", "")});
  }

  if (IdIs(id, {"data.scalar.string_to_array", "sb.fn.string.string_to_array",
                "sb.scalar.string_to_array", "string_to_array"})) {
    static constexpr std::size_t kTextLimit = 1048576;
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "string_to_array expects text, delimiter, and optional null string");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("array")});
    }
    const std::string text = ValueAsText(request.arguments[0].value);
    if (text.size() > kTextLimit) {
      return RefuseFunctionInvalidInput(request, "string_to_array input exceeded bounded text limit");
    }
    const bool has_null_string = request.arguments.size() == 3 && !IsSqlNull(request.arguments[2].value);
    const std::string null_string = has_null_string ? ValueAsText(request.arguments[2].value) : std::string();
    if (null_string.size() > kTextLimit) {
      return RefuseFunctionInvalidInput(request, "string_to_array null_string exceeded bounded text limit");
    }

    std::vector<std::string> split_values;
    if (IsSqlNull(request.arguments[1].value)) {
      if (!Utf8CharacterSlices(text, &split_values)) {
        return RefuseFunctionInvalidInput(request, "string_to_array delimiter NULL requires valid UTF-8 text");
      }
    } else {
      const std::string delimiter = ValueAsText(request.arguments[1].value);
      if (delimiter.size() > kTextLimit) {
        return RefuseFunctionInvalidInput(request, "string_to_array delimiter exceeded bounded text limit");
      }
      if (delimiter.empty()) {
        split_values.push_back(text);
      } else {
        std::size_t start = 0;
        while (start <= text.size()) {
          const auto pos = text.find(delimiter, start);
          split_values.push_back(text.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
          if (split_values.size() > kTextLimit) {
            return RefuseFunctionInvalidInput(request, "string_to_array element count exceeded bounded limit");
          }
          if (pos == std::string::npos) break;
          start = pos + delimiter.size();
        }
      }
    }

    std::vector<std::optional<std::string>> elements;
    elements.reserve(split_values.size());
    for (auto& value : split_values) {
      if (has_null_string && value == null_string) {
        elements.push_back(std::nullopt);
      } else {
        elements.push_back(std::move(value));
      }
    }
    std::string json = JsonArrayFromOptionalTextValues(elements);
    if (json.size() > kTextLimit) {
      return RefuseFunctionInvalidInput(request, "string_to_array JSON payload exceeded bounded text limit");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("array", std::move(json))});
  }

  if (IdIs(id, {"data.scalar.replace", "sb.fn.string.replace", "sb.scalar.replace", "replace"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "replace expects value, search, replacement");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) || IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::string out = ValueAsText(request.arguments[0].value);
    const std::string search = ValueAsText(request.arguments[1].value);
    const std::string replacement = ValueAsText(request.arguments[2].value);
    if (search.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
    std::size_t pos = 0;
    while ((pos = out.find(search, pos)) != std::string::npos) {
      out.replace(pos, search.size(), replacement);
      pos += replacement.size();
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.translate", "sb.fn.string.translate", "sb.scalar.translate", "translate"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "translate expects input, from, and to");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    return MakeFunctionSuccess(request,
                               {MakeTextValue("character",
                                              TranslateText(ValueAsText(request.arguments[0].value),
                                                            ValueAsText(request.arguments[1].value),
                                                            ValueAsText(request.arguments[2].value)))});
  }

  // overlay(string PLACING substring FROM start [FOR length])
  // Replaces `length` characters of `string` starting at one-based `start`
  // with `substring`. When length is omitted, defaults to substring's length.
  // Null-strict per dedicated null guard. start must be >= 1.
  if (IdIs(id, {"data.scalar.overlay", "sb.fn.string.overlay", "sb.scalar.overlay", "overlay"})) {
    if (request.arguments.size() != 3 && request.arguments.size() != 4) {
      return RefuseFunctionInvalidInput(request, "overlay expects string, substring, start, and optional length");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (!request.arguments[2].value.has_int64_value ||
        (request.arguments.size() == 4 && !request.arguments[3].value.has_int64_value)) {
      return RefuseFunctionInvalidInput(request, "overlay start and length must be int64 scalar values");
    }
    const std::int64_t one_based_start = request.arguments[2].value.int64_value;
    if (one_based_start < 1) return RefuseFunctionInvalidInput(request, "overlay start is one-based and must be >= 1");
    std::string out = ValueAsText(request.arguments[0].value);
    const std::string replacement = ValueAsText(request.arguments[1].value);
    const std::size_t start_index = static_cast<std::size_t>(one_based_start - 1);
    const std::size_t default_length = replacement.size();
    const std::int64_t length_arg = (request.arguments.size() == 4) ? request.arguments[3].value.int64_value
                                                                    : static_cast<std::int64_t>(default_length);
    if (length_arg < 0) return RefuseFunctionInvalidInput(request, "overlay length must be >= 0");
    const std::size_t length = static_cast<std::size_t>(length_arg);
    if (start_index > out.size()) {
      out.append(replacement);
    } else {
      const std::size_t safe_length = (start_index + length > out.size()) ? (out.size() - start_index) : length;
      out.replace(start_index, safe_length, replacement);
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.repeat", "sb.fn.string.repeat", "sb.scalar.repeat", "repeat"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "repeat expects value and count");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (!request.arguments[1].value.has_int64_value || request.arguments[1].value.int64_value < 0 ||
        request.arguments[1].value.int64_value > 1048576) {
      return RefuseFunctionInvalidInput(request, "repeat count must be an integer between 0 and 1048576");
    }
    const std::string input = ValueAsText(request.arguments[0].value);
    std::string out;
    for (std::int64_t i = 0; i < request.arguments[1].value.int64_value; ++i) out += input;
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.lpad", "sb.fn.string.lpad", "sb.scalar.lpad", "lpad"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 3) {
      return RefuseFunctionInvalidInput(request, "lpad expects text, length, and optional fill");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments.size() == 3 && IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (!request.arguments[1].value.has_int64_value || request.arguments[1].value.int64_value < 0 ||
        request.arguments[1].value.int64_value > 1048576) {
      return RefuseFunctionInvalidInput(request, "lpad length must be an integer between 0 and 1048576");
    }
    const std::string input = ValueAsText(request.arguments[0].value);
    const std::int64_t target = request.arguments[1].value.int64_value;
    const std::string fill = request.arguments.size() == 3 ? ValueAsText(request.arguments[2].value) : std::string(" ");
    if (static_cast<std::int64_t>(input.size()) >= target) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", input.substr(0, static_cast<std::size_t>(target)))});
    }
    if (fill.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", input)});
    }
    const std::int64_t pad_total = target - static_cast<std::int64_t>(input.size());
    std::string out;
    out.reserve(static_cast<std::size_t>(target));
    for (std::int64_t i = 0; i < pad_total; ++i) {
      out.push_back(fill[static_cast<std::size_t>(i) % fill.size()]);
    }
    out += input;
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.rpad", "sb.fn.string.rpad", "sb.scalar.rpad", "rpad"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 3) {
      return RefuseFunctionInvalidInput(request, "rpad expects text, length, and optional fill");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments.size() == 3 && IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (!request.arguments[1].value.has_int64_value || request.arguments[1].value.int64_value < 0 ||
        request.arguments[1].value.int64_value > 1048576) {
      return RefuseFunctionInvalidInput(request, "rpad length must be an integer between 0 and 1048576");
    }
    const std::string input = ValueAsText(request.arguments[0].value);
    const std::int64_t target = request.arguments[1].value.int64_value;
    const std::string fill = request.arguments.size() == 3 ? ValueAsText(request.arguments[2].value) : std::string(" ");
    if (static_cast<std::int64_t>(input.size()) >= target) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", input.substr(0, static_cast<std::size_t>(target)))});
    }
    if (fill.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", input)});
    }
    std::string out = input;
    out.reserve(static_cast<std::size_t>(target));
    const std::int64_t pad_total = target - static_cast<std::int64_t>(input.size());
    for (std::int64_t i = 0; i < pad_total; ++i) {
      out.push_back(fill[static_cast<std::size_t>(i) % fill.size()]);
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }

  if (IdIs(id, {"data.scalar.regexp_like", "sb.fn.regex.match", "sb.regex.match", "regexp_like"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 3) {
      return RefuseFunctionInvalidInput(request, "regexp_like expects string, pattern, and optional flags");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    if (request.arguments.size() == 3 && IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    const std::string subject = ValueAsText(request.arguments[0].value);
    const std::string pattern = ValueAsText(request.arguments[1].value);
    std::regex::flag_type flags = std::regex::ECMAScript;
    if (request.arguments.size() == 3) {
      const std::string flag_text = ValueAsText(request.arguments[2].value);
      for (const char ch : flag_text) {
        if (ch == 'i' || ch == 'I') {
          flags |= std::regex::icase;
        } else {
          return RefuseFunctionInvalidInput(request, "regexp_like flags must contain only 'i' (case-insensitive)");
        }
      }
    }
    try {
      const std::regex compiled(pattern, flags);
      return MakeFunctionSuccess(request, {MakeInt64Value("boolean", std::regex_search(subject, compiled) ? 1 : 0)});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "regexp_like received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.occurrences_regex", "sb.scalar.occurrences_regex", "occurrences_regex"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 3) {
      return RefuseFunctionInvalidInput(request, "occurrences_regex expects pattern, string, and optional flags");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        (request.arguments.size() == 3 && IsSqlNull(request.arguments[2].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const std::string pattern = ValueAsText(request.arguments[0].value);
    const std::string subject = ValueAsText(request.arguments[1].value);
    std::regex::flag_type flags;
    bool global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() == 3 ? ValueAsText(request.arguments[2].value) : "",
                               false,
                               &flags,
                               &global)) {
      return RefuseFunctionInvalidInput(request, "occurrences_regex flags must contain only 'i' (case-insensitive)");
    }
    try {
      const std::regex compiled(pattern, flags);
      std::int64_t count = 0;
      for (auto it = std::sregex_iterator(subject.begin(), subject.end(), compiled);
           it != std::sregex_iterator();
           ++it) {
        ++count;
        if (count > 1048576) {
          return RefuseFunctionInvalidInput(request, "occurrences_regex exceeded bounded match limit");
        }
      }
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", count)});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "occurrences_regex received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.position_regex", "sb.scalar.position_regex", "position_regex"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 5) {
      return RefuseFunctionInvalidInput(request, "position_regex expects pattern, string, optional occurrence, flags, and mode");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const std::string pattern = ValueAsText(request.arguments[0].value);
    const std::string subject = ValueAsText(request.arguments[1].value);
    std::int64_t occurrence = 1;
    if (request.arguments.size() >= 3) {
      if (!request.arguments[2].value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "position_regex occurrence must be int64");
      }
      occurrence = request.arguments[2].value.int64_value;
    }
    std::regex::flag_type flags;
    bool global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() >= 4 ? ValueAsText(request.arguments[3].value) : "",
                               false,
                               &flags,
                               &global)) {
      return RefuseFunctionInvalidInput(request, "position_regex flags must contain only 'i' (case-insensitive)");
    }
    std::string mode = "start";
    if (request.arguments.size() >= 5) mode = LowerAscii(ValueAsText(request.arguments[4].value));
    if (occurrence < 1) return RefuseFunctionInvalidInput(request, "position_regex occurrence must be >= 1");
    if (mode != "start" && mode != "after") {
      return RefuseFunctionInvalidInput(request, "position_regex mode must be START or AFTER");
    }
    try {
      const std::regex compiled(pattern, flags);
      auto search_begin = subject.cbegin();
      const auto search_end = subject.cend();
      std::int64_t seen = 0;
      while (search_begin <= search_end) {
        std::smatch match;
        if (!std::regex_search(search_begin, search_end, match, compiled)) break;
        ++seen;
        const auto absolute_start = static_cast<std::int64_t>(
            std::distance(subject.cbegin(), match[0].first));
        if (seen == occurrence) {
          const std::int64_t value = mode == "after"
              ? absolute_start + static_cast<std::int64_t>(match.length(0)) + 1
              : absolute_start + 1;
          return MakeFunctionSuccess(request, {MakeInt64Value("int64", value)});
        }
        auto next = match[0].second;
        if (match.length(0) == 0 && next != search_end) ++next;
        if (next == search_begin && next == search_end) break;
        search_begin = next;
        if (seen > 1048576) {
          return RefuseFunctionInvalidInput(request, "position_regex exceeded bounded match limit");
        }
      }
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", 0)});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "position_regex received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.regexp_count", "sb.scalar.regexp_count", "sb.regex.count", "regexp_count"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 3) {
      return RefuseFunctionInvalidInput(request, "regexp_count expects string, pattern, and optional flags");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        (request.arguments.size() == 3 && IsSqlNull(request.arguments[2].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const std::string subject = ValueAsText(request.arguments[0].value);
    const std::string pattern = ValueAsText(request.arguments[1].value);
    std::regex::flag_type flags;
    bool global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() == 3 ? ValueAsText(request.arguments[2].value) : "",
                               false,
                               &flags,
                               &global)) {
      return RefuseFunctionInvalidInput(request, "regexp_count flags must contain only 'i' (case-insensitive)");
    }
    try {
      const std::regex compiled(pattern, flags);
      std::int64_t count = 0;
      for (auto it = std::sregex_iterator(subject.begin(), subject.end(), compiled);
           it != std::sregex_iterator();
           ++it) {
        ++count;
        if (count > 1048576) {
          return RefuseFunctionInvalidInput(request, "regexp_count exceeded bounded match limit");
        }
      }
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", count)});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "regexp_count received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.regexp_match", "sb.scalar.regexp_match", "sb.scalar.regexp_matches",
                "sb.regex.capture", "regexp_match", "regexp_matches"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 3) {
      return RefuseFunctionInvalidInput(request, "regexp_match expects string, pattern, and optional flags");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        (request.arguments.size() == 3 && IsSqlNull(request.arguments[2].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("array")});
    }
    const std::string subject = ValueAsText(request.arguments[0].value);
    const std::string pattern = ValueAsText(request.arguments[1].value);
    std::regex::flag_type flags;
    bool global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() == 3 ? ValueAsText(request.arguments[2].value) : "",
                               false,
                               &flags,
                               &global)) {
      return RefuseFunctionInvalidInput(request, "regexp_match flags must contain only 'i' (case-insensitive)");
    }
    try {
      const std::regex compiled(pattern, flags);
      std::smatch match;
      if (!std::regex_search(subject, match, compiled)) {
        return MakeFunctionSuccess(request, {MakeNullValue("array")});
      }
      std::vector<std::string> values;
      if (match.size() > 1) {
        values.reserve(match.size() - 1);
        for (std::size_t i = 1; i < match.size(); ++i) values.push_back(match.str(i));
      } else {
        values.push_back(match.str(0));
      }
      return MakeFunctionSuccess(request, {MakeTextValue("array", JsonArrayFromTextValues(values))});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "regexp_match received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.substring_regex", "sb.scalar.substring_regex", "substring_regex"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 5) {
      return RefuseFunctionInvalidInput(request, "substring_regex expects pattern, string, optional occurrence, group, and flags");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    const std::string pattern = ValueAsText(request.arguments[0].value);
    const std::string subject = ValueAsText(request.arguments[1].value);
    std::int64_t occurrence = 1;
    std::int64_t group = 0;
    if (request.arguments.size() >= 3) {
      if (!request.arguments[2].value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "substring_regex occurrence must be int64");
      }
      occurrence = request.arguments[2].value.int64_value;
    }
    if (request.arguments.size() >= 4) {
      if (!request.arguments[3].value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "substring_regex group must be int64");
      }
      group = request.arguments[3].value.int64_value;
    }
    std::regex::flag_type flags;
    bool global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() >= 5 ? ValueAsText(request.arguments[4].value) : "",
                               false,
                               &flags,
                               &global)) {
      return RefuseFunctionInvalidInput(request, "substring_regex flags must contain only 'i' (case-insensitive)");
    }
    if (occurrence < 1) return RefuseFunctionInvalidInput(request, "substring_regex occurrence must be >= 1");
    if (group < 0) return RefuseFunctionInvalidInput(request, "substring_regex group must be >= 0");
    try {
      const std::regex compiled(pattern, flags);
      auto search_begin = subject.cbegin();
      const auto search_end = subject.cend();
      std::int64_t seen = 0;
      while (search_begin <= search_end) {
        std::smatch match;
        if (!std::regex_search(search_begin, search_end, match, compiled)) break;
        ++seen;
        if (seen == occurrence) {
          if (static_cast<std::size_t>(group) >= match.size()) {
            return MakeFunctionSuccess(request, {MakeNullValue("character")});
          }
          return MakeFunctionSuccess(request,
                                     {MakeTextValue("character", match.str(static_cast<std::size_t>(group)))});
        }
        auto next = match[0].second;
        if (match.length(0) == 0 && next != search_end) ++next;
        if (next == search_begin && next == search_end) break;
        search_begin = next;
        if (seen > 1048576) {
          return RefuseFunctionInvalidInput(request, "substring_regex exceeded bounded match limit");
        }
      }
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "substring_regex received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.regexp_replace", "sb.scalar.regexp_replace", "sb.regex.replace", "regexp_replace"})) {
    if (request.arguments.size() < 3 || request.arguments.size() > 4) {
      return RefuseFunctionInvalidInput(request, "regexp_replace expects string, pattern, replacement, and optional flags");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        IsSqlNull(request.arguments[2].value) ||
        (request.arguments.size() == 4 && IsSqlNull(request.arguments[3].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    const std::string subject = ValueAsText(request.arguments[0].value);
    const std::string pattern = ValueAsText(request.arguments[1].value);
    const std::string replacement = ValueAsText(request.arguments[2].value);
    std::regex::flag_type flags;
    bool global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() == 4 ? ValueAsText(request.arguments[3].value) : "",
                               true,
                               &flags,
                               &global)) {
      return RefuseFunctionInvalidInput(request, "regexp_replace flags must contain only 'i' and optional 'g'");
    }
    try {
      const std::regex compiled(pattern, flags);
      auto format_flags = std::regex_constants::match_default;
      if (!global) format_flags |= std::regex_constants::format_first_only;
      return MakeFunctionSuccess(request,
                                 {MakeTextValue("character",
                                                std::regex_replace(subject, compiled, replacement, format_flags))});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "regexp_replace received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.translate_regex", "sb.scalar.translate_regex", "translate_regex"})) {
    if (request.arguments.size() < 3 || request.arguments.size() > 5) {
      return RefuseFunctionInvalidInput(request, "translate_regex expects pattern, string, replacement, optional occurrence/ALL, and flags");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    const std::string pattern = ValueAsText(request.arguments[0].value);
    const std::string subject = ValueAsText(request.arguments[1].value);
    const std::string replacement = ValueAsText(request.arguments[2].value);
    bool replace_all = false;
    std::int64_t occurrence = 1;
    if (request.arguments.size() >= 4) {
      if (request.arguments[3].value.has_int64_value) {
        occurrence = request.arguments[3].value.int64_value;
      } else {
        const std::string occurrence_text = LowerAscii(ValueAsText(request.arguments[3].value));
        if (occurrence_text != "all") {
          return RefuseFunctionInvalidInput(request, "translate_regex occurrence must be int64 or ALL");
        }
        replace_all = true;
      }
    }
    if (!replace_all && occurrence < 1) {
      return RefuseFunctionInvalidInput(request, "translate_regex occurrence must be >= 1");
    }
    std::regex::flag_type flags;
    bool unused_global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() >= 5 ? ValueAsText(request.arguments[4].value) : "",
                               false,
                               &flags,
                               &unused_global)) {
      return RefuseFunctionInvalidInput(request, "translate_regex flags must contain only 'i' (case-insensitive)");
    }
    try {
      const std::regex compiled(pattern, flags);
      if (replace_all) {
        return MakeFunctionSuccess(request,
                                   {MakeTextValue("character",
                                                  std::regex_replace(subject, compiled, replacement))});
      }
      std::string out;
      auto search_begin = subject.cbegin();
      const auto search_end = subject.cend();
      std::int64_t seen = 0;
      while (search_begin <= search_end) {
        std::smatch match;
        if (!std::regex_search(search_begin, search_end, match, compiled)) break;
        ++seen;
        if (seen == occurrence) {
          out.assign(subject.cbegin(), match[0].first);
          out += match.format(replacement);
          out.append(match[0].second, subject.cend());
          return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
        }
        auto next = match[0].second;
        if (match.length(0) == 0 && next != search_end) ++next;
        if (next == search_begin && next == search_end) break;
        search_begin = next;
        if (seen > 1048576) {
          return RefuseFunctionInvalidInput(request, "translate_regex exceeded bounded match limit");
        }
      }
      return MakeFunctionSuccess(request, {MakeTextValue("character", subject)});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "translate_regex received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.regexp_split_to_array",
                "sb.scalar.regexp_split_to_array",
                "sb.scalar.regexp_split_to_table",
                "sb.regex.split_to_array",
                "regexp_split_to_array",
                "regexp_split_to_table"})) {
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "regexp_split_to_array expects text and pattern");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("array")});
    }
    const std::string subject = ValueAsText(request.arguments[0].value);
    const std::string pattern = ValueAsText(request.arguments[1].value);
    try {
      const std::regex compiled(pattern, std::regex::ECMAScript);
      std::vector<std::string> values;
      for (auto it = std::sregex_token_iterator(subject.begin(), subject.end(), compiled, -1);
           it != std::sregex_token_iterator();
           ++it) {
        values.push_back(it->str());
        if (values.size() > 1048576) {
          return RefuseFunctionInvalidInput(request, "regexp_split_to_array exceeded bounded token limit");
        }
      }
      return MakeFunctionSuccess(request, {MakeTextValue("array", JsonArrayFromTextValues(values))});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "regexp_split_to_array received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.regexp_substr", "sb.scalar.regexp_substr", "sb.regex.substr", "regexp_substr"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 6) {
      return RefuseFunctionInvalidInput(request, "regexp_substr expects string, pattern, optional start, occurrence, flags, and group");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    const std::string subject = ValueAsText(request.arguments[0].value);
    const std::string pattern = ValueAsText(request.arguments[1].value);
    std::int64_t start = 1;
    std::int64_t occurrence = 1;
    std::int64_t group = 0;
    if (request.arguments.size() >= 3) {
      if (!request.arguments[2].value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "regexp_substr start must be int64");
      }
      start = request.arguments[2].value.int64_value;
    }
    if (request.arguments.size() >= 4) {
      if (!request.arguments[3].value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "regexp_substr occurrence must be int64");
      }
      occurrence = request.arguments[3].value.int64_value;
    }
    std::regex::flag_type flags;
    bool global = false;
    if (!RegexOptionsFromFlags(request.arguments.size() >= 5 ? ValueAsText(request.arguments[4].value) : "",
                               false,
                               &flags,
                               &global)) {
      return RefuseFunctionInvalidInput(request, "regexp_substr flags must contain only 'i' (case-insensitive)");
    }
    if (request.arguments.size() == 6) {
      if (!request.arguments[5].value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "regexp_substr group must be int64");
      }
      group = request.arguments[5].value.int64_value;
    }
    if (start < 1) return RefuseFunctionInvalidInput(request, "regexp_substr start must be >= 1");
    if (occurrence < 1) return RefuseFunctionInvalidInput(request, "regexp_substr occurrence must be >= 1");
    if (group < 0) return RefuseFunctionInvalidInput(request, "regexp_substr group must be >= 0");
    if (static_cast<std::size_t>(start - 1) > subject.size()) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    try {
      const std::regex compiled(pattern, flags);
      auto search_begin = subject.cbegin() + static_cast<std::ptrdiff_t>(start - 1);
      const auto search_end = subject.cend();
      std::int64_t seen = 0;
      while (search_begin <= search_end) {
        std::smatch match;
        if (!std::regex_search(search_begin, search_end, match, compiled)) break;
        ++seen;
        if (seen == occurrence) {
          if (static_cast<std::size_t>(group) >= match.size()) {
            return MakeFunctionSuccess(request, {MakeNullValue("character")});
          }
          return MakeFunctionSuccess(request,
                                     {MakeTextValue("character", match.str(static_cast<std::size_t>(group)))});
        }
        auto next = match[0].second;
        if (match.length(0) == 0 && next != search_end) ++next;
        if (next == search_begin && next == search_end) break;
        search_begin = next;
      }
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    } catch (const std::regex_error&) {
      return RefuseFunctionInvalidInput(request, "regexp_substr received an invalid regular expression pattern");
    }
  }

  if (IdIs(id, {"data.scalar.starts_with", "sb.fn.string.starts_with", "starts_with"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "starts_with expects value and prefix");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    const auto value = ValueAsText(request.arguments[0].value);
    const auto prefix = ValueAsText(request.arguments[1].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", value.rfind(prefix, 0) == 0 ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.ends_with", "sb.fn.string.ends_with", "ends_with"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "ends_with expects value and suffix");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    const auto value = ValueAsText(request.arguments[0].value);
    const auto suffix = ValueAsText(request.arguments[1].value);
    const bool match = value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", match ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.contains", "sb.fn.string.contains", "contains"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "contains expects value and needle");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", ValueAsText(request.arguments[0].value).find(ValueAsText(request.arguments[1].value)) != std::string::npos ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.if", "data.scalar.iif", "sb.fn.conditional.if",
                "sb.scalar.iif", "if", "iif"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "if/iif expects condition true_value false_value");
    return MakeFunctionSuccess(request, {ValueTruthy(request.arguments[0].value) ? request.arguments[1].value : request.arguments[2].value});
  }

  if (IdIs(id, {"data.scalar.ifnull", "data.scalar.nvl", "data.scalar.isnull", "sb.fn.conditional.ifnull", "sb.scalar.ifnull", "ifnull", "nvl", "isnull"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "ifnull/nvl/isnull expects two arguments");
    return MakeFunctionSuccess(request, {IsSqlNull(request.arguments[0].value) ? request.arguments[1].value : request.arguments[0].value});
  }

  if (IdIs(id, {"data.scalar.coalesce", "data.scalar.coalesce_strict",
                "sb.scalar.coalesce", "sb.scalar.coalesce_strict",
                "coalesce", "coalesce_strict"})) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "coalesce/coalesce_strict expects at least one argument");
    for (const auto& argument : request.arguments) {
      if (!IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {argument.value});
    }
    return MakeFunctionSuccess(request, {MakeNullValue(request.arguments.front().value.descriptor_id)});
  }

  if (IdIs(id, {"data.scalar.nullif", "sb.scalar.nullif", "nullif"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "expected exactly two arguments");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {request.arguments[0].value});
    if (IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {request.arguments[0].value});
    if (ValueAsText(request.arguments[0].value) == ValueAsText(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
    }
    return MakeFunctionSuccess(request, {request.arguments[0].value});
  }

  if (IdIs(id, {"data.scalar.nvl2", "sb.scalar.nvl2", "nvl2"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "nvl2 expects expression, value_if_not_null, value_if_null");
    return MakeFunctionSuccess(request,
                               {IsSqlNull(request.arguments[0].value) ? request.arguments[2].value : request.arguments[1].value});
  }

  if (IdIs(id, {"data.scalar.greatest", "sb.scalar.greatest", "greatest"}) ||
      IdIs(id, {"data.scalar.least", "sb.scalar.least", "least"})) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "greatest/least expects at least one argument");
    const bool want_greatest = IdIs(id, {"data.scalar.greatest", "sb.scalar.greatest", "greatest"});
    std::size_t best_index = request.arguments.size();
    bool best_is_numeric = true;
    double best_real = 0;
    std::string best_text;
    for (std::size_t i = 0; i < request.arguments.size(); ++i) {
      const auto& candidate = request.arguments[i].value;
      if (IsSqlNull(candidate)) continue;
      double candidate_real = 0;
      const bool candidate_is_numeric = best_is_numeric && ValueAsReal64(candidate, &candidate_real);
      if (best_index == request.arguments.size()) {
        best_index = i;
        best_is_numeric = candidate_is_numeric;
        best_real = candidate_real;
        best_text = ValueAsText(candidate);
        continue;
      }
      bool replace = false;
      if (best_is_numeric && candidate_is_numeric) {
        replace = want_greatest ? candidate_real > best_real : candidate_real < best_real;
      } else {
        const std::string candidate_text = ValueAsText(candidate);
        if (best_is_numeric) {
          best_is_numeric = false;
          best_text = std::to_string(best_real);
        }
        replace = want_greatest ? candidate_text > best_text : candidate_text < best_text;
        if (replace) best_text = candidate_text;
      }
      if (replace) {
        best_index = i;
        if (best_is_numeric) best_real = candidate_real;
      }
    }
    if (best_index == request.arguments.size()) {
      return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
    }
    return MakeFunctionSuccess(request, {request.arguments[best_index].value});
  }

  if (IdIs(id, {"data.scalar.sign", "sb.fn.numeric.sign", "sb.scalar.sign", "sign"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "sign expects exactly one argument");
    const auto& value = request.arguments[0].value;
    if (IsSqlNull(value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    double input = 0.0;
    if (!ValueAsReal64(value, &input)) return RefuseFunctionInvalidInput(request, "sign requires a numeric value");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", input < 0.0 ? -1 : (input > 0.0 ? 1 : 0))});
  }

  if (IdIs(id, {"data.scalar.cast", "cast"})) {
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "cast expects value and target descriptor id");
    }
    const auto target_descriptor = LowerAscii(ValueAsText(request.arguments[1].value));
    if (target_descriptor.empty()) return RefuseFunctionInvalidInput(request, "cast target descriptor id is required");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue(target_descriptor)});
    const auto& value = request.arguments[0].value;
    if (target_descriptor == "character" || target_descriptor == "varchar" ||
        target_descriptor == "text" || target_descriptor == "string") {
      return MakeFunctionSuccess(request, {MakeTextValue(target_descriptor, ValueAsText(value))});
    }
    if (target_descriptor == "int64" || target_descriptor == "integer" ||
        target_descriptor == "bigint" || target_descriptor == "int32" ||
        target_descriptor == "int16" || target_descriptor == "int8") {
      std::int64_t parsed = 0;
      if (!ValueAsInt64Strict(value, &parsed)) return RefuseFunctionInvalidInput(request, "cast to int64 requires exact integer text/value");
      if (target_descriptor == "int32" &&
          (parsed < std::numeric_limits<std::int32_t>::min() ||
           parsed > std::numeric_limits<std::int32_t>::max())) {
        return RefuseFunctionInvalidInput(request, "cast to int32 is outside the int32 range");
      }
      if (target_descriptor == "int16" &&
          (parsed < std::numeric_limits<std::int16_t>::min() ||
           parsed > std::numeric_limits<std::int16_t>::max())) {
        return RefuseFunctionInvalidInput(request, "cast to int16 is outside the int16 range");
      }
      if (target_descriptor == "int8" &&
          (parsed < std::numeric_limits<std::int8_t>::min() ||
           parsed > std::numeric_limits<std::int8_t>::max())) {
        return RefuseFunctionInvalidInput(request, "cast to int8 is outside the int8 range");
      }
      const std::string descriptor =
          (target_descriptor == "integer" || target_descriptor == "bigint") ? "int64"
                                                                            : target_descriptor;
      return MakeFunctionSuccess(request, {MakeInt64Value(descriptor, parsed)});
    }
    if (target_descriptor == "uint64" || target_descriptor == "unsigned_bigint" ||
        target_descriptor == "uint32" || target_descriptor == "uint16" ||
        target_descriptor == "uint8") {
      std::uint64_t parsed = 0;
