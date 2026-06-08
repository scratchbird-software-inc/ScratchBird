// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  }
  return OperatorDiagnosticFailure(operator_id,
                                   context,
                                   "SBLR.DESCRIPTOR_MISMATCH",
                                   "temporal arithmetic requires temporal-plus-interval, interval-plus-temporal, interval-plus-interval, or temporal-minus-temporal operands");
}

int CompareNumericPayloads(const SblrValue& left, const SblrValue& right) {
  if (left.has_int64_value && right.has_int64_value) {
    return left.int64_value < right.int64_value ? -1 : (left.int64_value > right.int64_value ? 1 : 0);
  }
  if (left.has_uint64_value && right.has_uint64_value) {
    return left.uint64_value < right.uint64_value ? -1 : (left.uint64_value > right.uint64_value ? 1 : 0);
  }
  if (left.has_int64_value && right.has_uint64_value) {
    if (left.int64_value < 0) return -1;
    const auto left_unsigned = static_cast<std::uint64_t>(left.int64_value);
    return left_unsigned < right.uint64_value ? -1 : (left_unsigned > right.uint64_value ? 1 : 0);
  }
  if (left.has_uint64_value && right.has_int64_value) {
    if (right.int64_value < 0) return 1;
    const auto right_unsigned = static_cast<std::uint64_t>(right.int64_value);
    return left.uint64_value < right_unsigned ? -1 : (left.uint64_value > right_unsigned ? 1 : 0);
  }
  long double left_value = 0;
  long double right_value = 0;
  if (!TryLongDouble(left, &left_value) || !TryLongDouble(right, &right_value)) return 0;
  return left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
}

SblrResult OperatorFailure(std::string_view operator_id,
                           const SblrExecutionContext& context,
                           std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_OPERATOR_INVALID_INPUT", context, std::move(detail));
  diagnostic.fields.push_back({"operator_id", std::string(operator_id)});
  return MakeSblrFailure(SblrStatusCode::execution_failed, std::string(operator_id), std::move(diagnostic));
}

