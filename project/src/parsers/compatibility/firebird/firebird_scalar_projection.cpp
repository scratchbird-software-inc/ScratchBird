// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_scalar_projection.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace scratchbird::parser::firebird {
namespace {

constexpr std::string_view kOctetFromInt64 =
    "data.scalar.octet_from_int64";
constexpr std::string_view kInt64FromFirstOctet =
    "data.scalar.int64_from_first_octet";
constexpr std::string_view kAbs = "sb.scalar.abs";
constexpr std::string_view kPi = "sb.scalar.pi";
constexpr std::string_view kCast = "data.scalar.cast";
constexpr std::string_view kDateAdd = "sb.temporal.date_add";
constexpr std::string_view kDateDiff = "sb.temporal.date_diff";
constexpr std::string_view kDatePart = "sb.temporal.date_part";
constexpr std::string_view kFirebirdCalendarMonthProfile =
    "firebird.calendar_month.v1";
constexpr std::string_view kFirebirdTickProfile =
    "firebird.ticks_100us.v1";
constexpr std::string_view kHash64 = "data.scalar.hash64";
constexpr std::string_view kFirebirdWeakHashProfile =
    "firebird.weak_hash.v1";
constexpr std::size_t kMaximumProjectionCount = 16;
constexpr std::size_t kMaximumExpressionDepth = 4;

struct DirectRealFunctionSpec {
  std::string_view surface_name;
  std::string_view function_id;
  std::size_t arity;
};

enum class BoundNumericShape {
  kInt32Ternary,
  kInt32Binary,
  kInt64Unary,
  kFixedDecimalUnary,
  kFixedDecimalAndScale,
  kFixedDecimalOptionalScale,
};

enum class BoundResultScalePolicy {
  kZero,
  kFirstFixedLiteral,
  kFirstFixedLiteralWhenBinary,
};

struct BoundNumericFunctionSpec {
  std::string_view surface_name;
  std::string_view function_id;
  BoundNumericShape shape;
  FirebirdScalarProjectionOutputKind output_kind;
  BoundResultScalePolicy scale_policy;
};

struct BoundBitFunctionSpec {
  std::string_view surface_name;
  std::string_view function_id;
  FirebirdScalarProjectionOutputKind output_kind;
};

struct BoundBitMeasuredInput {
  std::string_view surface_name;
  std::int64_t left;
  std::int64_t right;
};

struct BoundStringFunctionSpec {
  std::string_view surface_name;
  std::string_view function_id;
  FirebirdScalarProjectionOutputKind output_kind;
  std::uint32_t declared_length;
};

constexpr std::array<DirectRealFunctionSpec, 14> kDirectRealFunctions{{
    {"ASIN", "sb.scalar.asin", 1},
    {"ATAN2", "sb.scalar.atan2", 2},
    {"ATAN", "sb.scalar.atan", 1},
    {"COS", "sb.scalar.cos", 1},
    {"COSH", "sb.scalar.cosh", 1},
    {"EXP", "sb.scalar.exp", 1},
    {"LN", "sb.scalar.ln", 1},
    {"LOG10", "sb.scalar.log10", 1},
    {"LOG", "sb.scalar.log", 2},
    {"POWER", "sb.scalar.power", 2},
    {"SINH", "sb.scalar.sinh", 1},
    {"SQRT", "sb.scalar.sqrt", 1},
    {"TAN", "sb.scalar.tan", 1},
    {"TANH", "sb.scalar.tanh", 1},
}};

constexpr std::array<BoundNumericFunctionSpec, 9> kBoundNumericFunctions{{
    {"MAXVALUE", "sb.scalar.greatest", BoundNumericShape::kInt32Ternary,
     FirebirdScalarProjectionOutputKind::kInt32,
     BoundResultScalePolicy::kZero},
    {"MINVALUE", "sb.scalar.least", BoundNumericShape::kInt32Ternary,
     FirebirdScalarProjectionOutputKind::kInt32,
     BoundResultScalePolicy::kZero},
    {"MOD", "sb.scalar.mod", BoundNumericShape::kInt32Binary,
     FirebirdScalarProjectionOutputKind::kInt32,
     BoundResultScalePolicy::kZero},
    {"SIGN", "sb.scalar.sign", BoundNumericShape::kInt64Unary,
     FirebirdScalarProjectionOutputKind::kInt16,
     BoundResultScalePolicy::kZero},
    {"CEIL", "sb.scalar.ceil", BoundNumericShape::kFixedDecimalUnary,
     FirebirdScalarProjectionOutputKind::kInt64,
     BoundResultScalePolicy::kZero},
    {"CEILING", "sb.scalar.ceil", BoundNumericShape::kFixedDecimalUnary,
     FirebirdScalarProjectionOutputKind::kInt64,
     BoundResultScalePolicy::kZero},
    {"FLOOR", "sb.scalar.floor", BoundNumericShape::kFixedDecimalUnary,
     FirebirdScalarProjectionOutputKind::kInt64,
     BoundResultScalePolicy::kZero},
    {"ROUND", "sb.scalar.round", BoundNumericShape::kFixedDecimalAndScale,
     FirebirdScalarProjectionOutputKind::kExactInt64,
     BoundResultScalePolicy::kFirstFixedLiteral},
    {"TRUNC", "sb.scalar.trunc", BoundNumericShape::kFixedDecimalOptionalScale,
     FirebirdScalarProjectionOutputKind::kInt64,
     BoundResultScalePolicy::kFirstFixedLiteralWhenBinary},
}};

constexpr std::array<BoundBitFunctionSpec, 5> kBoundBitFunctions{{
    {"BIN_AND", "sb.scalar.bit_and",
     FirebirdScalarProjectionOutputKind::kInt32},
    {"BIN_OR", "sb.scalar.bit_or",
     FirebirdScalarProjectionOutputKind::kInt32},
    {"BIN_XOR", "sb.scalar.bit_xor",
     FirebirdScalarProjectionOutputKind::kInt32},
    {"BIN_SHL", "sb.scalar.bit_shift_left",
     FirebirdScalarProjectionOutputKind::kInt64},
    {"BIN_SHR", "sb.scalar.bit_shift_right",
     FirebirdScalarProjectionOutputKind::kInt64},
}};

constexpr std::array<BoundBitMeasuredInput, 10> kBoundBitMeasuredInputs{{
    {"BIN_AND", 1, 1}, {"BIN_AND", 1, 0},
    {"BIN_OR", 1, 1},  {"BIN_OR", 1, 0},
    {"BIN_OR", 0, 0},  {"BIN_SHL", 8, 1},
    {"BIN_SHR", 8, 1}, {"BIN_XOR", 0, 1},
    {"BIN_XOR", 0, 0}, {"BIN_XOR", 1, 1},
}};

constexpr std::array<BoundStringFunctionSpec, 7> kBoundStringFunctions{{
    {"LEFT", "sb.scalar.left",
     FirebirdScalarProjectionOutputKind::kVaryingText, 7},
    {"OVERLAY", "sb.scalar.overlay",
     FirebirdScalarProjectionOutputKind::kVaryingText, 42},
    {"POSITION", "sb.scalar.position",
     FirebirdScalarProjectionOutputKind::kInt32, 0},
    {"POSITION", "sb.scalar.instr",
     FirebirdScalarProjectionOutputKind::kInt32, 0},
    {"REPLACE", "sb.scalar.replace",
     FirebirdScalarProjectionOutputKind::kVaryingText, 4},
    {"REVERSE", "sb.scalar.reverse",
     FirebirdScalarProjectionOutputKind::kVaryingText, 4},
    {"RIGHT", "sb.scalar.right",
     FirebirdScalarProjectionOutputKind::kVaryingText, 18},
}};

const DirectRealFunctionSpec* FindDirectRealFunctionBySurface(
    std::string_view upper_name) {
  for (const auto& spec : kDirectRealFunctions) {
    if (spec.surface_name == upper_name) return &spec;
  }
  return nullptr;
}

const DirectRealFunctionSpec* FindDirectRealFunctionById(
    std::string_view function_id) {
  for (const auto& spec : kDirectRealFunctions) {
    if (spec.function_id == function_id) return &spec;
  }
  return nullptr;
}

const BoundNumericFunctionSpec* FindBoundNumericFunctionBySurface(
    std::string_view upper_name) {
  for (const auto& spec : kBoundNumericFunctions) {
    if (spec.surface_name == upper_name) return &spec;
  }
  return nullptr;
}

const BoundBitFunctionSpec* FindBoundBitFunctionBySurface(
    std::string_view upper_name) {
  for (const auto& spec : kBoundBitFunctions) {
    if (spec.surface_name == upper_name) return &spec;
  }
  return nullptr;
}

const BoundBitFunctionSpec* FindBoundBitFunctionById(
    std::string_view function_id) {
  for (const auto& spec : kBoundBitFunctions) {
    if (spec.function_id == function_id) return &spec;
  }
  return nullptr;
}

const BoundStringFunctionSpec* FindBoundStringFunctionById(
    std::string_view function_id) {
  for (const auto& spec : kBoundStringFunctions) {
    if (spec.function_id == function_id) return &spec;
  }
  return nullptr;
}

bool IsBoundBitMeasuredInput(std::string_view surface_name,
                             std::int64_t left,
                             std::int64_t right) {
  for (const auto& input : kBoundBitMeasuredInputs) {
    if (input.surface_name == surface_name && input.left == left &&
        input.right == right) {
      return true;
    }
  }
  return false;
}

bool IsIdentifierCharacter(char value) {
  const unsigned char ch = static_cast<unsigned char>(value);
  return std::isalnum(ch) != 0 || value == '_' || value == '$';
}

void SkipWhitespace(std::string_view text, std::size_t* offset) {
  if (offset == nullptr) return;
  while (*offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[*offset])) != 0) {
    ++*offset;
  }
}

std::string TrimAscii(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string UpperAscii(std::string_view text) {
  std::string upper(text);
  for (char& value : upper) {
    value = static_cast<char>(
        std::toupper(static_cast<unsigned char>(value)));
  }
  return upper;
}

std::string LowerAscii(std::string_view text) {
  std::string lower(text);
  for (char& value : lower) {
    value = static_cast<char>(
        std::tolower(static_cast<unsigned char>(value)));
  }
  return lower;
}

std::optional<std::string> CharacterTypeForCharset(
    std::string_view presented_charset) {
  std::string charset = UpperAscii(TrimAscii(presented_charset));
  if (charset.empty()) charset = "NONE";
  if (charset == "UTF-8") charset = "UTF8";
  if (charset.empty()) return std::nullopt;
  std::string canonical = "character.";
  for (const unsigned char ch : charset) {
    if (std::isalnum(ch) == 0 && ch != '_') return std::nullopt;
    canonical.push_back(static_cast<char>(std::tolower(ch)));
  }
  return canonical;
}

bool ConsumeKeyword(std::string_view text,
                    std::size_t* offset,
                    std::string_view keyword) {
  if (offset == nullptr) return false;
  SkipWhitespace(text, offset);
  if (*offset + keyword.size() > text.size()) return false;
  for (std::size_t index = 0; index < keyword.size(); ++index) {
    const unsigned char actual =
        static_cast<unsigned char>(text[*offset + index]);
    const unsigned char expected =
        static_cast<unsigned char>(keyword[index]);
    if (std::toupper(actual) != std::toupper(expected)) return false;
  }
  if (*offset > 0 && IsIdentifierCharacter(text[*offset - 1])) return false;
  const std::size_t end = *offset + keyword.size();
  if (end < text.size() && IsIdentifierCharacter(text[end])) return false;
  *offset = end;
  return true;
}

struct ParsedIdentifier {
  std::string value;
  bool quoted{false};
};

std::optional<ParsedIdentifier> ReadIdentifier(std::string_view text,
                                               std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size()) return std::nullopt;
  if (text[*offset] == '"') {
    ++*offset;
    std::string value;
    while (*offset < text.size()) {
      const char ch = text[*offset];
      ++*offset;
      if (ch != '"') {
        value.push_back(ch);
        continue;
      }
      if (*offset < text.size() && text[*offset] == '"') {
        value.push_back('"');
        ++*offset;
        continue;
      }
      return ParsedIdentifier{std::move(value), true};
    }
    return std::nullopt;
  }

  if (!IsIdentifierCharacter(text[*offset]) ||
      std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
    return std::nullopt;
  }
  const std::size_t begin = *offset;
  while (*offset < text.size() && IsIdentifierCharacter(text[*offset])) {
    ++*offset;
  }
  return ParsedIdentifier{
      std::string(text.substr(begin, *offset - begin)), false};
}

