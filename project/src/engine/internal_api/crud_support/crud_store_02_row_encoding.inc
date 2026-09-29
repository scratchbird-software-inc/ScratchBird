// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by crud_store.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
                                           }),
                            state->row_versions.end());
  state->indexes.erase(std::remove_if(state->indexes.begin(),
                                      state->indexes.end(),
                                      [&purged_tables](const CrudIndexRecord& index) {
                                        return purged_tables.count(index.table_uuid) != 0;
                                      }),
                       state->indexes.end());
  state->index_entries.erase(std::remove_if(state->index_entries.begin(),
                                            state->index_entries.end(),
                                            [&purged_tables](const CrudIndexEntryRecord& entry) {
                                              return purged_tables.count(entry.table_uuid) != 0;
                                            }),
                             state->index_entries.end());
}

std::string BuildCompositeKey(const CrudValueFields& values,
                              const std::vector<std::string>& columns) {
  if (columns.empty()) return {};
  std::vector<CrudStoredValue> components;
  components.reserve(columns.size());
  for (const auto& column : columns) {
    components.push_back(CrudFieldValue(values, column));
  }
  return EncodeStoredLogicalKey(components);
}

std::vector<std::string> TokenizeCrudText(const std::string& value) {
  std::set<std::string> unique_tokens;
  std::string token;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '_') {
      token.push_back(static_cast<char>(std::tolower(c)));
    } else if (!token.empty()) {
      unique_tokens.insert(token);
      token.clear();
    }
  }
  if (!token.empty()) { unique_tokens.insert(token); }
  return {unique_tokens.begin(), unique_tokens.end()};
}

std::vector<std::string> TokenizeCrudNgrams(const std::string& value) {
  std::string normalized;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '_') {
      normalized.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  if (normalized.empty()) {
    return {};
  }
  if (normalized.size() <= 3) {
    return {normalized};
  }
  std::set<std::string> unique_grams;
  for (std::size_t index = 0; index + 3 <= normalized.size(); ++index) {
    unique_grams.insert(normalized.substr(index, 3));
  }
  return {unique_grams.begin(), unique_grams.end()};
}

std::vector<std::string> TokenizeCrudDocumentPath(const std::string& value) {
  std::set<std::string> keys;
  std::string token;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '_' || c == '$') {
      token.push_back(static_cast<char>(std::tolower(c)));
    } else if (!token.empty()) {
      keys.insert(token);
      token.clear();
    }
  }
  if (!token.empty()) { keys.insert(token); }
  if (keys.empty()) { return TokenizeCrudText(value); }
  return {keys.begin(), keys.end()};
}

bool CrudTokenIndexFamily(const std::string& family) {
  return family == kCrudIndexFamilyFullText ||
         family == kCrudIndexFamilyGin ||
         family == kCrudIndexFamilyInverted ||
         family == kCrudIndexFamilySparseWand;
}

bool CrudNgramIndexFamily(const std::string& family) {
  return family == kCrudIndexFamilyNgram;
}

