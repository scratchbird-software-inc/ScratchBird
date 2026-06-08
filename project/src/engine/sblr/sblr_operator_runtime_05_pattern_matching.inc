// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
      for (char ch : path.substr(start, pos - start)) index = index * 10 + static_cast<std::uint64_t>(ch - '0');
      ++pos;
      if (current->kind != JsonValue::Kind::array || index >= current->array_values.size()) return nullptr;
      current = &current->array_values[static_cast<std::size_t>(index)];
      continue;
    }
    const std::size_t start = pos;
    while (pos < path.size() && path[pos] != '.' && path[pos] != '[') ++pos;
    current = JsonObjectFind(*current, path.substr(start, pos - start));
    if (current == nullptr) return nullptr;
  }
  return current;
}

bool ParseJsonOperand(const SblrValue& value, JsonValue* out) {
  if (value.is_null) return false;
  const std::string text = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (text.empty()) return false;
  return JsonParser(text).Parse(out);
}

SblrValue JsonDocumentValue(const JsonValue& value) {
  SblrValue out;
  out.descriptor_id = "json_document";
  out.payload_kind = SblrValuePayloadKind::descriptor_payload;
  out.encoded_value = JsonCanonical(value);
  out.text_value = out.encoded_value;
  out.is_null = false;
  return out;
}

bool DocumentOperatorIs(std::string_view operator_id,
                        std::string_view normalized,
                        std::string_view legacy,
                        std::string_view legacy_alt = {}) {
  return operator_id == normalized || operator_id == legacy || (!legacy_alt.empty() && operator_id == legacy_alt);
}

int HexDigitValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

std::string HexEncodeLower(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() * 2);
  for (unsigned char ch : text) {
    out.push_back(kHex[ch >> 4]);
    out.push_back(kHex[ch & 0x0f]);
  }
  return out;
}

bool HexDecodeStrict(std::string_view hex, std::string* out) {
  if (hex.size() % 2 != 0) return false;
  std::string decoded;
  decoded.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int high = HexDigitValue(hex[i]);
    const int low = HexDigitValue(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  *out = std::move(decoded);
  return true;
}

std::vector<std::string> SplitComma(std::string_view text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    if (comma == std::string_view::npos) {
      out.emplace_back(text.substr(start));
      break;
    }
    out.emplace_back(text.substr(start, comma - start));
    start = comma + 1;
  }
  return out;
}

struct CollectionValue {
  enum class Kind {
    array,
    set,
  };

  Kind kind = Kind::array;
  std::vector<JsonValue> array_values;
  std::vector<std::string> set_values;
};

bool ParseSbSet(std::string_view encoded, std::vector<std::string>* values) {
  if (encoded.rfind("SBSET1;", 0) != 0) return false;
  const auto marker = encoded.find(";items=");
  if (marker == std::string_view::npos) return false;
  const std::string_view item_list = encoded.substr(marker + 7);
  if (item_list.empty()) return true;
  for (const auto& hex : SplitComma(item_list)) {
    std::string decoded;
    if (!HexDecodeStrict(hex, &decoded)) return false;
    values->push_back(std::move(decoded));
  }
  return true;
}

std::string EncodeSbSet(const std::vector<std::string>& values) {
  std::vector<std::string> unique_values = values;
  std::sort(unique_values.begin(), unique_values.end());
  unique_values.erase(std::unique(unique_values.begin(), unique_values.end()), unique_values.end());
  std::ostringstream out;
  out << "SBSET1;element=character;ordered=0;nulls=1;duplicates=0;items=";
  for (std::size_t i = 0; i < unique_values.size(); ++i) {
    if (i != 0) out << ",";
    out << HexEncodeLower(unique_values[i]);
  }
  return out.str();
}

bool ParseCollectionOperand(const SblrValue& value, CollectionValue* out) {
  if (value.is_null) return false;
  const std::string encoded = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (value.descriptor_id == "set_value" || encoded.rfind("SBSET1;", 0) == 0) {
    out->kind = CollectionValue::Kind::set;
    return ParseSbSet(encoded, &out->set_values);
  }
  JsonValue json;
  if (!JsonParser(encoded).Parse(&json) || json.kind != JsonValue::Kind::array) return false;
  out->kind = CollectionValue::Kind::array;
  out->array_values = std::move(json.array_values);
  return true;
}