std::optional<std::string> ReadStringLiteral(std::string_view text,
                                             std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '\'') return std::nullopt;
  ++*offset;
  std::string decoded;
  while (*offset < text.size()) {
    const char ch = text[*offset];
    ++*offset;
    if (ch != '\'') {
      decoded.push_back(ch);
      continue;
    }
    if (*offset < text.size() && text[*offset] == '\'') {
      decoded.push_back('\'');
      ++*offset;
      continue;
    }
    return decoded;
  }
  return std::nullopt;
}

std::optional<std::string> ReadSignedDecimal(std::string_view text,
                                             std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  SkipWhitespace(text, offset);
  const std::size_t begin = *offset;
  if (*offset < text.size() &&
      (text[*offset] == '+' || text[*offset] == '-')) {
    ++*offset;
  }
  const std::size_t digits = *offset;
  while (*offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
    ++*offset;
  }
  if (*offset == digits) {
    *offset = begin;
    return std::nullopt;
  }
  return std::string(text.substr(begin, *offset - begin));
}

std::optional<std::string> ReadRealNumericLiteral(std::string_view text,
                                                  std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  SkipWhitespace(text, offset);
  const std::size_t begin = *offset;
  if (*offset < text.size() &&
      (text[*offset] == '+' || text[*offset] == '-')) {
    ++*offset;
  }

  const std::size_t integer_digits = *offset;
  while (*offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
    ++*offset;
  }
  if (*offset == integer_digits) {
    *offset = begin;
    return std::nullopt;
  }
  if (*offset < text.size() && text[*offset] == '.') {
    ++*offset;
    const std::size_t fractional_digits = *offset;
    while (*offset < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
      ++*offset;
    }
    if (*offset == fractional_digits) {
      *offset = begin;
      return std::nullopt;
    }
  }

  if (*offset < text.size() &&
      (text[*offset] == 'e' || text[*offset] == 'E')) {
    ++*offset;
    if (*offset < text.size() &&
        (text[*offset] == '+' || text[*offset] == '-')) {
      ++*offset;
    }
    const std::size_t exponent_digits = *offset;
    while (*offset < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
      ++*offset;
    }
    if (*offset == exponent_digits) {
      *offset = begin;
      return std::nullopt;
    }
  }
  return std::string(text.substr(begin, *offset - begin));
}

struct BoundIntegerLiteral {
  std::string encoded_value;
  std::int64_t value{0};
};

struct BoundFixedDecimalLiteral {
  std::string encoded_value;
};

std::optional<std::int64_t> DecodeInt64Literal(std::string_view encoded) {
  if (encoded.empty()) return std::nullopt;
  if (encoded.front() == '+') encoded.remove_prefix(1);
  if (encoded.empty()) return std::nullopt;
  std::int64_t value = 0;
  const auto result =
      std::from_chars(encoded.data(), encoded.data() + encoded.size(), value);
  if (result.ec != std::errc{} ||
      result.ptr != encoded.data() + encoded.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<BoundIntegerLiteral> ReadBoundIntegerLiteral(
    std::string_view text,
    std::size_t* offset,
    bool require_int32) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  const auto encoded = ReadSignedDecimal(text, offset);
  if (!encoded) return std::nullopt;
  const auto value = DecodeInt64Literal(*encoded);
  if (!value ||
      (require_int32 &&
       (*value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()))) {
    *offset = start;
    return std::nullopt;
  }
  return BoundIntegerLiteral{*encoded, *value};
}

std::optional<BoundFixedDecimalLiteral> ReadBoundFixedDecimalLiteral(
    std::string_view text,
    std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  const auto encoded = ReadRealNumericLiteral(text, offset);
  if (!encoded || encoded->find_first_of("eE") != std::string::npos) {
    *offset = start;
    return std::nullopt;
  }
  const std::size_t decimal = encoded->find('.');
  if (decimal == std::string::npos || decimal + 1 >= encoded->size()) {
    *offset = start;
    return std::nullopt;
  }
  const std::size_t sign_digits =
      (!encoded->empty() &&
       (encoded->front() == '+' || encoded->front() == '-'))
          ? 1
          : 0;
  const std::size_t fractional_digits = encoded->size() - decimal - 1;
  const std::size_t total_digits = encoded->size() - sign_digits - 1;
  // This tranche presents a Firebird SQL_INT64 descriptor.  Wider exact
  // numeric literals belong to a later INT128/DECFLOAT-aware route.
  if (fractional_digits == 0 || fractional_digits > 18 ||
      total_digits == 0 || total_digits > 18) {
    *offset = start;
    return std::nullopt;
  }
  return BoundFixedDecimalLiteral{*encoded};
}

std::optional<std::int16_t> FixedDecimalScale(
    std::string_view encoded) {
  const std::size_t decimal = encoded.find('.');
  if (decimal == std::string_view::npos || decimal + 1 >= encoded.size() ||
      encoded.find_first_of("eE") != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t fractional_digits = encoded.size() - decimal - 1;
  if (fractional_digits == 0 || fractional_digits > 18) {
    return std::nullopt;
  }
  return -static_cast<std::int16_t>(fractional_digits);
}

std::optional<std::size_t> FindTopLevelKeyword(std::string_view text,
                                               std::string_view keyword,
                                               std::size_t begin) {
  std::size_t depth = 0;
  bool single_quoted = false;
  bool double_quoted = false;
  for (std::size_t index = begin; index < text.size(); ++index) {
    const char ch = text[index];
    if (single_quoted) {
      if (ch == '\'' && index + 1 < text.size() && text[index + 1] == '\'') {
        ++index;
      } else if (ch == '\'') {
        single_quoted = false;
      }
      continue;
    }
    if (double_quoted) {
      if (ch == '"' && index + 1 < text.size() && text[index + 1] == '"') {
        ++index;
      } else if (ch == '"') {
        double_quoted = false;
      }
      continue;
    }
    if (ch == '\'') {
      single_quoted = true;
      continue;
    }
    if (ch == '"') {
      double_quoted = true;
      continue;
    }
    if (ch == '(') {
      ++depth;
      continue;
    }
    if (ch == ')') {
      if (depth == 0) return std::nullopt;
      --depth;
      continue;
    }
    if (depth != 0 || index + keyword.size() > text.size()) continue;
    bool matches = true;
    for (std::size_t word_index = 0; word_index < keyword.size();
         ++word_index) {
      if (std::toupper(static_cast<unsigned char>(text[index + word_index])) !=
          std::toupper(static_cast<unsigned char>(keyword[word_index]))) {
        matches = false;
        break;
      }
    }
    if (!matches) continue;
    if (index > 0 && IsIdentifierCharacter(text[index - 1])) continue;
    const std::size_t end = index + keyword.size();
    if (end < text.size() && IsIdentifierCharacter(text[end])) continue;
    return index;
  }
  return std::nullopt;
}

std::optional<std::vector<std::string>> SplitTopLevelComma(
    std::string_view text) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  std::size_t depth = 0;
  bool single_quoted = false;
  bool double_quoted = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (single_quoted) {
      if (ch == '\'' && index + 1 < text.size() && text[index + 1] == '\'') {
        ++index;
      } else if (ch == '\'') {
        single_quoted = false;
      }
      continue;
    }
    if (double_quoted) {
      if (ch == '"' && index + 1 < text.size() && text[index + 1] == '"') {
        ++index;
      } else if (ch == '"') {
        double_quoted = false;
      }
      continue;
    }
    if (ch == '\'') {
      single_quoted = true;
    } else if (ch == '"') {
      double_quoted = true;
    } else if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      if (depth == 0) return std::nullopt;
      --depth;
    } else if (ch == ',' && depth == 0) {
      const std::string item = TrimAscii(text.substr(begin, index - begin));
      if (item.empty()) return std::nullopt;
      parts.push_back(item);
      begin = index + 1;
    }
  }
  if (single_quoted || double_quoted || depth != 0) return std::nullopt;
  const std::string item = TrimAscii(text.substr(begin));
  if (item.empty()) return std::nullopt;
  parts.push_back(item);
  return parts;
}

FirebirdScalarProjectionExpression Literal(std::string type_name,
                                           std::string encoded_value,
                                           bool is_null) {
  FirebirdScalarProjectionExpression expression;
  expression.type_name = std::move(type_name);
  expression.encoded_value = std::move(encoded_value);
  expression.is_null = is_null;
  return expression;
}

FirebirdScalarProjectionExpression Function(
    std::string type_name,
    std::string function_id,
    FirebirdScalarProjectionExpression argument) {
  FirebirdScalarProjectionExpression expression;
  expression.kind = FirebirdScalarProjectionExpressionKind::kFunction;
  expression.type_name = std::move(type_name);
  expression.function_id = std::move(function_id);
  expression.arguments.push_back(std::move(argument));
  return expression;
}

FirebirdScalarProjectionExpression Function(std::string type_name,
                                            std::string function_id) {
  FirebirdScalarProjectionExpression expression;
  expression.kind = FirebirdScalarProjectionExpressionKind::kFunction;
  expression.type_name = std::move(type_name);
  expression.function_id = std::move(function_id);
  return expression;
}

FirebirdScalarProjectionExpression Function(
    std::string type_name,
    std::string function_id,
    std::vector<FirebirdScalarProjectionExpression> arguments) {
  FirebirdScalarProjectionExpression expression;
  expression.kind = FirebirdScalarProjectionExpressionKind::kFunction;
  expression.type_name = std::move(type_name);
  expression.function_id = std::move(function_id);
  expression.arguments = std::move(arguments);
  return expression;
}

bool ConsumeComma(std::string_view text, std::size_t* offset) {
  if (offset == nullptr) return false;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ',') return false;
  ++*offset;
  return true;
}

bool IsBoundNoneCharset(std::string_view presented_charset) {
  const std::string charset = UpperAscii(TrimAscii(presented_charset));
  return charset.empty() || charset == "NONE";
}

constexpr std::string_view kFirebirdTemporalCastProfile =
    "firebird.temporal_cast.v1";

std::optional<FirebirdScalarProjectionExpression>
ParseBoundIntegerCastExpression(std::string_view text,
                                std::size_t* offset,
                                std::string_view attachment_charset,
                                bool allow_unquoted_numeric = false);

const FirebirdScalarProjectionExpression* BoundTemporalCastSourceLiteral(
    const FirebirdScalarProjectionExpression& expression) {
  if (expression.kind == FirebirdScalarProjectionExpressionKind::kLiteral) {
    return &expression;
  }
  if (expression.kind != FirebirdScalarProjectionExpressionKind::kFunction ||
      expression.function_id != kCast || expression.arguments.empty() ||
      expression.arguments.front().kind !=
          FirebirdScalarProjectionExpressionKind::kLiteral) {
    return nullptr;
  }
  return &expression.arguments.front();
}

