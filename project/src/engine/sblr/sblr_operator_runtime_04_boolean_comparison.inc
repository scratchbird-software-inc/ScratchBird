// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (frac_start == pos_) return false;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      const std::size_t exp_start = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (exp_start == pos_) return false;
    }
    out->kind = JsonValue::Kind::number;
    out->scalar = std::string(text_.substr(start, pos_ - start));
    return true;
  }

  bool ParseArray(JsonValue* out) {
    if (!Consume('[')) return false;
    out->kind = JsonValue::Kind::array;
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      JsonValue element;
      if (!ParseValue(&element)) return false;
      out->array_values.push_back(std::move(element));
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  bool ParseObject(JsonValue* out) {
    if (!Consume('{')) return false;
    out->kind = JsonValue::Kind::object;
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      std::string key;
      if (!ParseStringLiteral(&key)) return false;
      if (!Consume(':')) return false;
      JsonValue value;
      if (!ParseValue(&value)) return false;
      out->object_values.push_back({std::move(key), std::move(value)});
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  bool ParseValue(JsonValue* out) {
    SkipWhitespace();
    if (pos_ >= text_.size()) return false;
    switch (text_[pos_]) {
      case 'n':
        if (!ParseLiteral("null")) return false;
        out->kind = JsonValue::Kind::null_value;
        return true;
      case 't':
        if (!ParseLiteral("true")) return false;
        out->kind = JsonValue::Kind::boolean;
        out->bool_value = true;
        return true;
      case 'f':
        if (!ParseLiteral("false")) return false;
        out->kind = JsonValue::Kind::boolean;
        out->bool_value = false;
        return true;
      case '"':
        out->kind = JsonValue::Kind::string;
        return ParseStringLiteral(&out->scalar);
      case '[':
        return ParseArray(out);
      case '{':
        return ParseObject(out);
      default:
        return ParseNumber(out);
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

std::string JsonEscape(std::string_view text) {
  std::string out;
  out.push_back('"');
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          std::ostringstream hex;
          hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(ch));
          out += hex.str();
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string JsonCanonical(const JsonValue& value) {
  switch (value.kind) {
    case JsonValue::Kind::null_value:
      return "null";
    case JsonValue::Kind::boolean:
      return value.bool_value ? "true" : "false";
    case JsonValue::Kind::number:
      return value.scalar;
    case JsonValue::Kind::string:
      return JsonEscape(value.scalar);
    case JsonValue::Kind::array: {
      std::string out = "[";
      for (std::size_t i = 0; i < value.array_values.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += JsonCanonical(value.array_values[i]);
      }
      out.push_back(']');
      return out;
    }
    case JsonValue::Kind::object: {
      std::string out = "{";
      for (std::size_t i = 0; i < value.object_values.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += JsonEscape(value.object_values[i].first);
        out.push_back(':');
        out += JsonCanonical(value.object_values[i].second);
      }
      out.push_back('}');
      return out;
    }
  }
  return "null";
}

const JsonValue* JsonObjectFind(const JsonValue& object, std::string_view key) {
  if (object.kind != JsonValue::Kind::object) return nullptr;
  for (const auto& entry : object.object_values) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

JsonValue* JsonObjectFindMutable(JsonValue& object, std::string_view key) {
  if (object.kind != JsonValue::Kind::object) return nullptr;
  for (auto& entry : object.object_values) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

bool JsonEquals(const JsonValue& left, const JsonValue& right) {
  if (left.kind != right.kind) return false;
  switch (left.kind) {
    case JsonValue::Kind::null_value:
      return true;
    case JsonValue::Kind::boolean:
      return left.bool_value == right.bool_value;
    case JsonValue::Kind::number:
    case JsonValue::Kind::string:
      return left.scalar == right.scalar;
    case JsonValue::Kind::array:
      if (left.array_values.size() != right.array_values.size()) return false;
      for (std::size_t i = 0; i < left.array_values.size(); ++i) {
        if (!JsonEquals(left.array_values[i], right.array_values[i])) return false;
      }
      return true;
    case JsonValue::Kind::object:
      for (const auto& right_entry : right.object_values) {
        const auto* left_value = JsonObjectFind(left, right_entry.first);
        if (left_value == nullptr || !JsonEquals(*left_value, right_entry.second)) return false;
      }
      return left.object_values.size() == right.object_values.size();
  }
  return false;
}

bool JsonContains(const JsonValue& container, const JsonValue& candidate) {
  if (candidate.kind == JsonValue::Kind::object) {
    if (container.kind != JsonValue::Kind::object) return false;
    for (const auto& entry : candidate.object_values) {
      const auto* found = JsonObjectFind(container, entry.first);
      if (found == nullptr || !JsonContains(*found, entry.second)) return false;
    }
    return true;
  }
  if (candidate.kind == JsonValue::Kind::array) {
    if (container.kind != JsonValue::Kind::array) return false;
    for (const auto& candidate_value : candidate.array_values) {
      bool matched = false;
      for (const auto& container_value : container.array_values) {
        if (JsonContains(container_value, candidate_value)) {
          matched = true;
          break;
        }
      }
      if (!matched) return false;
    }
    return true;
  }
  return JsonEquals(container, candidate);
}

JsonValue JsonMerge(JsonValue left, const JsonValue& right) {
  if (left.kind != JsonValue::Kind::object || right.kind != JsonValue::Kind::object) return right;
  for (const auto& entry : right.object_values) {
    auto* existing = JsonObjectFindMutable(left, entry.first);
    if (existing == nullptr) {
      left.object_values.push_back(entry);
    } else {
      *existing = JsonMerge(*existing, entry.second);
    }
  }
  return left;
}

const JsonValue* JsonPathLookup(const JsonValue& root, std::string_view path) {
  const JsonValue* current = &root;
  std::size_t pos = 0;
  if (path.empty() || path == "$") return current;
  if (path[pos] == '$') ++pos;
  while (pos < path.size()) {
    if (path[pos] == '.') {
      ++pos;
      const std::size_t start = pos;
      while (pos < path.size() && path[pos] != '.' && path[pos] != '[') ++pos;
      if (start == pos) return nullptr;
      current = JsonObjectFind(*current, path.substr(start, pos - start));
      if (current == nullptr) return nullptr;
      continue;
    }
    if (path[pos] == '[') {
      ++pos;
      const std::size_t start = pos;
      while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') ++pos;
      if (start == pos || pos >= path.size() || path[pos] != ']') return nullptr;
      std::uint64_t index = 0;