bool TryParseDouble(const std::string& value, double* out) {
  try {
    std::size_t pos = 0;
    const double parsed = std::stod(value, &pos);
    if (pos != value.size() || !std::isfinite(parsed)) { return false; }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

int CompareCrudScalar(const std::string& left, const std::string& right) {
  double left_number = 0.0;
  double right_number = 0.0;
  if (TryParseDouble(left, &left_number) && TryParseDouble(right, &right_number)) {
    if (left_number < right_number) { return -1; }
    if (left_number > right_number) { return 1; }
    return 0;
  }
  if (left < right) { return -1; }
  if (left > right) { return 1; }
  return 0;
}

struct CrudBBox {
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
  bool valid = false;
};

CrudBBox ParseCrudBBox(const std::string& value) {
  const auto parts = Split(value, ',');
  CrudBBox box;
  if (parts.size() != 4) { return box; }
  if (!TryParseDouble(parts[0], &box.min_x) || !TryParseDouble(parts[1], &box.min_y) ||
      !TryParseDouble(parts[2], &box.max_x) || !TryParseDouble(parts[3], &box.max_y)) {
    return box;
  }
  if (box.min_x > box.max_x) { std::swap(box.min_x, box.max_x); }
  if (box.min_y > box.max_y) { std::swap(box.min_y, box.max_y); }
  box.valid = true;
  return box;
}

bool BBoxIntersects(const CrudBBox& left, const CrudBBox& right) {
  return left.valid && right.valid && left.min_x <= right.max_x && left.max_x >= right.min_x &&
         left.min_y <= right.max_y && left.max_y >= right.min_y;
}

bool BBoxContains(const CrudBBox& container, const CrudBBox& contained) {
  return container.valid && contained.valid && container.min_x <= contained.min_x && container.min_y <= contained.min_y &&
         container.max_x >= contained.max_x && container.max_y >= contained.max_y;
}

std::vector<double> ParseCrudVector(const std::string& value) {
  std::vector<double> vector;
  for (const auto& part : Split(value, ',')) {
    double parsed = 0.0;
    if (!TryParseDouble(part, &parsed)) { return {}; }
    vector.push_back(parsed);
  }
  return vector;
}

double CrudVectorDistance(const std::string& left_value, const std::string& right_value) {
  const auto left = ParseCrudVector(left_value);
  const auto right = ParseCrudVector(right_value);
  if (left.empty() || left.size() != right.size()) { return std::numeric_limits<double>::infinity(); }
  double sum = 0.0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const double delta = left[i] - right[i];
    sum += delta * delta;
  }
  return std::sqrt(sum);
}

std::string ExtractFunctionArgument(const std::string& expression, const std::string& function_name) {
  const std::string prefix = function_name + "(";
  if (!StartsWith(expression, prefix) || expression.size() <= prefix.size() || expression.back() != ')') { return {}; }
  return expression.substr(prefix.size(), expression.size() - prefix.size() - 1);
}

CrudStoredValue ExpressionKeyForValues(const std::string& expression,
                                      const CrudValueFields& values) {
  const auto unary = [&](std::string column, std::string_view operation) {
    auto value = CrudFieldValue(values, column);
    if (!value.isPresent()) return value;
    if (operation == "length") return CrudStoredValue(std::to_string(value.bytes.size()));
    for (char& c : value.bytes) {
      const auto byte = static_cast<unsigned char>(c);
      c = static_cast<char>(operation == "lower" ? std::tolower(byte) : std::toupper(byte));
    }
    return value;
  };
  if (StartsWith(expression, "lower:")) return unary(expression.substr(6), "lower");
  if (StartsWith(expression, "upper:")) return unary(expression.substr(6), "upper");
  if (StartsWith(expression, "length:")) return unary(expression.substr(7), "length");
  if (StartsWith(expression, "identity:")) return CrudFieldValue(values, expression.substr(9));
  if (StartsWith(expression, "desc:")) return CrudFieldValue(values, expression.substr(5));
  if (StartsWith(expression, "sum:")) {
    const auto rest = expression.substr(4);
    const auto pos = rest.find(':');
    if (pos == std::string::npos) throw std::invalid_argument("invalid sum key expression");
    const auto left = CrudFieldValue(values, rest.substr(0, pos));
    const auto right = CrudFieldValue(values, rest.substr(pos + 1));
    if (left.isSqlNull() || right.isSqlNull()) return CrudStoredValue::SqlNull();
    if (!left.isPresent() || !right.isPresent())
      throw std::invalid_argument("unresolved sum key value state");
    std::size_t left_end = 0, right_end = 0;
    const auto lhs = std::stoll(left.bytes, &left_end);
    const auto rhs = std::stoll(right.bytes, &right_end);
    if (left_end != left.bytes.size() || right_end != right.bytes.size())
      throw std::invalid_argument("invalid sum key integer");
    if ((rhs > 0 && lhs > std::numeric_limits<long long>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<long long>::min() - rhs))
      throw std::overflow_error("sum key integer overflow");
    return std::to_string(lhs + rhs);
  }
  if (StartsWith(expression, "cast:")) {
    const auto rest = expression.substr(5);
    const auto pos = rest.find(':');
    return CrudFieldValue(values, pos == std::string::npos ? rest : rest.substr(0, pos));
  }
  for (const auto operation : {"lower", "upper", "length"}) {
    const auto argument = ExtractFunctionArgument(expression, operation);
    if (!argument.empty()) return unary(argument, operation);
  }
  return CrudFieldValue(values, expression);
}

bool ExpressionEnvelopeSupported(const std::string& expression) {
  return StartsWith(expression, "lower:") || StartsWith(expression, "upper:") || StartsWith(expression, "length:") ||
         StartsWith(expression, "identity:") || StartsWith(expression, "cast:") || StartsWith(expression, "desc:") ||
         StartsWith(expression, "sum:") ||
         !ExtractFunctionArgument(expression, "lower").empty() ||
         !ExtractFunctionArgument(expression, "upper").empty() || !ExtractFunctionArgument(expression, "length").empty() ||
         (!expression.empty() && expression.find(':') == std::string::npos &&
          expression.find('(') == std::string::npos && expression.find(')') == std::string::npos);
}

std::vector<std::string> CrudIndexEntryKeysForValues(const CrudIndexRecord& index,
                                                     const CrudValueFields& values) {
  if (!index.predicate_kind.empty()) {
    if (index.predicate_kind == "where_eq" && CrudFieldValue(values, index.predicate_column) != index.predicate_value) { return {}; }
    if (index.predicate_kind == "where_mod_eq") {
      const auto equals = index.predicate_value.find('=');
      if (equals == std::string::npos) { return {}; }
      try {
        const auto field = CrudFieldValue(values, index.predicate_column);
        if (!field.isPresent()) return {};
        const auto value = std::stoll(field.bytes);
        const auto divisor = std::stoll(index.predicate_value.substr(0, equals));
        const auto expected = std::stoll(index.predicate_value.substr(equals + 1));
        if (divisor == 0 || value % divisor != expected) { return {}; }
      } catch (...) {
        return {};
      }
    }
    if (index.predicate_kind != "where_eq" &&
        index.predicate_kind != "where_mod_eq" &&
        index.predicate_kind != "where_true") {
      return {};
    }
  }
  const auto columns = CrudIndexKeyColumns(index);
  const std::string family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (columns.empty()) { return {}; }
  if (CrudTokenIndexFamily(family) || CrudNgramIndexFamily(family) ||
      family == kCrudIndexFamilyDocumentPath) {
    const auto value = CrudFieldValue(values, columns.front());
    if (value.isSqlNull()) return {};
    if (!value.isPresent()) throw std::invalid_argument("unresolved token index value state");
    auto tokens = CrudTokenIndexFamily(family) ? TokenizeCrudText(value.bytes)
        : CrudNgramIndexFamily(family) ? TokenizeCrudNgrams(value.bytes)
                                      : TokenizeCrudDocumentPath(value.bytes);
    for (auto& token : tokens) token = EncodeStoredLogicalKey({CrudStoredValue(std::move(token))});
    return tokens;
  }
  if (family == kCrudIndexFamilyExpression)
    return {EncodeStoredLogicalKey({ExpressionKeyForValues(columns.front(), values)})};
  return {BuildCompositeKey(values, columns)};
}

std::string CrudIndexPayloadForValues(const CrudIndexRecord& index,
                                      const CrudValueFields& values) {
  CrudValueFields payload;
  for (const auto& column : index.include_columns) { payload.push_back({column, CrudFieldValue(values, column)}); }
  return EncodeCrudValues(payload);
}

bool EntryKeyMatchesPredicate(const CrudIndexRecord& index,
                              const CrudIndexEntryRecord& entry,
                              const EnginePredicateEnvelope& predicate) {
  if (predicate.predicate_kind.empty()) { return true; }
  const auto columns = CrudIndexKeyColumns(index);
  const std::string first_column = columns.empty() ? index.column_name : columns.front();
  const std::string logical_entry_key =
      CrudIndexEntryLogicalKey(index, entry);
  const auto components = DecodeStoredLogicalKey(logical_entry_key, columns.size());
  if (!components || components->empty()) return false;
  const auto& first_value = components->front();
  if (!first_value.isPresent()) return false;
  const auto& first_key = first_value.bytes;
  if (predicate.predicate_kind == "column_equals") {
    return predicate.canonical_predicate_envelope == first_column && !predicate.bound_values.empty() &&
           CrudPredicateValuePayload(predicate.bound_values.front()) == first_key;
  }
  if (predicate.predicate_kind == "columns_all_equal") {
    const auto predicate_columns = Split(predicate.canonical_predicate_envelope, ',');
    const auto& key_parts = *components;
    if (predicate_columns.empty() || predicate.bound_values.size() < predicate_columns.size() ||
        columns.size() < predicate_columns.size() || key_parts.size() < predicate_columns.size()) {
      return false;
    }
    for (std::size_t index = 0; index < predicate_columns.size(); ++index) {
      if (columns[index] != predicate_columns[index] ||
          (!key_parts[index].isPresent() || CrudPredicateValuePayload(predicate.bound_values[index]) != key_parts[index].bytes)) {
        return false;
      }
    }
    return true;
  }
  if (predicate.predicate_kind == "column_in_list") {
    if (predicate.canonical_predicate_envelope != first_column) { return false; }
    for (const auto& value : predicate.bound_values) {
      if (CrudPredicateValuePayload(value) == first_key) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "column_range") {
    if (predicate.canonical_predicate_envelope != first_column) { return false; }
    const auto lower = predicate.bound_values.empty() ? std::optional<int>{0}
        : CompareCrudPredicateValue(first_key, predicate.bound_values[0], CompareCrudScalar);
    const auto upper = predicate.bound_values.size() < 2 ? std::optional<int>{0}
        : CompareCrudPredicateValue(first_key, predicate.bound_values[1], CompareCrudScalar);
    const bool lower_ok = lower && *lower >= 0;
    const bool upper_ok = upper && *upper <= 0;
    return lower_ok && upper_ok;
  }
  if (predicate.predicate_kind == "text_term_contains") {
    const std::string term = !predicate.bound_values.empty() ? predicate.bound_values.front().encoded_value : predicate.canonical_predicate_envelope;
    return first_key == LowerAscii(term);
  }
  if (predicate.predicate_kind == "text_all_terms") {
    for (const auto& term : predicate.bound_values) {
      if (first_key == LowerAscii(term.encoded_value)) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "expression_equals") {
    return predicate.canonical_predicate_envelope == first_column && !predicate.bound_values.empty() &&
           first_key == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "spatial_bbox_intersects" || predicate.predicate_kind == "spatial_bbox_contains") {
    if (predicate.canonical_predicate_envelope != first_column || predicate.bound_values.empty()) { return false; }
    const auto row_box = ParseCrudBBox(first_key);
    const auto query_box = ParseCrudBBox(predicate.bound_values.front().encoded_value);
    if (predicate.predicate_kind == "spatial_bbox_intersects") { return BBoxIntersects(row_box, query_box); }
    return BBoxContains(row_box, query_box);