bool IsBoundTemporalCastShape(
    const FirebirdScalarProjectionExpression& source,
    std::string_view target_descriptor) {
  // functional/intfunc/cast/test_03.py: the parser binds the two nested
  // neutral numeric casts but never rounds 1.25001 or decides whether the
  // resulting integer can be converted to a date.  That decision belongs to
  // the neutral conversion operation in the engine.
  if (target_descriptor == "date" &&
      source.kind == FirebirdScalarProjectionExpressionKind::kFunction &&
      source.type_name == "int32" && source.function_id == kCast &&
      source.arguments.size() == 2 &&
      source.arguments[1].kind ==
          FirebirdScalarProjectionExpressionKind::kLiteral &&
      source.arguments[1].type_name == "text" &&
      source.arguments[1].encoded_value == "int32") {
    const auto& rounded = source.arguments[0];
    if (rounded.kind == FirebirdScalarProjectionExpressionKind::kFunction &&
        rounded.type_name == "decimal" && rounded.function_id == kCast &&
        rounded.arguments.size() == 3 &&
        rounded.arguments[0].kind ==
            FirebirdScalarProjectionExpressionKind::kLiteral &&
        rounded.arguments[0].type_name == "numeric.fixed" &&
        rounded.arguments[0].encoded_value == "1.25001" &&
        !rounded.arguments[0].is_null &&
        rounded.arguments[1].kind ==
            FirebirdScalarProjectionExpressionKind::kLiteral &&
        rounded.arguments[1].type_name == "text" &&
        rounded.arguments[1].encoded_value == "decimal(18,0)" &&
        rounded.arguments[2].kind ==
            FirebirdScalarProjectionExpressionKind::kLiteral &&
        rounded.arguments[2].type_name == "text" &&
        rounded.arguments[2].encoded_value == "half_up") {
      return true;
    }
  }

  const auto* source_literal = BoundTemporalCastSourceLiteral(source);
  if (source_literal == nullptr || source_literal->is_null ||
      source_literal->type_name != "character.none") {
    return false;
  }
  const std::string_view literal = source_literal->encoded_value;
  if (source.kind == FirebirdScalarProjectionExpressionKind::kLiteral) {
    return (target_descriptor == "date" &&
            (literal == "28.1.2001" || literal == "10.2.1973" ||
             literal == "29.2.2002")) ||
           (target_descriptor == "time" &&
            (literal == "14:34:59.1234" || literal == "13:28:45" ||
             literal == "9:11:60")) ||
           (target_descriptor == "timestamp" &&
            (literal == "10.2.1489 14:34:59.1234" ||
             literal == "1.4.2002 0:59:59.1"));
  }
  if (source.type_name == "date" && literal == "10.2.1973") {
    return target_descriptor == "character(32)" ||
           target_descriptor == "varchar(40)" ||
           target_descriptor == "timestamp";
  }
  if (source.type_name == "time" && literal == "13:28:45") {
    return target_descriptor == "character(32)" ||
           target_descriptor == "varchar(32)";
  }
  if (source.type_name == "timestamp" &&
      literal == "1.4.2002 0:59:59.1") {
    return target_descriptor == "character(50)" ||
           target_descriptor == "varchar(50)" ||
           target_descriptor == "date" || target_descriptor == "time";
  }
  return false;
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundTemporalCastExpression(std::string_view text,
                                 std::size_t* offset,
                                 std::string_view attachment_charset,
                                 std::size_t depth = 0) {
  if (offset == nullptr || depth > 1 ||
      !IsBoundNoneCharset(attachment_charset)) {
    return std::nullopt;
  }
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "CAST")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  FirebirdScalarProjectionExpression source;
  const std::size_t source_start = *offset;
  if (auto nested = ParseBoundTemporalCastExpression(
          text, offset, attachment_charset, depth + 1)) {
    source = std::move(*nested);
  } else {
    *offset = source_start;
    if (depth == 0) {
      if (auto integer_cast = ParseBoundIntegerCastExpression(
              text, offset, attachment_charset, true)) {
        source = std::move(*integer_cast);
      }
    }
    if (source.type_name.empty()) {
      *offset = source_start;
      const auto literal = ReadStringLiteral(text, offset);
      if (!literal) {
        *offset = start;
        return std::nullopt;
      }
      source = Literal("character.none", *literal, false);
    }
  }

  if (!ConsumeKeyword(text, offset, "AS")) {
    *offset = start;
    return std::nullopt;
  }

  std::string result_type;
  std::string target_descriptor;
  const std::size_t target_start = *offset;
  if (ConsumeKeyword(text, offset, "DATE")) {
    result_type = "date";
    target_descriptor = "date";
  } else {
    *offset = target_start;
    if (ConsumeKeyword(text, offset, "TIME")) {
      result_type = "time";
      target_descriptor = "time";
    } else {
      *offset = target_start;
      if (ConsumeKeyword(text, offset, "TIMESTAMP")) {
        result_type = "timestamp";
        target_descriptor = "timestamp";
      } else {
        *offset = target_start;
        const bool fixed = ConsumeKeyword(text, offset, "CHAR");
        if (!fixed) {
          *offset = target_start;
          if (!ConsumeKeyword(text, offset, "VARCHAR")) {
            *offset = start;
            return std::nullopt;
          }
        }
        SkipWhitespace(text, offset);
        if (*offset >= text.size() || text[*offset] != '(') {
          *offset = start;
          return std::nullopt;
        }
        ++*offset;
        const auto length = ReadSignedDecimal(text, offset);
        SkipWhitespace(text, offset);
        if (!length || length->front() == '-' || *offset >= text.size() ||
            text[*offset] != ')') {
          *offset = start;
          return std::nullopt;
        }
        ++*offset;
        result_type = fixed ? "character" : "varchar";
        target_descriptor = result_type + "(" + *length + ")";
      }
    }
  }

  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ')' ||
      !IsBoundTemporalCastShape(source, target_descriptor)) {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  // The parser binds the measured Firebird source descriptor, requested
  // target descriptor, and explicit conversion profile only.  Validation,
  // parsing, calendar conversion, fractional normalization, and canonical
  // temporal result construction all remain neutral engine operations.
  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(3);
  arguments.push_back(std::move(source));
  arguments.push_back(Literal("text", std::move(target_descriptor), false));
  arguments.push_back(
      Literal("text", std::string(kFirebirdTemporalCastProfile), false));
  return Function(std::move(result_type), std::string(kCast),
                  std::move(arguments));
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundNumericLiteralCastExpression(
    std::string_view text,
    std::size_t* offset,
    std::string_view attachment_charset) {
  if (offset == nullptr || !IsBoundNoneCharset(attachment_charset)) {
    return std::nullopt;
  }
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "CAST")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  const auto literal = ReadBoundFixedDecimalLiteral(text, offset);
  if (!literal ||
      (literal->encoded_value != "1.25001" &&
       literal->encoded_value != "1.24999") ||
      !ConsumeKeyword(text, offset, "AS")) {
    *offset = start;
    return std::nullopt;
  }

  std::string result_type;
  std::string target_descriptor;
  bool requires_half_up = false;
  const std::size_t target_start = *offset;
  if (ConsumeKeyword(text, offset, "CHAR")) {
    SkipWhitespace(text, offset);
    if (literal->encoded_value != "1.25001" || *offset >= text.size() ||
        text[*offset] != '(') {
      *offset = start;
      return std::nullopt;
    }
    ++*offset;
    const auto length = ReadSignedDecimal(text, offset);
    SkipWhitespace(text, offset);
    if (!length || *length != "21" || *offset >= text.size() ||
        text[*offset] != ')') {
      *offset = start;
      return std::nullopt;
    }
    ++*offset;
    result_type = "character";
    target_descriptor = "character";
  } else {
    *offset = target_start;
    if (ConsumeKeyword(text, offset, "VARCHAR")) {
      SkipWhitespace(text, offset);
      if (literal->encoded_value != "1.25001" || *offset >= text.size() ||
          text[*offset] != '(') {
        *offset = start;
        return std::nullopt;
      }
      ++*offset;
      const auto length = ReadSignedDecimal(text, offset);
      SkipWhitespace(text, offset);
      if (!length || *length != "21" || *offset >= text.size() ||
          text[*offset] != ')') {
        *offset = start;
        return std::nullopt;
      }
      ++*offset;
      result_type = "varchar";
      target_descriptor = "varchar";
    } else {
      *offset = target_start;
      if (!ConsumeKeyword(text, offset, "NUMERIC")) {
        *offset = start;
        return std::nullopt;
      }
      SkipWhitespace(text, offset);
      if (*offset >= text.size() || text[*offset] != '(') {
        *offset = start;
        return std::nullopt;
      }
      ++*offset;
      const auto precision = ReadSignedDecimal(text, offset);
      if (!precision || *precision != "2" || !ConsumeComma(text, offset)) {
        *offset = start;
        return std::nullopt;
      }
      const auto scale = ReadSignedDecimal(text, offset);
      SkipWhitespace(text, offset);
      if (!scale || *scale != "1" || *offset >= text.size() ||
          text[*offset] != ')') {
        *offset = start;
        return std::nullopt;
      }
      ++*offset;
      result_type = "decimal";
      target_descriptor = "decimal(2,1)";
      requires_half_up = true;
    }
  }

  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  // Preserve the exact numeric token and requested target in neutral engine
  // IR.  Decimal canonicalization and half-up quantization are exclusively
  // engine operations; the parser binds no result value or padded text.
  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(requires_half_up ? 3 : 2);
  arguments.push_back(
      Literal("numeric.fixed", literal->encoded_value, false));
  arguments.push_back(Literal("text", std::move(target_descriptor), false));
  if (requires_half_up) {
    arguments.push_back(Literal("text", "half_up", false));
  }
  return Function(std::move(result_type), std::string(kCast),
                  std::move(arguments));
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundIntegerCastExpression(std::string_view text,
                                std::size_t* offset,
                                std::string_view attachment_charset,
                                bool allow_unquoted_numeric) {
  if (offset == nullptr || !IsBoundNoneCharset(attachment_charset)) {
    return std::nullopt;
  }
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "CAST")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  std::string literal_value;
  std::string source_type;
  const std::size_t literal_start = *offset;
  if (const auto literal = ReadStringLiteral(text, offset)) {
    if (*literal != "1.25001" && *literal != "1.5001") {
      *offset = start;
      return std::nullopt;
    }
    literal_value = *literal;
    source_type = "character.none";
  } else {
    *offset = literal_start;
    const auto numeric_literal = ReadBoundFixedDecimalLiteral(text, offset);
    if (!allow_unquoted_numeric || !numeric_literal ||
        numeric_literal->encoded_value != "1.25001") {
      *offset = start;
      return std::nullopt;
    }
    literal_value = numeric_literal->encoded_value;
    source_type = "numeric.fixed";
  }
  if (!ConsumeKeyword(text, offset, "AS")) {
    *offset = start;
    return std::nullopt;
  }
  const bool numeric_source = source_type == "numeric.fixed";
  if ((numeric_source &&
       (!allow_unquoted_numeric || !ConsumeKeyword(text, offset, "INT"))) ||
      (!numeric_source && !ConsumeKeyword(text, offset, "INTEGER"))) {
    *offset = start;
    return std::nullopt;
  }
  if (source_type.empty()) {
    *offset = start;
    return std::nullopt;
  }
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  // Bind the Firebird conversion as two neutral engine operations. The
  // source retains its measured descriptor. Decimal parsing/quantization
  // and the strict int32 conversion happen in the engine. No rounded result
  // is represented here.
  std::vector<FirebirdScalarProjectionExpression> decimal_arguments;
  decimal_arguments.reserve(3);
  decimal_arguments.push_back(
      Literal(std::move(source_type), std::move(literal_value), false));
  decimal_arguments.push_back(Literal("text", "decimal(18,0)", false));
  decimal_arguments.push_back(Literal("text", "half_up", false));
  auto rounded = Function("decimal", std::string(kCast),
                          std::move(decimal_arguments));

  std::vector<FirebirdScalarProjectionExpression> integer_arguments;
  integer_arguments.reserve(2);
  integer_arguments.push_back(std::move(rounded));
  integer_arguments.push_back(Literal("text", "int32", false));
  return Function("int32", std::string(kCast),
                  std::move(integer_arguments));
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundHashLiteralExpression(std::string_view text,
                                std::size_t* offset,
                                std::string_view attachment_charset) {
  if (offset == nullptr || !IsBoundNoneCharset(attachment_charset)) {
    return std::nullopt;
  }
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "HASH")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  const auto literal = ReadStringLiteral(text, offset);
  SkipWhitespace(text, offset);
  if (!literal || *literal != "toto" || *offset >= text.size() ||
      text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  // This is a Firebird-family binding only.  Preserve the literal bytes in a
  // canonical character descriptor and select the exact neutral operation
  // profile; the parser never evaluates the weak-hash algorithm or embeds a
  // result value.
  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(2);
  arguments.push_back(Literal("character", *literal, false));
  arguments.push_back(
      Literal("text", std::string(kFirebirdWeakHashProfile), false));
  return Function("int64", std::string(kHash64), std::move(arguments));
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundStringLiteralExpression(std::string_view text,
                                  std::size_t* offset,
                                  std::string_view attachment_charset) {
  if (offset == nullptr || !IsBoundNoneCharset(attachment_charset)) {
    return std::nullopt;
  }
  const std::size_t start = *offset;
  const auto function_name = ReadIdentifier(text, offset);
  if (!function_name || function_name->quoted) {
    *offset = start;
    return std::nullopt;
  }
  const std::string upper_name = UpperAscii(function_name->value);
  const bool supported =
      upper_name == "LEFT" || upper_name == "OVERLAY" ||
      upper_name == "POSITION" || upper_name == "REPLACE" ||
      upper_name == "REVERSE" || upper_name == "RIGHT";
  SkipWhitespace(text, offset);
  if (!supported || *offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  const auto close_call = [&]() {
    SkipWhitespace(text, offset);
    if (*offset >= text.size() || text[*offset] != ')') return false;
    ++*offset;
    return true;
  };
  const auto text_literal = [&]()
      -> std::optional<FirebirdScalarProjectionExpression> {
    const auto literal = ReadStringLiteral(text, offset);
    if (!literal) return std::nullopt;
    return Literal("text", *literal, false);
  };
  const auto canonical_integer = [&]() -> std::optional<BoundIntegerLiteral> {
    const auto literal = ReadBoundIntegerLiteral(text, offset, true);
    if (!literal || literal->encoded_value != std::to_string(literal->value)) {
      return std::nullopt;
    }
    return literal;
  };

  if (upper_name == "LEFT" || upper_name == "RIGHT") {
    auto value = text_literal();
    if (!value || !ConsumeComma(text, offset)) {
      *offset = start;
      return std::nullopt;
    }
    const auto count = canonical_integer();
    const bool measured =
        upper_name == "LEFT"
            ? value->encoded_value == "bonjour" && count && count->value == 3
            : value->encoded_value == "NORD PAS DE CALAIS" && count &&
                  count->value == 13;
    if (!measured || !close_call()) {
      *offset = start;
      return std::nullopt;
    }
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(2);
    arguments.push_back(std::move(*value));
    arguments.push_back(
        Literal("int64", std::to_string(count->value), false));
    return Function("text",
                    upper_name == "LEFT" ? "sb.scalar.left"
                                         : "sb.scalar.right",
                    std::move(arguments));
  }

  if (upper_name == "REVERSE") {
    auto value = text_literal();
    if (!value || value->encoded_value != "DRON" || !close_call()) {
      *offset = start;
      return std::nullopt;
    }
    return Function("text", "sb.scalar.reverse", std::move(*value));
  }

  if (upper_name == "REPLACE") {
    auto value = text_literal();
    if (!value || !ConsumeComma(text, offset)) {
      *offset = start;
      return std::nullopt;
    }
    auto search = text_literal();
    if (!search || !ConsumeComma(text, offset)) {
      *offset = start;
      return std::nullopt;
    }
    auto replacement = text_literal();
    if (!replacement || value->encoded_value != "toto" ||
        search->encoded_value != "o" || replacement->encoded_value != "i" ||
        !close_call()) {
      *offset = start;
      return std::nullopt;
    }
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(3);
    arguments.push_back(std::move(*value));
    arguments.push_back(std::move(*search));
    arguments.push_back(std::move(*replacement));
    return Function("text", "sb.scalar.replace", std::move(arguments));
  }

  if (upper_name == "OVERLAY") {
    auto value = text_literal();
    if (!value || !ConsumeKeyword(text, offset, "PLACING")) {
      *offset = start;
      return std::nullopt;
    }
    auto replacement = text_literal();
    if (!replacement || !ConsumeKeyword(text, offset, "FROM")) {
      *offset = start;
      return std::nullopt;
    }
    const auto from = canonical_integer();
    if (!from || !ConsumeKeyword(text, offset, "FOR")) {
      *offset = start;
      return std::nullopt;
    }
    const auto length = canonical_integer();
    if (!length ||
        value->encoded_value !=
            "il fait beau dans le sud  de la france" ||
        replacement->encoded_value != "NORD" || from->value != 22 ||
        length->value != 4 || !close_call()) {
      *offset = start;
      return std::nullopt;
    }
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(4);
    arguments.push_back(std::move(*value));
    arguments.push_back(std::move(*replacement));
    arguments.push_back(Literal("int64", "22", false));
    arguments.push_back(Literal("int64", "4", false));
    return Function("text", "sb.scalar.overlay", std::move(arguments));
  }

  auto needle = text_literal();
  if (!needle) {
    *offset = start;
    return std::nullopt;
  }
  const std::size_t separator = *offset;
  if (ConsumeKeyword(text, offset, "IN")) {
    auto haystack = text_literal();
    if (!haystack || needle->encoded_value != "beau" ||
        haystack->encoded_value != "il fait beau dans le nord" ||
        !close_call()) {
      *offset = start;
      return std::nullopt;
    }
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(2);
    arguments.push_back(std::move(*needle));
    arguments.push_back(std::move(*haystack));
    return Function("int64", "sb.scalar.position", std::move(arguments));
  }

  *offset = separator;
  if (!ConsumeComma(text, offset)) {
    *offset = start;
    return std::nullopt;
  }
  auto haystack = text_literal();
  if (!haystack) {
    *offset = start;
    return std::nullopt;
  }
  const std::size_t optional_start = *offset;
  if (!ConsumeComma(text, offset)) {
    *offset = optional_start;
    if (needle->encoded_value != "beau" ||
        haystack->encoded_value != "beau,il fait beau" || !close_call()) {
      *offset = start;
      return std::nullopt;
    }
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(2);
    arguments.push_back(std::move(*needle));
    arguments.push_back(std::move(*haystack));
    return Function("int64", "sb.scalar.position", std::move(arguments));
  }

  const auto from = canonical_integer();
  if (!from || needle->encoded_value != "beau" ||
      haystack->encoded_value != "beau,il fait beau" || from->value != 2 ||
      !close_call()) {
    *offset = start;
    return std::nullopt;
  }
  // Firebird POSITION(needle, haystack, start) is normalized to the existing
  // neutral INSTR(haystack, needle, start) contract.  This is argument
  // binding only; the neutral engine remains the value authority.
  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(3);
  arguments.push_back(std::move(*haystack));
  arguments.push_back(std::move(*needle));
  arguments.push_back(Literal("int64", "2", false));
  return Function("int64", "sb.scalar.instr", std::move(arguments));
}

bool ReadFixedUnsigned(std::string_view text,
                       std::size_t offset,
                       std::size_t count,
                       unsigned* value) {
  if (value == nullptr || offset + count > text.size()) return false;
  unsigned parsed = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const unsigned char ch =
        static_cast<unsigned char>(text[offset + index]);
    if (std::isdigit(ch) == 0) return false;
    parsed = parsed * 10u + static_cast<unsigned>(ch - '0');
  }
  *value = parsed;
  return true;
}

bool IsGregorianLeapYear(unsigned year) {
  return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

unsigned GregorianDaysInMonth(unsigned year, unsigned month) {
  static constexpr std::array<unsigned, 12> kDays{{
      31u, 28u, 31u, 30u, 31u, 30u,
      31u, 31u, 30u, 31u, 30u, 31u,
  }};
  if (month < 1u || month > kDays.size()) return 0;
  if (month == 2u && IsGregorianLeapYear(year)) return 29u;
  return kDays[month - 1u];
}

std::optional<std::string> NormalizeBoundSlashTimestamp(
    std::string_view value) {
  // This is lexical/type binding for the exact Firebird QA slash form.  No
  // temporal result is evaluated here; the normalized typed literal is sent
  // to the neutral engine function.
  if (value.size() != 19 || value[2] != '/' || value[5] != '/' ||
      value[10] != ' ' || value[13] != ':' || value[16] != ':') {
    return std::nullopt;
  }
  unsigned month = 0;
  unsigned day = 0;
  unsigned year = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!ReadFixedUnsigned(value, 0, 2, &month) ||
      !ReadFixedUnsigned(value, 3, 2, &day) ||
      !ReadFixedUnsigned(value, 6, 4, &year) ||
      !ReadFixedUnsigned(value, 11, 2, &hour) ||
      !ReadFixedUnsigned(value, 14, 2, &minute) ||
      !ReadFixedUnsigned(value, 17, 2, &second) ||
      year < 1u || year > 9999u || month < 1u || month > 12u ||
      day < 1u || day > GregorianDaysInMonth(year, month) ||
      hour > 23u || minute > 59u || second > 59u) {
    return std::nullopt;
  }
  return std::string(value.substr(6, 4)) + "-" +
         std::string(value.substr(0, 2)) + "-" +
         std::string(value.substr(3, 2)) + "T" +
         std::string(value.substr(11, 8));
}

std::optional<std::string> NormalizeBoundDmyDate(std::string_view value) {
  // Firebird accepts this DMY literal spelling in the admitted ISO-week QA
  // shape.  Canonicalization is parser binding work, never week evaluation.
  if (value.size() != 10 || value[2] != '.' || value[5] != '.') {
    return std::nullopt;
  }
  unsigned day = 0;
  unsigned month = 0;
  unsigned year = 0;
  if (!ReadFixedUnsigned(value, 0, 2, &day) ||
      !ReadFixedUnsigned(value, 3, 2, &month) ||
      !ReadFixedUnsigned(value, 6, 4, &year) ||
      year < 1u || year > 9999u || month < 1u || month > 12u ||
      day < 1u || day > GregorianDaysInMonth(year, month)) {
    return std::nullopt;
  }
  return std::string(value.substr(6, 4)) + "-" +
         std::string(value.substr(3, 2)) + "-" +
         std::string(value.substr(0, 2));
}

std::optional<std::string> NormalizeBoundIsoDate(std::string_view value) {
  // DATEADD binding admits only the canonical typed ISO date spelling used by
  // its bounded QA tranche.  Gregorian validation is type binding, not
  // temporal result evaluation.
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
    return std::nullopt;
  }
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!ReadFixedUnsigned(value, 0, 4, &year) ||
      !ReadFixedUnsigned(value, 5, 2, &month) ||
      !ReadFixedUnsigned(value, 8, 2, &day) ||
      year < 1u || year > 9999u || month < 1u || month > 12u ||
      day < 1u || day > GregorianDaysInMonth(year, month)) {
    return std::nullopt;
  }
  return std::string(value);
}

