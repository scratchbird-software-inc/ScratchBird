// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/range/range_function_landing_zone.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::engine::functions {
namespace {

struct Bound {
  bool present = false;
  bool numeric = false;
  double numeric_value = 0.0;
  std::string text;
};

struct ParsedRange {
  bool empty = false;
  Bound lower;
  Bound upper;
  bool lower_inc = false;
  bool upper_inc = false;
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

std::string Trim(std::string_view input) {
  std::size_t first = 0;
  while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) ++first;
  std::size_t last = input.size();
  while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) --last;
  return std::string(input.substr(first, last - first));
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool IdIs(const std::string& id, std::initializer_list<std::string_view> names) {
  for (const auto name : names) {
    const std::string text(name);
    if (id == text || id == "sb.scalar." + text || id == "range." + text || id == "sb.fn.range." + text) {
      return true;
    }
  }
  return false;
}

bool AnyNull(const FunctionCallRequest& request) {
  for (const auto& argument : request.arguments) {
    if (IsSqlNull(argument.value)) return true;
  }
  return false;
}

bool ParseFiniteDouble(std::string_view input, double* out) {
  const auto text = Trim(input);
  if (text.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) return false;
  *out = value;
  return true;
}

std::string UnquoteBound(std::string text) {
  text = Trim(text);
  if (text.size() >= 2 && ((text.front() == '\'' && text.back() == '\'') ||
                           (text.front() == '"' && text.back() == '"'))) {
    text = text.substr(1, text.size() - 2);
  }
  return text;
}

Bound MakeBound(std::string text) {
  Bound bound;
  bound.text = UnquoteBound(std::move(text));
  bound.present = !bound.text.empty();
  if (bound.present) {
    bound.numeric = ParseFiniteDouble(bound.text, &bound.numeric_value);
  }
  return bound;
}

int CompareBounds(const Bound& left, const Bound& right) {
  if (left.numeric && right.numeric) {
    if (left.numeric_value < right.numeric_value) return -1;
    if (left.numeric_value > right.numeric_value) return 1;
    return 0;
  }
  if (left.text < right.text) return -1;
  if (left.text > right.text) return 1;
  return 0;
}

void NormalizeEmpty(ParsedRange* range) {
  if (range->empty || !range->lower.present || !range->upper.present) return;
  const int cmp = CompareBounds(range->lower, range->upper);
  if (cmp > 0 || (cmp == 0 && !(range->lower_inc && range->upper_inc))) {
    range->empty = true;
  }
}

std::optional<std::string> JsonValueForKey(std::string_view input, std::string_view key, bool* is_null = nullptr) {
  if (is_null) *is_null = false;
  const std::string body(input);
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto key_pos = body.find(needle);
  if (key_pos == std::string::npos) return std::nullopt;
  const auto colon = body.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  std::size_t pos = colon + 1;
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
  if (pos >= body.size()) return std::nullopt;

  if (body[pos] == '"') {
    ++pos;
    std::string out;
    bool escaped = false;
    for (; pos < body.size(); ++pos) {
      const char ch = body[pos];
      if (escaped) {
        out.push_back(ch);
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') return out;
      out.push_back(ch);
    }
    return std::nullopt;
  }

  const std::size_t start = pos;
  while (pos < body.size() && body[pos] != ',' && body[pos] != '}') ++pos;
  std::string token = Trim(std::string_view(body).substr(start, pos - start));
  if (LowerAscii(token) == "null") {
    if (is_null) *is_null = true;
    return std::nullopt;
  }
  return token;
}

std::optional<bool> JsonBoolForKey(std::string_view input, std::string_view key) {
  bool is_null = false;
  auto value = JsonValueForKey(input, key, &is_null);
  if (!value || is_null) return std::nullopt;
  const auto lowered = LowerAscii(Trim(*value));
  if (lowered == "true") return true;
  if (lowered == "false") return false;
  return std::nullopt;
}

ParsedRange ParseJsonRange(std::string_view input) {
  ParsedRange range;
  if (JsonBoolForKey(input, "empty").value_or(false)) {
    range.empty = true;
    return range;
  }

  bool lower_null = false;
  bool upper_null = false;
  if (auto lower = JsonValueForKey(input, "lower", &lower_null); lower && !lower_null) {
    range.lower = MakeBound(*lower);
  }
  if (auto upper = JsonValueForKey(input, "upper", &upper_null); upper && !upper_null) {
    range.upper = MakeBound(*upper);
  }
  range.lower_inc = range.lower.present && JsonBoolForKey(input, "lower_inc").value_or(true);
  range.upper_inc = range.upper.present && JsonBoolForKey(input, "upper_inc").value_or(false);
  NormalizeEmpty(&range);
  return range;
}

ParsedRange ParseBracketRange(std::string_view input) {
  const auto text = Trim(input);
  if (LowerAscii(text) == "empty") {
    ParsedRange range;
    range.empty = true;
    return range;
  }
  if (text.size() > 4096) return {.error = "range descriptor exceeds scalar helper limit"};
  if (text.size() < 2 || (text.front() != '[' && text.front() != '(') ||
      (text.back() != ']' && text.back() != ')')) {
    return {.error = "range descriptor must be empty or bracket text like [lower,upper)"};
  }

  bool quoted = false;
  char quote = '\0';
  std::size_t comma = std::string::npos;
  for (std::size_t i = 1; i + 1 < text.size(); ++i) {
    const char ch = text[i];
    if (quoted) {
      if (ch == quote) quoted = false;
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quoted = true;
      quote = ch;
      continue;
    }
    if (ch == ',') {
      comma = i;
      break;
    }
  }
  if (comma == std::string::npos) return {.error = "range descriptor is missing bound separator"};

  ParsedRange range;
  range.lower = MakeBound(text.substr(1, comma - 1));
  range.upper = MakeBound(text.substr(comma + 1, text.size() - comma - 2));
  range.lower_inc = text.front() == '[' && range.lower.present;
  range.upper_inc = text.back() == ']' && range.upper.present;
  NormalizeEmpty(&range);
  return range;
}

ParsedRange ParseRangeValue(const scratchbird::engine::sblr::SblrValue& value) {
  const auto text = Trim(ValueAsText(value));
  if (text.empty()) return {.error = "range descriptor is empty"};
  if (text.front() == '{' && text.back() == '}') return ParseJsonRange(text);
  return ParseBracketRange(text);
}

Bound BoundFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  Bound bound;
  bound.present = true;
  bound.text = Trim(ValueAsText(value));
  if (value.has_real64_value) {
    bound.numeric = std::isfinite(value.real64_value);
    bound.numeric_value = value.real64_value;
  } else if (value.has_int64_value) {
    bound.numeric = true;
    bound.numeric_value = static_cast<double>(value.int64_value);
  } else if (value.has_uint64_value) {
    bound.numeric = true;
    bound.numeric_value = static_cast<double>(value.uint64_value);
  } else {
    bound.numeric = ParseFiniteDouble(bound.text, &bound.numeric_value);
  }
  return bound;
}

bool RangeContainsElement(const ParsedRange& range, const Bound& element) {
  if (range.empty) return false;
  if (range.lower.present) {
    const int cmp = CompareBounds(element, range.lower);
    if (cmp < 0 || (cmp == 0 && !range.lower_inc)) return false;
  }
  if (range.upper.present) {
    const int cmp = CompareBounds(element, range.upper);
    if (cmp > 0 || (cmp == 0 && !range.upper_inc)) return false;
  }
  return true;
}

bool LowerContains(const ParsedRange& left, const ParsedRange& right) {
  if (!left.lower.present) return true;
  if (!right.lower.present) return false;
  const int cmp = CompareBounds(left.lower, right.lower);
  if (cmp < 0) return true;
  if (cmp > 0) return false;
  return left.lower_inc || !right.lower_inc;
}

bool UpperContains(const ParsedRange& left, const ParsedRange& right) {
  if (!left.upper.present) return true;
  if (!right.upper.present) return false;
  const int cmp = CompareBounds(left.upper, right.upper);
  if (cmp > 0) return true;
  if (cmp < 0) return false;
  return left.upper_inc || !right.upper_inc;
}

bool RangeContainsRange(const ParsedRange& left, const ParsedRange& right) {
  if (right.empty) return true;
  if (left.empty) return false;
  return LowerContains(left, right) && UpperContains(left, right);
}

bool RangeStrictlyLeft(const ParsedRange& left, const ParsedRange& right) {
  if (left.empty || right.empty || !left.upper.present || !right.lower.present) return false;
  const int cmp = CompareBounds(left.upper, right.lower);
  if (cmp < 0) return true;
  if (cmp > 0) return false;
  return !(left.upper_inc && right.lower_inc);
}

FunctionCallResult ReturnBool(const FunctionCallRequest& request, bool value) {
  return MakeFunctionSuccess(request, {MakeInt64Value("boolean", value ? 1 : 0)});
}

FunctionCallResult RefuseRangeParse(const FunctionCallRequest& request, const ParsedRange& parsed) {
  return RefuseFunctionInvalidInput(request, parsed.error.empty() ? "invalid range descriptor" : parsed.error);
}

std::optional<ParsedRange> ParseRangeArgument(const FunctionCallRequest& request, std::size_t index,
                                              FunctionCallResult* refusal) {
  auto parsed = ParseRangeValue(request.arguments[index].value);
  if (!parsed.ok()) {
    *refusal = RefuseRangeParse(request, parsed);
    return std::nullopt;
  }
  return parsed;
}

}  // namespace