SblrValue CollectionArrayValue(std::vector<JsonValue> values) {
  JsonValue array;
  array.kind = JsonValue::Kind::array;
  array.array_values = std::move(values);
  SblrValue out;
  out.descriptor_id = "array";
  out.payload_kind = SblrValuePayloadKind::descriptor_payload;
  out.encoded_value = JsonCanonical(array);
  out.text_value = out.encoded_value;
  out.is_null = false;
  return out;
}

SblrValue CollectionSetValue(std::vector<std::string> values) {
  SblrValue out;
  out.descriptor_id = "set_value";
  out.payload_kind = SblrValuePayloadKind::descriptor_payload;
  out.encoded_value = EncodeSbSet(values);
  out.text_value = out.encoded_value;
  out.is_null = false;
  return out;
}

bool CollectionContains(const CollectionValue& collection, const JsonValue& candidate) {
  if (collection.kind == CollectionValue::Kind::array) {
    for (const auto& value : collection.array_values) {
      if (JsonEquals(value, candidate)) return true;
    }
    return false;
  }
  const std::string canonical = JsonCanonical(candidate);
  return std::find(collection.set_values.begin(), collection.set_values.end(), canonical) != collection.set_values.end();
}

bool CollectionContainsText(const CollectionValue& collection, std::string_view candidate) {
  if (collection.kind == CollectionValue::Kind::set) {
    return std::find(collection.set_values.begin(), collection.set_values.end(), candidate) != collection.set_values.end();
  }
  JsonValue json_candidate;
  json_candidate.kind = JsonValue::Kind::string;
  json_candidate.scalar = std::string(candidate);
  return CollectionContains(collection, json_candidate);
}

std::uint64_t CollectionCardinality(const CollectionValue& collection) {
  return collection.kind == CollectionValue::Kind::array ? collection.array_values.size() : collection.set_values.size();
}

bool CollectionOperatorIs(std::string_view operator_id,
                          std::string_view normalized,
                          std::string_view legacy,
                          std::string_view legacy_alt = {}) {
  return operator_id == normalized || operator_id == legacy || (!legacy_alt.empty() && operator_id == legacy_alt);
}

struct VectorValue {
  std::vector<long double> elements;
};

bool TryLongDoubleText(std::string_view text, long double* out) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  if (text.empty()) return false;
  try {
    std::size_t parsed = 0;
    const long double value = std::stold(std::string(text), &parsed);
    if (parsed != text.size() || !std::isfinite(static_cast<double>(value))) return false;
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool JsonNumberToLongDouble(const JsonValue& value, long double* out) {
  if (value.kind != JsonValue::Kind::number) return false;
  return TryLongDoubleText(value.scalar, out);
}

bool ParseVectorOperand(const SblrValue& value, VectorValue* out) {
  if (value.is_null) return false;
  if (!value.binary_value.empty()) return false;
  const std::string encoded = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (encoded.empty()) return false;
  JsonValue json;
  if (JsonParser(encoded).Parse(&json)) {
    if (json.kind != JsonValue::Kind::array) return false;
    for (const auto& element : json.array_values) {
      long double number = 0;
      if (!JsonNumberToLongDouble(element, &number)) return false;
      out->elements.push_back(number);
    }
    return !out->elements.empty();
  }
  for (const auto& part : SplitComma(encoded)) {
    long double number = 0;
    if (!TryLongDoubleText(part, &number)) return false;
    out->elements.push_back(number);
  }
  return !out->elements.empty();
}

SblrValue VectorScalarValue(long double value) {
  SblrValue out;
  out.descriptor_id = "real64";
  out.payload_kind = SblrValuePayloadKind::real64;
  out.has_real64_value = true;
  out.real64_value = static_cast<double>(value);
  std::ostringstream stream;
  stream << std::setprecision(17) << out.real64_value;
  out.encoded_value = stream.str();
  out.text_value = out.encoded_value;
  out.is_null = false;
  return out;