bool AddWouldOverflow(std::int64_t left, std::int64_t right) {
  return (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
         (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right);
}

bool SubtractWouldOverflow(std::int64_t left, std::int64_t right) {
  return (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
         (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right);
}

bool MultiplyWouldOverflow(std::int64_t left, std::int64_t right) {
  if (left == 0 || right == 0) return false;
  if (left == -1) return right == std::numeric_limits<std::int64_t>::min();
  if (right == -1) return left == std::numeric_limits<std::int64_t>::min();
  if (left > 0) {
    if (right > 0) return left > std::numeric_limits<std::int64_t>::max() / right;
    return right < std::numeric_limits<std::int64_t>::min() / left;
  }
  if (right > 0) return left < std::numeric_limits<std::int64_t>::min() / right;
  return left != 0 && right < std::numeric_limits<std::int64_t>::max() / left;
}

bool IsTextualPayload(const SblrValue& value) {
  return value.payload_kind == SblrValuePayloadKind::text ||
         value.payload_kind == SblrValuePayloadKind::uuid_text ||
         value.payload_kind == SblrValuePayloadKind::temporal_text ||
         value.payload_kind == SblrValuePayloadKind::descriptor_payload ||
         value.descriptor_id == "text" ||
         value.descriptor_id == "varchar" ||
         value.descriptor_id == "char" ||
         value.descriptor_id == "string";
}

std::string TextPayload(const SblrValue& value) {
  if (!value.text_value.empty()) return value.text_value;
  return value.encoded_value;
}

char FoldAscii(char ch, bool case_insensitive) {
  if (!case_insensitive) return ch;
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

struct LikeMatchResult {
  bool ok = false;
  bool matched = false;
};

LikeMatchResult LikeMatch(std::string_view value, std::string_view pattern, bool case_insensitive) {
  LikeMatchResult out;
  const std::size_t value_size = value.size();
  const std::size_t pattern_size = pattern.size();
  std::size_t value_index = 0;
  std::size_t pattern_index = 0;
  std::size_t star_pattern = std::string_view::npos;
  std::size_t star_value = std::string_view::npos;
  while (value_index < value_size) {
    if (pattern_index < pattern_size && pattern[pattern_index] == '\\') {
      if (pattern_index + 1 >= pattern_size) return out;
      const char literal = FoldAscii(pattern[pattern_index + 1], case_insensitive);
      if (FoldAscii(value[value_index], case_insensitive) == literal) {
        pattern_index += 2;
        ++value_index;
        continue;
      }
    } else if (pattern_index < pattern_size && pattern[pattern_index] == '_') {
      ++pattern_index;
      ++value_index;
      continue;
    } else if (pattern_index < pattern_size && pattern[pattern_index] == '%') {
      star_pattern = pattern_index++;
      star_value = value_index;
      continue;
    } else if (pattern_index < pattern_size &&
               FoldAscii(value[value_index], case_insensitive) ==
                   FoldAscii(pattern[pattern_index], case_insensitive)) {
      ++pattern_index;
      ++value_index;
      continue;
    }
    if (star_pattern != std::string_view::npos) {
      pattern_index = star_pattern + 1;
      value_index = ++star_value;
      continue;
    }
    out.ok = true;
    return out;
  }
  while (pattern_index < pattern_size && pattern[pattern_index] == '%') ++pattern_index;
  if (pattern_index < pattern_size && pattern[pattern_index] == '\\' && pattern_index + 1 >= pattern_size) return out;
  out.ok = true;
  out.matched = pattern_index == pattern_size;
  return out;
}

bool TextMetadataCompatible(const SblrValue& left, const SblrValue& right) {
  if (!left.charset_name.empty() && !right.charset_name.empty() && left.charset_name != right.charset_name) return false;
  if (!left.collation_name.empty() && !right.collation_name.empty() && left.collation_name != right.collation_name) return false;
  return true;
}

bool StringOperatorIs(std::string_view operator_id,
                      std::string_view normalized,
                      std::string_view legacy,
                      std::string_view legacy_alt = {}) {
  return operator_id == normalized || operator_id == legacy || (!legacy_alt.empty() && operator_id == legacy_alt);
}

struct JsonValue {
  enum class Kind {
    null_value,
    boolean,
    number,
    string,
    array,
    object,
  };

  Kind kind = Kind::null_value;
  bool bool_value = false;
  std::string scalar;
  std::vector<JsonValue> array_values;
  std::vector<std::pair<std::string, JsonValue>> object_values;
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  bool Parse(JsonValue* out) {
    SkipWhitespace();
    if (!ParseValue(out)) return false;
    SkipWhitespace();
    return pos_ == text_.size();
  }

 private:
  void SkipWhitespace() {
    while (pos_ < text_.size()) {
      const char ch = text_[pos_];
      if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
      ++pos_;
    }
  }

  bool Consume(char expected) {
    SkipWhitespace();
    if (pos_ >= text_.size() || text_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ParseLiteral(std::string_view literal) {
    if (text_.substr(pos_, literal.size()) != literal) return false;
    pos_ += literal.size();
    return true;
  }

  bool ParseStringLiteral(std::string* out) {
    SkipWhitespace();
    if (pos_ >= text_.size() || text_[pos_] != '"') return false;
    ++pos_;
    std::string value;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_++];
      if (ch == '"') {
        *out = std::move(value);
        return true;
      }
      if (static_cast<unsigned char>(ch) < 0x20) return false;
      if (ch == '\\') {
        if (pos_ >= text_.size()) return false;
        const char escaped = text_[pos_++];
        switch (escaped) {
          case '"': value.push_back('"'); break;
          case '\\': value.push_back('\\'); break;
          case '/': value.push_back('/'); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          case 'u':
            if (pos_ + 4 > text_.size()) return false;
            for (int i = 0; i < 4; ++i) {
              const char hex = text_[pos_ + i];
              if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F'))) return false;
            }
            value += "\\u";
            value.append(text_.substr(pos_, 4));
            pos_ += 4;
            break;
          default:
            return false;
        }
      } else {
        value.push_back(ch);
      }
    }
    return false;
  }

  bool ParseNumber(JsonValue* out) {
    SkipWhitespace();
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return false;
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    } else {
      return false;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      const std::size_t frac_start = pos_;