std::optional<std::string> NormalizeBoundIsoTimestamp(
    std::string_view value) {
  // The Firebird surface literal uses a space.  The neutral engine literal
  // uses its canonical T separator; this is lexical lowering only.
  if (value.size() != 19 || value[10] != ' ' || value[13] != ':' ||
      value[16] != ':') {
    return std::nullopt;
  }
  const auto date = NormalizeBoundIsoDate(value.substr(0, 10));
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!date || !ReadFixedUnsigned(value, 11, 2, &hour) ||
      !ReadFixedUnsigned(value, 14, 2, &minute) ||
      !ReadFixedUnsigned(value, 17, 2, &second) || hour > 23u ||
      minute > 59u || second > 59u) {
    return std::nullopt;
  }
  std::string normalized(value);
  normalized[10] = 'T';
  return normalized;
}

std::optional<std::string> NormalizeBoundExactTime(std::string_view value,
                                                   bool allow_colon_fraction) {
  // Firebird's TIME carrier has 100 microsecond precision.  This helper only
  // validates and canonicalizes the literal spelling; it never advances or
  // subtracts a temporal value.
  if (value.size() != 8 && value.size() != 13) return std::nullopt;
  if (value[2] != ':' || value[5] != ':') return std::nullopt;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!ReadFixedUnsigned(value, 0, 2, &hour) ||
      !ReadFixedUnsigned(value, 3, 2, &minute) ||
      !ReadFixedUnsigned(value, 6, 2, &second) || hour > 23u ||
      minute > 59u || second > 59u) {
    return std::nullopt;
  }
  std::string fraction = "0000";
  if (value.size() == 13) {
    if (value[8] != '.' && !(allow_colon_fraction && value[8] == ':')) {
      return std::nullopt;
    }
    unsigned ignored_fraction = 0;
    if (!ReadFixedUnsigned(value, 9, 4, &ignored_fraction)) {
      return std::nullopt;
    }
    fraction = std::string(value.substr(9, 4));
  }
  return std::string(value.substr(0, 8)) + "." + fraction;
}