bool IsRangeFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  return id.rfind("range.", 0) == 0 || id.rfind("sb.fn.range.", 0) == 0 ||
         IdIs(id, {"range_contains", "range_contains_element", "range_lower", "range_lower_inc",
                   "range_overlaps", "range_strictly_left", "range_strictly_right",
                   "range_upper", "range_upper_inc"});
}

FunctionCallResult DispatchRangeFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (IdIs(id, {"range_contains"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "range_contains expects two ranges");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    FunctionCallResult refusal;
    const auto left = ParseRangeArgument(request, 0, &refusal);
    if (!left) return refusal;
    const auto right = ParseRangeArgument(request, 1, &refusal);
    if (!right) return refusal;
    return ReturnBool(request, RangeContainsRange(*left, *right));
  }
  if (IdIs(id, {"range_contains_element"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "range_contains_element expects range and element");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    FunctionCallResult refusal;
    const auto range = ParseRangeArgument(request, 0, &refusal);
    if (!range) return refusal;
    return ReturnBool(request, RangeContainsElement(*range, BoundFromValue(request.arguments[1].value)));
  }
  if (IdIs(id, {"range_lower"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "range_lower expects one range");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    FunctionCallResult refusal;
    const auto range = ParseRangeArgument(request, 0, &refusal);
    if (!range) return refusal;
    if (range->empty || !range->lower.present) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", range->lower.text)});
  }
  if (IdIs(id, {"range_lower_inc"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "range_lower_inc expects one range");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    FunctionCallResult refusal;
    const auto range = ParseRangeArgument(request, 0, &refusal);
    if (!range) return refusal;
    return ReturnBool(request, !range->empty && range->lower.present && range->lower_inc);
  }
  if (IdIs(id, {"range_overlaps"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "range_overlaps expects two ranges");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    FunctionCallResult refusal;
    const auto left = ParseRangeArgument(request, 0, &refusal);
    if (!left) return refusal;
    const auto right = ParseRangeArgument(request, 1, &refusal);
    if (!right) return refusal;
    return ReturnBool(request, !left->empty && !right->empty &&
                                  !RangeStrictlyLeft(*left, *right) && !RangeStrictlyLeft(*right, *left));
  }
  if (IdIs(id, {"range_strictly_left"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "range_strictly_left expects two ranges");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    FunctionCallResult refusal;
    const auto left = ParseRangeArgument(request, 0, &refusal);
    if (!left) return refusal;
    const auto right = ParseRangeArgument(request, 1, &refusal);
    if (!right) return refusal;
    return ReturnBool(request, RangeStrictlyLeft(*left, *right));
  }
  if (IdIs(id, {"range_strictly_right"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "range_strictly_right expects two ranges");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    FunctionCallResult refusal;
    const auto left = ParseRangeArgument(request, 0, &refusal);
    if (!left) return refusal;
    const auto right = ParseRangeArgument(request, 1, &refusal);
    if (!right) return refusal;
    return ReturnBool(request, RangeStrictlyLeft(*right, *left));
  }
  if (IdIs(id, {"range_upper"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "range_upper expects one range");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    FunctionCallResult refusal;
    const auto range = ParseRangeArgument(request, 0, &refusal);
    if (!range) return refusal;
    if (range->empty || !range->upper.present) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", range->upper.text)});
  }
  if (IdIs(id, {"range_upper_inc"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "range_upper_inc expects one range");
    if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    FunctionCallResult refusal;
    const auto range = ParseRangeArgument(request, 0, &refusal);
    if (!range) return refusal;
    return ReturnBool(request, !range->empty && range->upper.present && range->upper_inc);
  }

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_RANGE_FUNCTION_UNHANDLED",
                                      "range helper id is not handled by the activated range scalar surface");
}

}  // namespace scratchbird::engine::functions
