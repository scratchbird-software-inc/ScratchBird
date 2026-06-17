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

std::string FirstCompositeComponent(const std::string& key) {
  const auto pos = key.find(kCompositeSeparator);
  if (pos == std::string::npos) { return key; }
  return key.substr(0, pos);
}

std::string BuildCompositeKey(const std::vector<std::pair<std::string, std::string>>& values,
                              const std::vector<std::string>& columns) {
  std::string key;
  for (const auto& column : columns) {
    if (!key.empty()) { key.push_back(kCompositeSeparator); }
    key += CrudFieldValue(values, column);
  }
  return key;
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

std::string ExpressionKeyForValues(const std::string& expression,
                                   const std::vector<std::pair<std::string, std::string>>& values) {
  if (StartsWith(expression, "lower:")) { return LowerAscii(CrudFieldValue(values, expression.substr(6))); }
  if (StartsWith(expression, "upper:")) {
    std::string value = CrudFieldValue(values, expression.substr(6));
    for (char& c : value) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return value;
  }
  if (StartsWith(expression, "length:")) { return std::to_string(CrudFieldValue(values, expression.substr(7)).size()); }
  if (StartsWith(expression, "identity:")) { return CrudFieldValue(values, expression.substr(9)); }
  const std::string lower_arg = ExtractFunctionArgument(expression, "lower");
  if (!lower_arg.empty()) { return LowerAscii(CrudFieldValue(values, lower_arg)); }
  const std::string upper_arg = ExtractFunctionArgument(expression, "upper");
  if (!upper_arg.empty()) {
    std::string value = CrudFieldValue(values, upper_arg);
    for (char& c : value) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return value;
  }
  const std::string length_arg = ExtractFunctionArgument(expression, "length");
  if (!length_arg.empty()) { return std::to_string(CrudFieldValue(values, length_arg).size()); }
  return CrudFieldValue(values, expression);
}

bool ExpressionEnvelopeSupported(const std::string& expression) {
  return StartsWith(expression, "lower:") || StartsWith(expression, "upper:") || StartsWith(expression, "length:") ||
         StartsWith(expression, "identity:") || !ExtractFunctionArgument(expression, "lower").empty() ||
         !ExtractFunctionArgument(expression, "upper").empty() || !ExtractFunctionArgument(expression, "length").empty();
}

std::vector<std::string> CrudIndexEntryKeysForValues(const CrudIndexRecord& index,
                                                     const std::vector<std::pair<std::string, std::string>>& values) {
  if (!index.predicate_kind.empty()) {
    if (index.predicate_kind == "where_eq" && CrudFieldValue(values, index.predicate_column) != index.predicate_value) { return {}; }
  }
  const auto columns = CrudIndexKeyColumns(index);
  const std::string family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (columns.empty()) { return {}; }
  if (family == kCrudIndexFamilyFullText) { return TokenizeCrudText(CrudFieldValue(values, columns.front())); }
  if (family == kCrudIndexFamilyExpression) { return {ExpressionKeyForValues(columns.front(), values)}; }
  return {BuildCompositeKey(values, columns)};
}

std::string CrudIndexPayloadForValues(const CrudIndexRecord& index,
                                      const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<std::pair<std::string, std::string>> payload;
  for (const auto& column : index.include_columns) { payload.push_back({column, CrudFieldValue(values, column)}); }
  return EncodeCrudPairs(payload);
}

bool EntryKeyMatchesPredicate(const CrudIndexRecord& index,
                              const CrudIndexEntryRecord& entry,
                              const EnginePredicateEnvelope& predicate) {
  if (predicate.predicate_kind.empty()) { return true; }
  const auto columns = CrudIndexKeyColumns(index);
  const std::string first_column = columns.empty() ? index.column_name : columns.front();
  const std::string first_key = FirstCompositeComponent(entry.key_value);
  if (predicate.predicate_kind == "column_equals") {
    return predicate.canonical_predicate_envelope == first_column && !predicate.bound_values.empty() &&
           first_key == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "columns_all_equal") {
    const auto predicate_columns = Split(predicate.canonical_predicate_envelope, ',');
    const auto key_parts = Split(entry.key_value, kCompositeSeparator);
    if (predicate_columns.empty() || predicate.bound_values.size() < predicate_columns.size() ||
        columns.size() < predicate_columns.size() || key_parts.size() < predicate_columns.size()) {
      return false;
    }
    for (std::size_t index = 0; index < predicate_columns.size(); ++index) {
      if (columns[index] != predicate_columns[index] ||
          key_parts[index] != predicate.bound_values[index].encoded_value) {
        return false;
      }
    }
    return true;
  }
  if (predicate.predicate_kind == "column_in_list") {
    if (predicate.canonical_predicate_envelope != first_column) { return false; }
    for (const auto& value : predicate.bound_values) {
      if (first_key == value.encoded_value) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "column_range") {
    if (predicate.canonical_predicate_envelope != first_column) { return false; }
    const bool lower_ok = predicate.bound_values.empty() || CompareCrudScalar(first_key, predicate.bound_values[0].encoded_value) >= 0;
    const bool upper_ok = predicate.bound_values.size() < 2 || CompareCrudScalar(first_key, predicate.bound_values[1].encoded_value) <= 0;
    return lower_ok && upper_ok;
  }
  if (predicate.predicate_kind == "text_term_contains") {
    const std::string term = !predicate.bound_values.empty() ? predicate.bound_values.front().encoded_value : predicate.canonical_predicate_envelope;
    return entry.key_value == LowerAscii(term);
  }
  if (predicate.predicate_kind == "text_all_terms") {
    for (const auto& term : predicate.bound_values) {
      if (entry.key_value == LowerAscii(term.encoded_value)) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "expression_equals") {
    return predicate.canonical_predicate_envelope == first_column && !predicate.bound_values.empty() &&
           first_key == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "spatial_bbox_intersects" || predicate.predicate_kind == "spatial_bbox_contains") {
    if (predicate.canonical_predicate_envelope != first_column || predicate.bound_values.empty()) { return false; }
    const auto row_box = ParseCrudBBox(entry.key_value);
    const auto query_box = ParseCrudBBox(predicate.bound_values.front().encoded_value);
    if (predicate.predicate_kind == "spatial_bbox_intersects") { return BBoxIntersects(row_box, query_box); }
    return BBoxContains(row_box, query_box);