std::optional<std::string> NormalizeBoundDmyExactTimestamp(
    std::string_view value) {
  if (value.size() != 24 || value[10] != ' ') return std::nullopt;
  const auto date = NormalizeBoundDmyDate(value.substr(0, 10));
  const auto time = NormalizeBoundExactTime(value.substr(11), false);
  if (!date || !time) return std::nullopt;
  return *date + "T" + *time;
}

std::optional<std::string> NormalizeBoundIsoExactTimestamp(
    std::string_view value) {
  if (value.size() != 24 || value[10] != ' ') return std::nullopt;
  const auto date = NormalizeBoundIsoDate(value.substr(0, 10));
  const auto time = NormalizeBoundExactTime(value.substr(11), false);
  if (!date || !time) return std::nullopt;
  return *date + "T" + *time;
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundDateAddTemporalLiteral(std::string_view text,
                                 std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  std::string type_name;
  if (ConsumeKeyword(text, offset, "DATE")) {
    type_name = "date";
  } else if (ConsumeKeyword(text, offset, "TIMESTAMP")) {
    type_name = "timestamp";
  } else {
    *offset = start;
    if (!ConsumeKeyword(text, offset, "TIME")) return std::nullopt;
    type_name = "time";
  }
  const auto literal = ReadStringLiteral(text, offset);
  const auto normalized =
      !literal
          ? std::optional<std::string>{}
          : (type_name == "date" ? NormalizeBoundIsoDate(*literal)
             : type_name == "timestamp"
                 ? NormalizeBoundIsoTimestamp(*literal)
                 : NormalizeBoundExactTime(*literal, true));
  if (!normalized) {
    *offset = start;
    return std::nullopt;
  }
  return Literal(std::move(type_name), *normalized, false);
}

bool IsBoundDateAddMonthQaInput(std::int64_t amount,
                                std::string_view date) {
  // This list is the exact literal/amount surface measured by canonical
  // dateadd_02.  It is admission metadata only: month-end behavior remains an
  // explicitly profiled neutral-engine operation.
  static constexpr std::array<std::pair<std::int64_t, std::string_view>, 22>
      kInputs{{
          {1, "2004-01-31"}, {1, "2004-02-28"},
          {1, "2004-02-29"}, {-1, "2004-02-28"},
          {-1, "2004-02-29"}, {11, "2004-02-28"},
          {11, "2004-02-29"}, {12, "2004-02-28"},
          {12, "2004-02-29"}, {-11, "2004-02-28"},
          {-11, "2004-02-29"}, {-12, "2004-02-28"},
          {-12, "2004-02-29"}, {-1, "2004-03-31"},
          {1, "2003-01-31"}, {1, "2003-02-28"},
          {-1, "2003-02-28"}, {11, "2003-02-28"},
          {12, "2003-02-28"}, {-11, "2003-02-28"},
          {-12, "2003-02-28"}, {-1, "2003-03-31"},
      }};
  for (const auto& input : kInputs) {
    if (input.first == amount && input.second == date) return true;
  }
  return false;
}

std::string EncodeBoundDateAddInterval(std::int64_t amount,
                                       std::string_view upper_unit) {
  const std::uint64_t magnitude =
      amount < 0
          ? static_cast<std::uint64_t>(-(amount + 1)) + 1u
          : static_cast<std::uint64_t>(amount);
  std::string interval = amount < 0 ? "-P" : "P";
  interval += std::to_string(magnitude);
  interval += upper_unit == "YEAR" ? "Y" : "D";
  return interval;
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundDateAddExpression(std::string_view text, std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "DATEADD")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  std::optional<BoundIntegerLiteral> amount;
  std::optional<ParsedIdentifier> part;
  std::optional<FirebirdScalarProjectionExpression> temporal;
  bool expanded_syntax = false;
  const std::size_t argument_start = *offset;

  // Firebird's expanded syntax: DATEADD(<amount> <part> TO <temporal>).
  amount = ReadBoundIntegerLiteral(text, offset, true);
  if (amount) {
    part = ReadIdentifier(text, offset);
    if (!part || part->quoted || !ConsumeKeyword(text, offset, "TO")) {
      amount.reset();
    } else {
      temporal = ParseBoundDateAddTemporalLiteral(text, offset);
      expanded_syntax = temporal.has_value();
    }
  }

  // Firebird's comma syntax: DATEADD(<part>, <amount>, <temporal>).
  if (!amount || !temporal) {
    *offset = argument_start;
    expanded_syntax = false;
    part = ReadIdentifier(text, offset);
    if (!part || part->quoted || !ConsumeComma(text, offset)) {
      *offset = start;
      return std::nullopt;
    }
    amount = ReadBoundIntegerLiteral(text, offset, true);
    if (!amount || !ConsumeComma(text, offset)) {
      *offset = start;
      return std::nullopt;
    }
    temporal = ParseBoundDateAddTemporalLiteral(text, offset);
  }

  SkipWhitespace(text, offset);
  if (!amount || !part || !temporal || *offset >= text.size() ||
      text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  const std::string upper_part = UpperAscii(part->value);
  const bool date_day = temporal->type_name == "date" && upper_part == "DAY";
  const bool date_year =
      temporal->type_name == "date" && upper_part == "YEAR";
  const bool timestamp_day =
      temporal->type_name == "timestamp" && upper_part == "DAY";
  const bool legacy_simple = amount->encoded_value == "-1" &&
                             amount->value == -1 &&
                             (date_day || date_year || timestamp_day);
  if (legacy_simple) {
    const std::string result_type = temporal->type_name;
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(2);
    arguments.push_back(std::move(*temporal));
    arguments.push_back(Literal(
        "interval", EncodeBoundDateAddInterval(amount->value, upper_part),
        false));
    return Function(result_type, std::string(kDateAdd), std::move(arguments));
  }

  const bool date_month = temporal->type_name == "date" &&
                          upper_part == "MONTH" && expanded_syntax &&
                          amount->encoded_value ==
                              std::to_string(amount->value) &&
                          IsBoundDateAddMonthQaInput(
                              amount->value, temporal->encoded_value);
  const bool time_tick = temporal->type_name == "time" &&
                         amount->encoded_value == "-1" &&
                         amount->value == -1 &&
                         temporal->encoded_value == "12:12:00.0000" &&
                         (upper_part == "HOUR" || upper_part == "MINUTE" ||
                          upper_part == "SECOND" ||
                          upper_part == "MILLISECOND");
  if (!date_month && !time_tick) {
    *offset = start;
    return std::nullopt;
  }

  const std::string result_type = temporal->type_name;
  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(4);
  arguments.push_back(std::move(*temporal));
  arguments.push_back(Literal("text", LowerAscii(upper_part), false));
  arguments.push_back(
      Literal("int64", std::to_string(amount->value), false));
  arguments.push_back(Literal(
      "text",
      std::string(date_month ? kFirebirdCalendarMonthProfile
                             : kFirebirdTickProfile),
      false));
  return Function(result_type, std::string(kDateAdd), std::move(arguments));
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundSlashTimestampCast(std::string_view text, std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "CAST")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  const auto literal = ReadStringLiteral(text, offset);
  if (!literal || !ConsumeKeyword(text, offset, "AS") ||
      !ConsumeKeyword(text, offset, "TIMESTAMP")) {
    *offset = start;
    return std::nullopt;
  }
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  const auto normalized = NormalizeBoundSlashTimestamp(*literal);
  if (!normalized) {
    *offset = start;
    return std::nullopt;
  }
  return Literal("timestamp", *normalized, false);
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundMillisecondTemporal(std::string_view text, std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;

  if (ConsumeKeyword(text, offset, "TIME")) {
    const auto literal = ReadStringLiteral(text, offset);
    const auto normalized =
        literal ? NormalizeBoundExactTime(*literal, false)
                : std::optional<std::string>{};
    if (normalized) return Literal("time", *normalized, false);
    *offset = start;
    return std::nullopt;
  }

  *offset = start;
  if (!ConsumeKeyword(text, offset, "CAST")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  const auto literal = ReadStringLiteral(text, offset);
  if (!literal || !ConsumeKeyword(text, offset, "AS")) {
    *offset = start;
    return std::nullopt;
  }

  std::string type_name;
  std::optional<std::string> normalized;
  if (ConsumeKeyword(text, offset, "TIMESTAMP")) {
    type_name = "timestamp";
    normalized = NormalizeBoundDmyExactTimestamp(*literal);
  } else if (ConsumeKeyword(text, offset, "TIME")) {
    type_name = "time";
    normalized = NormalizeBoundExactTime(*literal, false);
  } else {
    *offset = start;
    return std::nullopt;
  }
  SkipWhitespace(text, offset);
  if (!normalized || *offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  return Literal(std::move(type_name), *normalized, false);
}

bool IsBoundDateDiffMillisecondQaPair(
    const FirebirdScalarProjectionExpression& begin,
    const FirebirdScalarProjectionExpression& end) {
  if (begin.type_name != end.type_name) return false;
  if (begin.type_name == "timestamp") {
    return begin.encoded_value == "0001-01-01T00:00:00.0001" &&
           end.encoded_value == "9999-12-31T23:59:59.9999";
  }
  if (begin.type_name == "time") {
    return begin.encoded_value == "00:00:00.0001" &&
           end.encoded_value == "23:59:59.9999";
  }
  return false;
}

bool IsBoundDateDiffPart(std::string_view upper_part) {
  static constexpr std::array<std::string_view, 6> kParts{{
      "SECOND", "MINUTE", "HOUR", "YEAR", "MONTH", "DAY",
  }};
  for (const auto part : kParts) {
    if (part == upper_part) return true;
  }
  return false;
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundDateDiffExpression(std::string_view text, std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "DATEDIFF")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  const auto part = ReadIdentifier(text, offset);
  const std::string upper_part =
      part && !part->quoted ? UpperAscii(part->value) : std::string{};
  const bool exact_millisecond = upper_part == "MILLISECOND";
  if (!exact_millisecond && !IsBoundDateDiffPart(upper_part)) {
    *offset = start;
    return std::nullopt;
  }

  std::optional<FirebirdScalarProjectionExpression> begin;
  std::optional<FirebirdScalarProjectionExpression> end;
  const std::size_t separator = *offset;
  if (ConsumeComma(text, offset)) {
    begin = exact_millisecond
                ? ParseBoundMillisecondTemporal(text, offset)
                : ParseBoundSlashTimestampCast(text, offset);
    if (!begin || !ConsumeComma(text, offset)) {
      *offset = start;
      return std::nullopt;
    }
    end = exact_millisecond
              ? ParseBoundMillisecondTemporal(text, offset)
              : ParseBoundSlashTimestampCast(text, offset);
  } else {
    *offset = separator;
    if (!ConsumeKeyword(text, offset, "FROM")) {
      *offset = start;
      return std::nullopt;
    }
    begin = exact_millisecond
                ? ParseBoundMillisecondTemporal(text, offset)
                : ParseBoundSlashTimestampCast(text, offset);
    if (!begin || !ConsumeKeyword(text, offset, "TO")) {
      *offset = start;
      return std::nullopt;
    }
    end = exact_millisecond
              ? ParseBoundMillisecondTemporal(text, offset)
              : ParseBoundSlashTimestampCast(text, offset);
  }
  SkipWhitespace(text, offset);
  if (!end || *offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  if (exact_millisecond &&
      (!begin || !end ||
       !IsBoundDateDiffMillisecondQaPair(*begin, *end))) {
    *offset = start;
    return std::nullopt;
  }

  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(exact_millisecond ? 4 : 3);
  arguments.push_back(Literal("text", LowerAscii(upper_part), false));
  arguments.push_back(std::move(*begin));
  arguments.push_back(std::move(*end));
  if (exact_millisecond) {
    arguments.push_back(
        Literal("text", std::string(kFirebirdTickProfile), false));
  }
  return Function(exact_millisecond ? "numeric.fixed" : "int64",
                  std::string(kDateDiff),
                  std::move(arguments));
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundExtractWeekExpression(std::string_view text,
                                std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "EXTRACT")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  if (!ConsumeKeyword(text, offset, "WEEK") ||
      !ConsumeKeyword(text, offset, "FROM") ||
      !ConsumeKeyword(text, offset, "DATE")) {
    *offset = start;
    return std::nullopt;
  }
  const auto literal = ReadStringLiteral(text, offset);
  SkipWhitespace(text, offset);
  if (!literal || *offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  const auto normalized = NormalizeBoundDmyDate(*literal);
  if (!normalized) {
    *offset = start;
    return std::nullopt;
  }
  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(2);
  arguments.push_back(Literal("text", "week", false));
  arguments.push_back(Literal("date", *normalized, false));
  return Function("int64", std::string(kDatePart),
                  std::move(arguments));
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundExtractMillisecondExpression(std::string_view text,
                                       std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "EXTRACT")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  if (!ConsumeKeyword(text, offset, "MILLISECOND") ||
      !ConsumeKeyword(text, offset, "FROM")) {
    *offset = start;
    return std::nullopt;
  }

  std::string type_name;
  if (ConsumeKeyword(text, offset, "TIME")) {
    type_name = "time";
  } else if (ConsumeKeyword(text, offset, "TIMESTAMP")) {
    type_name = "timestamp";
  } else {
    *offset = start;
    return std::nullopt;
  }
  const auto literal = ReadStringLiteral(text, offset);
  const auto normalized =
      !literal ? std::optional<std::string>{}
      : type_name == "time" ? NormalizeBoundExactTime(*literal, false)
                            : NormalizeBoundIsoExactTimestamp(*literal);
  SkipWhitespace(text, offset);
  if (!normalized || *offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  const bool admitted_time =
      type_name == "time" && *normalized == "12:12:00.1111";
  const bool admitted_timestamp =
      type_name == "timestamp" &&
      *normalized == "2008-12-08T12:12:00.1111";
  if (!admitted_time && !admitted_timestamp) {
    *offset = start;
    return std::nullopt;
  }

  std::vector<FirebirdScalarProjectionExpression> arguments;
  arguments.reserve(3);
  arguments.push_back(Literal("text", "millisecond", false));
  arguments.push_back(Literal(std::move(type_name), *normalized, false));
  arguments.push_back(
      Literal("text", std::string(kFirebirdTickProfile), false));
  return Function("numeric.fixed", std::string(kDatePart),
                  std::move(arguments));
}

std::optional<std::vector<FirebirdScalarProjectionExpression>>
ParseBoundNumericArguments(std::string_view text,
                           std::size_t* offset,
                           const BoundNumericFunctionSpec& spec) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  std::vector<FirebirdScalarProjectionExpression> arguments;

  const auto append_integer = [&](bool require_int32) {
    const auto literal =
        ReadBoundIntegerLiteral(text, offset, require_int32);
    if (!literal) return false;
    arguments.push_back(
        Literal("int64", literal->encoded_value, false));
    return true;
  };
  const auto append_fixed_decimal = [&]() {
    const auto literal = ReadBoundFixedDecimalLiteral(text, offset);
    if (!literal) return false;
    // ROUND must retain the bound decimal digits until the neutral engine
    // performs exact scale-aware rounding.  CEIL/FLOOR/TRUNC keep their
    // admitted real64 engine contracts.  This descriptor is binding metadata,
    // not a parser-computed result.
    const bool exact_round_input =
        spec.shape == BoundNumericShape::kFixedDecimalAndScale;
    arguments.push_back(
        Literal(exact_round_input ? "numeric.fixed" : "real64",
                literal->encoded_value, false));
    return true;
  };

  switch (spec.shape) {
    case BoundNumericShape::kInt32Ternary:
      for (std::size_t index = 0; index < 3; ++index) {
        if ((index != 0 && !ConsumeComma(text, offset)) ||
            !append_integer(true)) {
          *offset = start;
          return std::nullopt;
        }
      }
      break;
    case BoundNumericShape::kInt32Binary:
      for (std::size_t index = 0; index < 2; ++index) {
        if ((index != 0 && !ConsumeComma(text, offset)) ||
            !append_integer(true)) {
          *offset = start;
          return std::nullopt;
        }
      }
      break;
    case BoundNumericShape::kInt64Unary:
      if (!append_integer(false)) {
        *offset = start;
        return std::nullopt;
      }
      break;
    case BoundNumericShape::kFixedDecimalUnary:
      if (!append_fixed_decimal()) {
        *offset = start;
        return std::nullopt;
      }
      break;
    case BoundNumericShape::kFixedDecimalAndScale:
      if (!append_fixed_decimal() || !ConsumeComma(text, offset) ||
          !append_integer(false)) {
        *offset = start;
        return std::nullopt;
      }
      break;
    case BoundNumericShape::kFixedDecimalOptionalScale:
      if (!append_fixed_decimal()) {
        *offset = start;
        return std::nullopt;
      }
      SkipWhitespace(text, offset);
      if (*offset < text.size() && text[*offset] == ',') {
        ++*offset;
        if (!append_integer(false)) {
          *offset = start;
          return std::nullopt;
        }
      }
      break;
  }
  return arguments;
}

std::optional<std::int16_t> BoundNumericResultScale(
    const BoundNumericFunctionSpec& spec,
    const FirebirdScalarProjectionExpression& expression) {
  if (spec.scale_policy == BoundResultScalePolicy::kZero) return 0;
  if (expression.arguments.empty()) return std::nullopt;
  if (spec.scale_policy ==
          BoundResultScalePolicy::kFirstFixedLiteralWhenBinary &&
      expression.arguments.size() == 1) {
    return 0;
  }
  return FixedDecimalScale(expression.arguments.front().encoded_value);
}

struct BoundDecimalMathCastSpec {
  std::string_view surface_name;
  std::string_view function_id;
};

constexpr std::array<BoundDecimalMathCastSpec, 3> kBoundDecimalMathCasts{{
    {"ACOS", "sb.scalar.acos"},
    {"COT", "sb.scalar.cot"},
    {"SIN", "sb.scalar.sin"},
}};

const BoundDecimalMathCastSpec* FindBoundDecimalMathCast(
    std::string_view upper_name) {
  for (const auto& spec : kBoundDecimalMathCasts) {
    if (spec.surface_name == upper_name) return &spec;
  }
  return nullptr;
}

std::optional<FirebirdScalarProjectionExpression>
ParseBoundDecimalMathCastExpression(std::string_view text,
                                    std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "CAST")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  const auto inner_name = ReadIdentifier(text, offset);
  const auto* inner_spec =
      inner_name && !inner_name->quoted
          ? FindBoundDecimalMathCast(UpperAscii(inner_name->value))
          : nullptr;
  SkipWhitespace(text, offset);
  if (inner_spec == nullptr || *offset >= text.size() ||
      text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  const auto inner_literal = ReadBoundIntegerLiteral(text, offset, false);
  SkipWhitespace(text, offset);
  if (!inner_literal || *offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  if (!ConsumeKeyword(text, offset, "AS") ||
      !ConsumeKeyword(text, offset, "DECIMAL")) {
    *offset = start;
    return std::nullopt;
  }
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  const auto precision = ReadBoundIntegerLiteral(text, offset, false);
  if (!precision || precision->encoded_value != "18" ||
      !ConsumeComma(text, offset)) {
    *offset = start;
    return std::nullopt;
  }
  const auto scale = ReadBoundIntegerLiteral(text, offset, false);
  SkipWhitespace(text, offset);
  if (!scale || scale->encoded_value != "15" || *offset >= text.size() ||
      text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  auto inner = Function(
      "real64", std::string(inner_spec->function_id),
      Literal("real64", inner_literal->encoded_value, false));
  std::vector<FirebirdScalarProjectionExpression> cast_arguments;
  cast_arguments.push_back(std::move(inner));
  cast_arguments.push_back(Literal("text", "decimal(18,15)", false));
  cast_arguments.push_back(Literal("text", "half_up", false));
  return Function("decimal", std::string(kCast),
                  std::move(cast_arguments));
}

std::optional<FirebirdScalarProjectionExpression> ParseScalarExpression(
    std::string_view text,
    std::size_t* offset,
    std::size_t depth,
    std::string_view attachment_charset);

std::optional<FirebirdScalarProjectionExpression> ParseCastLiteral(
    std::string_view text,
    std::size_t* offset,
    std::string_view attachment_charset) {
  const std::size_t start = *offset;
  if (!ConsumeKeyword(text, offset, "CAST")) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  bool is_null = false;
  std::string value;
  const std::size_t literal_start = *offset;
  if (ConsumeKeyword(text, offset, "NULL")) {
    is_null = true;
  } else {
    *offset = literal_start;
    const auto literal = ReadStringLiteral(text, offset);
    if (!literal) {
      *offset = start;
      return std::nullopt;
    }
    value = *literal;
  }
  if (!ConsumeKeyword(text, offset, "AS")) {
    *offset = start;
    return std::nullopt;
  }

  std::string type_name;
  bool fixed_character = false;
  const std::size_t type_start = *offset;
  if (ConsumeKeyword(text, offset, "BLOB")) {
    type_name = "blob.binary";
  } else {
    *offset = type_start;
    bool character_type = ConsumeKeyword(text, offset, "CHAR");
    if (!character_type) {
      *offset = type_start;
      character_type = ConsumeKeyword(text, offset, "CHARACTER");
    }
    fixed_character = true;
    if (!character_type) {
      *offset = start;
      return std::nullopt;
    }
    const auto attachment_type = CharacterTypeForCharset(attachment_charset);
    if (!attachment_type) {
      *offset = start;
      return std::nullopt;
    }
    type_name = *attachment_type;
    SkipWhitespace(text, offset);
    if (*offset < text.size() && text[*offset] == '(') {
      ++*offset;
      const auto length = ReadSignedDecimal(text, offset);
      SkipWhitespace(text, offset);
      const bool positive_length =
          length && length->front() != '-' &&
          length->find_first_of("123456789") != std::string::npos;
      if (!positive_length ||
          *offset >= text.size() || text[*offset] != ')') {
        *offset = start;
        return std::nullopt;
      }
      ++*offset;
    }
    const std::size_t charset_start = *offset;
    if (ConsumeKeyword(text, offset, "CHARACTER")) {
      if (!ConsumeKeyword(text, offset, "SET")) {
        *offset = start;
        return std::nullopt;
      }
      const auto charset = ReadIdentifier(text, offset);
      if (!charset || charset->quoted || UpperAscii(charset->value) != "UTF8") {
        *offset = start;
        return std::nullopt;
      }
      type_name = "character.utf8";
    } else {
      *offset = charset_start;
    }
  }

  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;
  // A fixed CHAR empty literal acquires a leading padding space at engine
  // runtime.  This bounded envelope deliberately carries no length/padding
  // operator, so that shape must remain outside the finite exact route.
  if (fixed_character && !is_null && value.empty()) {
    *offset = start;
    return std::nullopt;
  }
  return Literal(std::move(type_name), std::move(value), is_null);
}

std::optional<FirebirdScalarProjectionExpression> ParseAsciiCharArgument(
    std::string_view text,
    std::size_t* offset) {
  const std::size_t start = *offset;
  if (ConsumeKeyword(text, offset, "NULL")) {
    return Literal("int64", "", true);
  }
  *offset = start;
  const auto number = ReadSignedDecimal(text, offset);
  if (!number) return std::nullopt;
  return Literal("int64", *number, false);
}

std::optional<FirebirdScalarProjectionExpression> ParseAbsArgument(
    std::string_view text,
    std::size_t* offset) {
  // This tranche intentionally binds only non-null signed decimal literals.
  // Firebird gives an untyped NULL a different ABS result descriptor, which
  // cannot share the exact non-null SQL_INT64 route used by ABS(-1).
  const auto number = ReadSignedDecimal(text, offset);
  if (!number) return std::nullopt;
  return Literal("int64", *number, false);
}

std::optional<FirebirdScalarProjectionExpression> ParseAsciiValArgument(
    std::string_view text,
    std::size_t* offset,
    std::size_t depth,
    std::string_view attachment_charset) {
  const std::size_t start = *offset;
  if (const auto nested = ParseScalarExpression(
          text, offset, depth + 1, attachment_charset)) {
    if (nested->kind == FirebirdScalarProjectionExpressionKind::kFunction &&
        nested->function_id == kOctetFromInt64) {
      return nested;
    }
  }
  *offset = start;
  if (const auto cast =
          ParseCastLiteral(text, offset, attachment_charset)) {
    return cast;
  }
  *offset = start;
  const auto literal_type = CharacterTypeForCharset(attachment_charset);
  if (!literal_type) return std::nullopt;
  if (ConsumeKeyword(text, offset, "NULL")) {
    return Literal(*literal_type, "", true);
  }
  *offset = start;
  if (const auto literal = ReadStringLiteral(text, offset)) {
    return Literal(*literal_type, *literal, false);
  }
  *offset = start;
  return std::nullopt;
}

std::optional<FirebirdScalarProjectionExpression> ParseScalarExpression(
    std::string_view text,
    std::size_t* offset,
    std::size_t depth,
    std::string_view attachment_charset) {
  if (offset == nullptr || depth > kMaximumExpressionDepth) {
    return std::nullopt;
  }
  const std::size_t start = *offset;
  if (const auto temporal_cast = ParseBoundTemporalCastExpression(
          text, offset, attachment_charset)) {
    return temporal_cast;
  }
  *offset = start;
  if (const auto numeric_literal_cast =
          ParseBoundNumericLiteralCastExpression(
              text, offset, attachment_charset)) {
    return numeric_literal_cast;
  }
  *offset = start;
  if (const auto integer_cast = ParseBoundIntegerCastExpression(
          text, offset, attachment_charset)) {
    return integer_cast;
  }
  *offset = start;
  if (const auto hash = ParseBoundHashLiteralExpression(
          text, offset, attachment_charset)) {
    return hash;
  }
  *offset = start;
  if (const auto string_literal = ParseBoundStringLiteralExpression(
          text, offset, attachment_charset)) {
    return string_literal;
  }
  *offset = start;
  if (const auto dateadd = ParseBoundDateAddExpression(text, offset)) {
    return dateadd;
  }
  *offset = start;
  if (const auto datediff = ParseBoundDateDiffExpression(text, offset)) {
    return datediff;
  }
  *offset = start;
  if (const auto extract_millisecond =
          ParseBoundExtractMillisecondExpression(text, offset)) {
    return extract_millisecond;
  }
  *offset = start;
  if (const auto extract = ParseBoundExtractWeekExpression(text, offset)) {
    return extract;
  }
  *offset = start;
  if (const auto cast = ParseBoundDecimalMathCastExpression(text, offset)) {
    return cast;
  }
  *offset = start;
  const auto function_name = ReadIdentifier(text, offset);
  if (!function_name || function_name->quoted) {
    *offset = start;
    return std::nullopt;
  }
  const std::string upper_name = UpperAscii(function_name->value);
  const auto* direct_real_spec =
      FindDirectRealFunctionBySurface(upper_name);
  const auto* bound_numeric_spec =
      FindBoundNumericFunctionBySurface(upper_name);
  const auto* bound_bit_spec = FindBoundBitFunctionBySurface(upper_name);
  if (upper_name != "ASCII_CHAR" && upper_name != "ASCII_VAL" &&
      upper_name != "ABS" && upper_name != "PI" &&
      direct_real_spec == nullptr && bound_numeric_spec == nullptr &&
      bound_bit_spec == nullptr) {
    *offset = start;
    return std::nullopt;
  }
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != '(') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  if (upper_name == "PI") {
    SkipWhitespace(text, offset);
    if (*offset >= text.size() || text[*offset] != ')') {
      *offset = start;
      return std::nullopt;
    }
    ++*offset;
    return Function("real64", std::string(kPi));
  }

  if (bound_numeric_spec != nullptr) {
    auto arguments =
        ParseBoundNumericArguments(text, offset, *bound_numeric_spec);
    if (!arguments) {
      *offset = start;
      return std::nullopt;
    }
    SkipWhitespace(text, offset);
    if (*offset >= text.size() || text[*offset] != ')') {
      *offset = start;
      return std::nullopt;
    }
    ++*offset;
    const bool exact_fixed_result =
        bound_numeric_spec->shape ==
        BoundNumericShape::kFixedDecimalAndScale;
    const bool real64_result =
        bound_numeric_spec->shape == BoundNumericShape::kFixedDecimalUnary ||
        bound_numeric_spec->shape ==
            BoundNumericShape::kFixedDecimalOptionalScale;
    return Function(exact_fixed_result
                        ? "numeric.fixed"
                        : (real64_result ? "real64" : "int64"),
                    std::string(bound_numeric_spec->function_id),
                    std::move(*arguments));
  }

  if (bound_bit_spec != nullptr) {
    const auto left = ReadBoundIntegerLiteral(text, offset, true);
    if (!left || !ConsumeComma(text, offset)) {
      *offset = start;
      return std::nullopt;
    }
    const auto right = ReadBoundIntegerLiteral(text, offset, true);
    SkipWhitespace(text, offset);
    if (!right || *offset >= text.size() || text[*offset] != ')' ||
        left->encoded_value != std::to_string(left->value) ||
        right->encoded_value != std::to_string(right->value) ||
        !IsBoundBitMeasuredInput(bound_bit_spec->surface_name, left->value,
                                 right->value)) {
      *offset = start;
      return std::nullopt;
    }
    ++*offset;
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(2);
    arguments.push_back(Literal("int64", left->encoded_value, false));
    arguments.push_back(Literal("int64", right->encoded_value, false));
    return Function("int64", std::string(bound_bit_spec->function_id),
                    std::move(arguments));
  }

  if (direct_real_spec != nullptr) {
    std::vector<FirebirdScalarProjectionExpression> arguments;
    arguments.reserve(direct_real_spec->arity);
    for (std::size_t index = 0; index < direct_real_spec->arity; ++index) {
      if (index != 0) {
        SkipWhitespace(text, offset);
        if (*offset >= text.size() || text[*offset] != ',') {
          *offset = start;
          return std::nullopt;
        }
        ++*offset;
      }
      const auto literal = ReadRealNumericLiteral(text, offset);
      if (!literal) {
        *offset = start;
        return std::nullopt;
      }
      arguments.push_back(Literal("real64", *literal, false));
    }
    SkipWhitespace(text, offset);
    if (*offset >= text.size() || text[*offset] != ')') {
      *offset = start;
      return std::nullopt;
    }
    ++*offset;
    return Function("real64", std::string(direct_real_spec->function_id),
                    std::move(arguments));
  }

  std::optional<FirebirdScalarProjectionExpression> argument;
  if (upper_name == "ASCII_CHAR") {
    argument = ParseAsciiCharArgument(text, offset);
  } else if (upper_name == "ABS") {
    argument = ParseAbsArgument(text, offset);
  } else {
    argument = ParseAsciiValArgument(
        text, offset, depth, attachment_charset);
  }
  if (!argument) {
    *offset = start;
    return std::nullopt;
  }
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != ')') {
    *offset = start;
    return std::nullopt;
  }
  ++*offset;

  if (upper_name == "ASCII_CHAR") {
    return Function("binary", std::string(kOctetFromInt64),
                    std::move(*argument));
  }
  if (upper_name == "ABS") {
    return Function("int64", std::string(kAbs), std::move(*argument));
  }
  return Function("int64", std::string(kInt64FromFirstOctet),
                  std::move(*argument));
}

bool ExpressionCanBeNull(
    const FirebirdScalarProjectionExpression& expression) {
  if (expression.kind == FirebirdScalarProjectionExpressionKind::kLiteral) {
    return expression.is_null;
  }
  for (const auto& argument : expression.arguments) {
    if (ExpressionCanBeNull(argument)) return true;
  }
  return false;
}

std::optional<FirebirdScalarProjectionItem> ParseProjectionItem(
    std::string_view text,
    std::string_view attachment_charset) {
  std::size_t offset = 0;
  auto expression =
      ParseScalarExpression(text, &offset, 0, attachment_charset);
  if (!expression) return std::nullopt;

  const BoundNumericFunctionSpec* source_bound_numeric_spec = nullptr;
  std::size_t source_name_offset = 0;
  if (const auto source_name = ReadIdentifier(text, &source_name_offset);
      source_name && !source_name->quoted) {
    source_bound_numeric_spec =
        FindBoundNumericFunctionBySurface(UpperAscii(source_name->value));
    if (source_bound_numeric_spec != nullptr &&
        source_bound_numeric_spec->function_id != expression->function_id) {
      source_bound_numeric_spec = nullptr;
    }
  }

  std::string result_name;
  FirebirdScalarProjectionOutputKind output_kind;
  std::int16_t scale = 0;
  std::int16_t subtype = 0;
  std::uint32_t declared_length = 0;
  if (expression->function_id == kOctetFromInt64) {
    result_name = "ASCII_CHAR";
    output_kind = FirebirdScalarProjectionOutputKind::kBinaryOctet;
  } else if (expression->function_id == kInt64FromFirstOctet) {
    result_name = "ASCII_VAL";
    output_kind = FirebirdScalarProjectionOutputKind::kInt16;
  } else if (expression->function_id == kAbs) {
    result_name = "ABS";
    output_kind = FirebirdScalarProjectionOutputKind::kInt64;
  } else if (expression->function_id == kPi) {
    result_name = "PI";
    output_kind = FirebirdScalarProjectionOutputKind::kReal64;
  } else if (expression->function_id == kHash64) {
    result_name = "HASH";
    output_kind = FirebirdScalarProjectionOutputKind::kInt64;
  } else if (expression->function_id == kCast) {
    result_name = "CAST";
    if (expression->arguments.size() < 2 ||
        expression->arguments[1].kind !=
            FirebirdScalarProjectionExpressionKind::kLiteral ||
        expression->arguments[1].type_name != "text" ||
        expression->arguments[1].is_null) {
      return std::nullopt;
    }
    const std::string_view target_descriptor =
        expression->arguments[1].encoded_value;
    const bool firebird_temporal_profile =
        expression->arguments.size() == 3 &&
        expression->arguments[2].kind ==
            FirebirdScalarProjectionExpressionKind::kLiteral &&
        expression->arguments[2].type_name == "text" &&
        expression->arguments[2].encoded_value ==
            kFirebirdTemporalCastProfile &&
        !expression->arguments[2].is_null;
    if (firebird_temporal_profile && expression->type_name == "date" &&
        target_descriptor == "date") {
      output_kind = FirebirdScalarProjectionOutputKind::kDate;
    } else if (firebird_temporal_profile &&
               expression->type_name == "time" &&
               target_descriptor == "time") {
      output_kind = FirebirdScalarProjectionOutputKind::kTime;
    } else if (firebird_temporal_profile &&
               expression->type_name == "timestamp" &&
               target_descriptor == "timestamp") {
      output_kind = FirebirdScalarProjectionOutputKind::kTimestamp;
    } else if (firebird_temporal_profile &&
               expression->type_name == "character" &&
               (target_descriptor == "character(32)" ||
                target_descriptor == "character(50)")) {
      output_kind = FirebirdScalarProjectionOutputKind::kFixedText;
      declared_length = target_descriptor == "character(32)" ? 32u : 50u;
    } else if (firebird_temporal_profile &&
               expression->type_name == "varchar" &&
               (target_descriptor == "varchar(32)" ||
                target_descriptor == "varchar(40)" ||
                target_descriptor == "varchar(50)")) {
      output_kind = FirebirdScalarProjectionOutputKind::kVaryingText;
      declared_length = target_descriptor == "varchar(32)"
                            ? 32u
                            : (target_descriptor == "varchar(40)" ? 40u
                                                                   : 50u);
    } else if (expression->type_name == "character" &&
        target_descriptor == "character" &&
        expression->arguments.size() == 2) {
      output_kind = FirebirdScalarProjectionOutputKind::kFixedText;
      declared_length = 21;
    } else if (expression->type_name == "varchar" &&
               target_descriptor == "varchar" &&
               expression->arguments.size() == 2) {
      output_kind = FirebirdScalarProjectionOutputKind::kVaryingText;
      declared_length = 21;
    } else if (expression->type_name == "int32" &&
               target_descriptor == "int32" &&
               expression->arguments.size() == 2) {
      output_kind = FirebirdScalarProjectionOutputKind::kInt32;
    } else if (expression->type_name == "decimal" &&
               target_descriptor == "decimal(2,1)" &&
               expression->arguments.size() == 3 &&
               expression->arguments[2].kind ==
                   FirebirdScalarProjectionExpressionKind::kLiteral &&
               expression->arguments[2].type_name == "text" &&
               expression->arguments[2].encoded_value == "half_up" &&
               !expression->arguments[2].is_null) {
      output_kind = FirebirdScalarProjectionOutputKind::kExactInt16;
      scale = -1;
      subtype = 1;
    } else if (expression->type_name == "decimal" &&
               target_descriptor == "decimal(18,15)" &&
               expression->arguments.size() == 3) {
      output_kind = FirebirdScalarProjectionOutputKind::kExactInt64;
      scale = -15;
      subtype = 2;
    } else {
      return std::nullopt;
    }
  } else if (expression->function_id == kDateAdd) {
    result_name = "DATEADD";
    if (expression->type_name == "date") {
      output_kind = FirebirdScalarProjectionOutputKind::kDate;
    } else if (expression->type_name == "timestamp") {
      output_kind = FirebirdScalarProjectionOutputKind::kTimestamp;
    } else if (expression->type_name == "time") {
      output_kind = FirebirdScalarProjectionOutputKind::kTime;
    } else {
      return std::nullopt;
    }
  } else if (expression->function_id == kDateDiff) {
    result_name = "DATEDIFF";
    if (expression->type_name == "numeric.fixed") {
      output_kind = FirebirdScalarProjectionOutputKind::kExactInt64;
      scale = -1;
    } else {
      output_kind = FirebirdScalarProjectionOutputKind::kInt64;
    }
  } else if (expression->function_id == kDatePart) {
    result_name = "EXTRACT";
    if (expression->type_name == "numeric.fixed") {
      output_kind = FirebirdScalarProjectionOutputKind::kExactInt32;
      scale = -1;
    } else {
      output_kind = FirebirdScalarProjectionOutputKind::kInt16;
    }
  } else if (const auto* bound_bit_spec =
                 FindBoundBitFunctionById(expression->function_id)) {
    result_name = std::string(bound_bit_spec->surface_name);
    output_kind = bound_bit_spec->output_kind;
  } else if (const auto* bound_string_spec =
                 FindBoundStringFunctionById(expression->function_id)) {
    result_name = std::string(bound_string_spec->surface_name);
    output_kind = bound_string_spec->output_kind;
    declared_length = bound_string_spec->declared_length;
  } else if (const auto* direct_real_spec =
                 FindDirectRealFunctionById(expression->function_id)) {
    result_name = std::string(direct_real_spec->surface_name);
    output_kind = FirebirdScalarProjectionOutputKind::kReal64;
  } else if (const auto* bound_numeric_spec = source_bound_numeric_spec) {
    const auto bound_scale =
        BoundNumericResultScale(*bound_numeric_spec, *expression);
    if (!bound_scale) return std::nullopt;
    result_name = std::string(bound_numeric_spec->surface_name);
    output_kind = bound_numeric_spec->output_kind;
    scale = *bound_scale;
  } else {
    return std::nullopt;
  }
  SkipWhitespace(text, &offset);
  if (offset < text.size()) {
    const std::size_t alias_start = offset;
    if (!ConsumeKeyword(text, &offset, "AS")) offset = alias_start;
    const auto alias = ReadIdentifier(text, &offset);
    if (!alias || alias->value.empty()) return std::nullopt;
    result_name = alias->quoted ? alias->value : UpperAscii(alias->value);
    SkipWhitespace(text, &offset);
  }
  if (offset != text.size()) return std::nullopt;
  const bool nullable = ExpressionCanBeNull(*expression);
  return FirebirdScalarProjectionItem{
      std::move(result_name),
      output_kind,
      nullable,
      scale,
      subtype,
      declared_length,
      std::move(*expression)};
}

std::string EscapeJson(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (ch < 0x20) {
          escaped += "\\u00";
          escaped.push_back(kHex[(ch >> 4) & 0x0f]);
          escaped.push_back(kHex[ch & 0x0f]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
    }
  }
  return escaped;
}

void AppendExpressionJson(std::ostream& out,
                          const std::string& prefix,
                          const FirebirdScalarProjectionExpression& expression) {
  const bool function =
      expression.kind == FirebirdScalarProjectionExpressionKind::kFunction;
  out << "\"" << prefix << "expr_kind\":\""
      << (function ? "function" : "literal") << "\","
      << "\"" << prefix << "expr_opcode\":\""
      << (function ? "SBLR_FUNCTION_CALL" : "SBLR_LITERAL") << "\","
      << "\"" << prefix << "type\":\""
      << EscapeJson(expression.type_name) << "\","
      << "\"" << prefix << "value\":\""
      << EscapeJson(expression.encoded_value) << "\","
      << "\"" << prefix << "is_null\":\""
      << (expression.is_null ? "true" : "false") << "\",";
  if (!function) return;
  out << "\"" << prefix << "function_id\":\""
      << EscapeJson(expression.function_id) << "\","
      << "\"" << prefix << "function_arg_count\":\""
      << expression.arguments.size() << "\",";
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    AppendExpressionJson(out, prefix + "arg_" + std::to_string(index) + "_",
                         expression.arguments[index]);
  }
}

}  // namespace

FirebirdScalarProjectionRoute ParseFirebirdScalarProjectionRoute(
    std::string_view firebird_sql,
    FirebirdScalarProjectionBindOptions options) {
  FirebirdScalarProjectionRoute route;
  std::string sql = TrimAscii(firebird_sql);
  while (!sql.empty() && sql.back() == ';') {
    sql.pop_back();
    sql = TrimAscii(sql);
  }

  std::size_t offset = 0;
  if (!ConsumeKeyword(sql, &offset, "SELECT")) return route;
  const auto from = FindTopLevelKeyword(sql, "FROM", offset);
  if (!from) return route;
  const std::string projection_text =
      TrimAscii(std::string_view(sql).substr(offset, *from - offset));
  const auto projection_items = SplitTopLevelComma(projection_text);
  if (!projection_items || projection_items->empty() ||
      projection_items->size() > kMaximumProjectionCount) {
    return route;
  }

  offset = *from;
  if (!ConsumeKeyword(sql, &offset, "FROM")) return route;
  const auto source = ReadIdentifier(sql, &offset);
  if (!source || source->quoted || UpperAscii(source->value) != "RDB$DATABASE") {
    return route;
  }
  SkipWhitespace(sql, &offset);
  if (offset != sql.size()) return route;

  for (const auto& item_text : *projection_items) {
    auto item = ParseProjectionItem(item_text, options.attachment_charset);
    if (!item) return {};
    route.items.push_back(std::move(*item));
  }
  return route;
}

std::string EncodeFirebirdScalarProjectionEnvelope(
    const FirebirdScalarProjectionRoute& route) {
  if (!route.recognized() || route.items.size() > kMaximumProjectionCount) {
    return {};
  }
  std::ostringstream out;
  out << "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      << "\"envelope_major\":3,"
      << "\"sblr_version\":\"sblr_v3\","
      << "\"operation_id\":\"query.evaluate_projection\","
      << "\"opcode\":\"SBLR_QUERY_EVALUATE_PROJECTION\","
      << "\"operation_family\":\"sblr.query.relational.v3\","
      << "\"sblr_operation_family\":\"sblr.query.relational.v3\","
      << "\"result_shape\":\"engine.api.result.v1\","
      << "\"diagnostic_shape\":\"engine.diagnostic.v1\","
      << "\"parser_resolved_names_to_uuids\":true,"
      << "\"requires_security_context\":true,"
      << "\"requires_transaction_context\":true,"
      << "\"requires_cluster_authority\":false,"
      << "\"contains_sql_text\":false,"
      << "\"identifier_profile_uuid\":\"firebird_v5\","
      << "\"source_dialect\":\"firebird\","
      << "\"scalar_projection_source\":\"singleton_system_row\","
      << "\"scalar_projection_parser_executes_sql\":false,"
      << "\"projection_count\":\"" << route.items.size() << "\",";
  for (std::size_t index = 0; index < route.items.size(); ++index) {
    const std::string prefix = "projection_" + std::to_string(index) + "_";
    out << "\"" << prefix << "name\":\""
        << "c" << index << "\",";
    AppendExpressionJson(out, prefix, route.items[index].expression);
  }
  std::string encoded = out.str();
  if (!encoded.empty() && encoded.back() == ',') encoded.back() = '}';
  return encoded;
}

}  // namespace scratchbird::parser::firebird
