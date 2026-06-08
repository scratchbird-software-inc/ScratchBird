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

bool VectorOperatorIs(std::string_view operator_id,
                      std::string_view normalized,
                      std::string_view legacy,
                      std::string_view legacy_alt = {}) {
  return operator_id == normalized || operator_id == legacy || (!legacy_alt.empty() && operator_id == legacy_alt);
}

long double VectorDotProduct(const VectorValue& left, const VectorValue& right) {
  long double dot = 0;
  for (std::size_t i = 0; i < left.elements.size(); ++i) {
    dot += left.elements[i] * right.elements[i];
  }
  return dot;
}

long double VectorSquaredDistance(const VectorValue& left, const VectorValue& right) {
  long double sum = 0;
  for (std::size_t i = 0; i < left.elements.size(); ++i) {
    const long double delta = left.elements[i] - right.elements[i];
    sum += delta * delta;
  }
  return sum;
}

long double VectorNorm(const VectorValue& value) {
  long double sum = 0;
  for (long double element : value.elements) sum += element * element;
  return std::sqrt(sum);
}

std::string LowerAsciiCopy(std::string text) {
  for (auto& ch : text) ch = FoldAscii(ch, true);
  return text;
}

struct Point2d {
  long double x = 0;
  long double y = 0;
};

bool ParsePointFromJson(const JsonValue& json, Point2d* out) {
  if (json.kind != JsonValue::Kind::array || json.array_values.size() != 2) return false;
  return JsonNumberToLongDouble(json.array_values[0], &out->x) &&
         JsonNumberToLongDouble(json.array_values[1], &out->y);
}

bool ParsePointOperand(const SblrValue& value, Point2d* out) {
  const std::string encoded = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (encoded.empty()) return false;
  JsonValue json;
  if (JsonParser(encoded).Parse(&json)) return ParsePointFromJson(json, out);
  std::string text = encoded;
  if (text.rfind("POINT(", 0) == 0 && text.size() > 7 && text.back() == ')') {
    text = text.substr(6, text.size() - 7);
  } else if (text.rfind("point(", 0) == 0 && text.size() > 7 && text.back() == ')') {
    text = text.substr(6, text.size() - 7);
  }
  for (auto& ch : text) {
    if (ch == ',') ch = ' ';
  }
  std::istringstream stream(text);
  std::string x_text;
  std::string y_text;
  std::string trailing;
  if (!(stream >> x_text >> y_text) || (stream >> trailing)) return false;
  return TryLongDoubleText(x_text, &out->x) && TryLongDoubleText(y_text, &out->y);
}

bool SpecializedOperatorIs(std::string_view operator_id,
                           std::string_view normalized,
                           std::string_view legacy,
                           std::string_view legacy_alt = {}) {
  return operator_id == normalized || operator_id == legacy || (!legacy_alt.empty() && operator_id == legacy_alt);
}

datatypes::AdvancedDatatypeOperationKind SpecializedOperationKind(std::string_view operator_id) {
  if (SpecializedOperatorIs(operator_id, "op_spatial_contains", "operator.spatial.contains")) {
    return datatypes::AdvancedDatatypeOperationKind::contains;
  }
  if (SpecializedOperatorIs(operator_id, "op_spatial_intersects", "operator.spatial.intersects")) {
    return datatypes::AdvancedDatatypeOperationKind::intersects;
  }
  if (SpecializedOperatorIs(operator_id, "op_spatial_distance", "operator.spatial.distance")) {
    return datatypes::AdvancedDatatypeOperationKind::distance;
  }
  if (SpecializedOperatorIs(operator_id, "op_search_match", "operator.search.match")) {
    return datatypes::AdvancedDatatypeOperationKind::search_match;
  }
  if (SpecializedOperatorIs(operator_id, "op_search_rank", "operator.search.rank")) {
    return datatypes::AdvancedDatatypeOperationKind::rank;
  }
  if (SpecializedOperatorIs(operator_id, "op_graph_path_match", "operator.graph.path_match")) {
    return datatypes::AdvancedDatatypeOperationKind::path_match;
  }
  return datatypes::AdvancedDatatypeOperationKind::unsupported;
}

datatypes::CanonicalTypeId SpecializedAdmissionType(std::string_view operator_id, const SblrValue& left) {
  if (SpecializedOperatorIs(operator_id, "op_spatial_contains", "operator.spatial.contains") ||
      SpecializedOperatorIs(operator_id, "op_spatial_intersects", "operator.spatial.intersects") ||
      SpecializedOperatorIs(operator_id, "op_spatial_distance", "operator.spatial.distance")) {
    const auto type_id = datatypes::CanonicalTypeIdFromStableName(left.descriptor_id);
    return datatypes::AdvancedDatatypeFamilyFor(type_id) == datatypes::AdvancedDatatypeFamily::spatial
               ? type_id
               : datatypes::CanonicalTypeId::point;
  }
  if (SpecializedOperatorIs(operator_id, "op_search_match", "operator.search.match") ||
      SpecializedOperatorIs(operator_id, "op_search_rank", "operator.search.rank")) {
    const auto type_id = datatypes::CanonicalTypeIdFromStableName(left.descriptor_id);
    return datatypes::AdvancedDatatypeFamilyFor(type_id) == datatypes::AdvancedDatatypeFamily::search
               ? type_id
               : datatypes::CanonicalTypeId::search_query;
  }
  if (SpecializedOperatorIs(operator_id, "op_graph_path_match", "operator.graph.path_match")) {
    const auto type_id = datatypes::CanonicalTypeIdFromStableName(left.descriptor_id);
    return datatypes::AdvancedDatatypeFamilyFor(type_id) == datatypes::AdvancedDatatypeFamily::graph
               ? type_id
               : datatypes::CanonicalTypeId::graph_path;
  }
  return datatypes::CanonicalTypeId::unknown;
}

std::string SpecializedDescriptorProfile(std::string_view operator_id) {
  if (SpecializedOperatorIs(operator_id, "op_spatial_contains", "operator.spatial.contains") ||
      SpecializedOperatorIs(operator_id, "op_spatial_intersects", "operator.spatial.intersects") ||
      SpecializedOperatorIs(operator_id, "op_spatial_distance", "operator.spatial.distance")) {
    return "format=wkb;srid=4326";
  }
  if (SpecializedOperatorIs(operator_id, "op_search_match", "operator.search.match") ||
      SpecializedOperatorIs(operator_id, "op_search_rank", "operator.search.rank")) {
    return "language=en;tokenizer=unicode_v1";
  }
  if (SpecializedOperatorIs(operator_id, "op_graph_path_match", "operator.graph.path_match")) {
    return "direction=directed;schema_uuid=019f0000-0000-7000-8000-000000080801";
  }
  return {};
}

SblrResult AdmitSpecializedOperator(std::string_view operator_id,
                                    const SblrValue& left,
                                    const SblrExecutionContext& context) {
  datatypes::AdvancedDatatypeFamilyRequest request;
  request.type_id = SpecializedAdmissionType(operator_id, left);
  request.operation = SpecializedOperationKind(operator_id);
  request.index_kind = datatypes::AdvancedDatatypeIndexKind::none;
  request.descriptor_profile = SpecializedDescriptorProfile(operator_id);
  const auto result = datatypes::EvaluateAdvancedDatatypeFamily(request);
  if (result.ok()) return MakeSblrSuccess(std::string(operator_id));
  return OperatorDiagnosticFailure(operator_id,
                                   context,
                                   result.diagnostic.diagnostic_code.empty()
                                       ? "SBLR.SPECIALIZED_OPERATOR_REJECTED"
                                       : result.diagnostic.diagnostic_code,
                                   result.diagnostic_detail.empty()
                                       ? "advanced datatype family admission rejected specialized operator"
                                       : result.diagnostic_detail);
}

std::uint64_t SearchOccurrenceCount(std::string haystack, std::string needle) {
  haystack = LowerAsciiCopy(std::move(haystack));
  needle = LowerAsciiCopy(std::move(needle));
  if (needle.empty()) return 0;
  std::uint64_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

bool GraphPathContains(const JsonValue& path, const JsonValue& candidate) {
  if (path.kind != JsonValue::Kind::array) return false;
  for (const auto& value : path.array_values) {
    if (JsonEquals(value, candidate)) return true;
  }
  return false;
}

}  // namespace

const std::vector<SblrOperatorEntry>& StandardSblrOperatorRegistry() {
  static const std::vector<SblrOperatorEntry> registry = {
      {"op_not", "019b6cf8-b000-78b0-aead-68859d790e87", "NOT", "logical", "runtime.operator.not", false},
      {"op_and", "019b6cf8-b000-7a9f-ae1f-0d7501a83abf", "AND", "logical", "runtime.operator.and", true},
      {"op_or", "019b6cf8-b000-7e62-a668-f1d11f359c0f", "OR", "logical", "runtime.operator.or", true},
      {"op_xor", "019b6cf8-b000-7c13-9a01-000000000013", "XOR", "logical", "runtime.operator.xor", true},
      {"op_eq", "019b6cf8-b000-7486-a047-5485f6d6c096", "=", "comparison", "runtime.operator.eq", false},
      {"op_ne", "019b6cf8-b000-7253-a332-a3d413dc812d", "<>", "comparison", "runtime.operator.ne", false},
      {"op_lt", "019b6cf8-b000-7a1a-a808-40c8e06089cc", "<", "comparison", "runtime.operator.lt", false},
      {"op_le", "019b6cf8-b000-7044-94e8-2df026f30cfe", "<=", "comparison", "runtime.operator.le", false},
      {"op_gt", "019b6cf8-b000-72ed-a21d-cf2f2ad06742", ">", "comparison", "runtime.operator.gt", false},
      {"op_ge", "019b6cf8-b000-7946-86c1-8bb0c4d1893c", ">=", "comparison", "runtime.operator.ge", false},
      {"op_is_distinct", "019b6cf8-b000-7272-ac67-84e7e3aca280", "IS DISTINCT FROM", "comparison", "runtime.operator.is_distinct", false},
      {"op_is_null", "019b6cf8-b000-7229-a919-d84f8ae66ebf", "IS NULL", "null_test", "runtime.operator.is_null", false},
      {"op_add", "019b6cf8-b000-7338-a292-32fe85227a3d", "+", "arithmetic", "runtime.operator.add", false},
      {"op_sub", "019b6cf8-b000-7842-ad73-e4c421e73ac5", "-", "arithmetic", "runtime.operator.subtract", false},
      {"op_unary_minus", "019b6cf8-b000-71f0-b235-0f50f1d63c72", "UNARY_MINUS", "arithmetic", "runtime.operator.unary_minus", false},
      {"op_mul", "019b6cf8-b000-7678-aa9d-97813574d3d9", "*", "arithmetic", "runtime.operator.multiply", false},
      {"op_div", "019b6cf8-b000-7e6d-a5fa-095da65c1a2e", "/", "arithmetic", "runtime.operator.divide", false},
      {"op_mod", "019b6cf8-b000-71c6-aa2f-1f4039f64c15", "%", "arithmetic", "runtime.operator.modulo", false},
      {"op_concat", "019b6cf8-b000-7610-a62f-7fdb4ee748a1", "||", "string", "runtime.operator.concat", false},
      {"op_like", "019b6cf8-b000-7681-a626-727b4de9f115", "LIKE", "pattern", "runtime.operator.like", false},
      {"op_regex", "019b6cf8-b000-7ed2-a0f6-1fcd7757f71d", "REGEX", "pattern", "runtime.operator.regex", false},
      {"op_regex_match", "019b6cf8-b000-74b9-9d4c-bbe0c7a64449", "REGEX_MATCH", "pattern", "runtime.operator.regex_match", false},
      {"op_ilike", "019b6cf8-b000-78f1-8c98-39b45f87417a", "ILIKE", "pattern", "runtime.operator.ilike", false},
      {"op_position", "019b6cf8-b000-70b5-a7ee-7eaf09486f6e", "POSITION", "string", "runtime.operator.position", false},
      {"op_contains", "019b6cf8-b000-73e3-a09b-b21510d8a061", "CONTAINS", "string", "runtime.operator.contains", false},
      {"op_starts_with", "019b6cf8-b000-781b-bf93-a28b44b93e92", "STARTS WITH", "string", "runtime.operator.starts_with", false},
      {"op_ends_with", "019b6cf8-b000-79f9-aec7-d5bd8b3fbb47", "ENDS WITH", "string", "runtime.operator.ends_with", false},
      {"op_json_get", "019b6cf8-b000-7293-8759-76dc8c8df01f", "JSON_GET", "document", "runtime.operator.json_get", false},
      {"op_json_get_text", "019b6cf8-b000-7a92-bd1c-a64e281073d6", "JSON_GET_TEXT", "document", "runtime.operator.json_get_text", false},
      {"op_json_exists", "019b6cf8-b000-73dd-bd1b-1f66f0e92af4", "JSON_EXISTS", "document", "runtime.operator.json_exists", false},
      {"op_json_contains", "019b6cf8-b000-7280-9ec4-434f0b09e0db", "JSON_CONTAINS", "document", "runtime.operator.json_contains", false},
      {"op_json_merge", "019b6cf8-b000-7b6b-a154-75c18e39fa95", "JSON_MERGE", "document", "runtime.operator.json_merge", false},
      {"op_json_patch", "019b6cf8-b000-73f2-90b9-8319e9a73a47", "JSON_PATCH", "document", "runtime.operator.json_patch", false},
      {"op_array_subscript", "019b6cf8-b000-7a1d-a003-8657089321db", "[]", "array_list", "runtime.operator.array_subscript", false},
      {"op_collection_contains", "019b6cf8-b000-7759-8532-fb7b1a2113f9", "CONTAINS ELEMENT", "array_list", "runtime.operator.collection_contains", false},
      {"op_array_contains", "019b6cf8-b000-73b4-9af5-e1e1ebf48e66", "ARRAY_CONTAINS", "array_list", "runtime.operator.array_contains", false},
      {"op_collection_overlap", "019b6cf8-b000-7796-99c4-8585e4be70f5", "OVERLAPS", "array_list", "runtime.operator.collection_overlap", false},
      {"op_collection_concat", "019b6cf8-b000-7281-981e-37607a087e9e", "||", "array_list", "runtime.operator.collection_concat", false},
      {"op_collection_cardinality", "019b6cf8-b000-7d5a-abd8-c4d406565d73", "CARDINALITY", "array_list", "runtime.operator.collection_cardinality", false},
      {"op_vector_distance", "019b6cf8-b000-7ef7-91c7-819d9486a111", "<->", "vector", "runtime.operator.vector_distance", false},
      {"op_vector_squared_distance", "019b6cf8-b000-7779-a370-9eab637ae0b2", "<#>", "vector", "runtime.operator.vector_squared_distance", false},
      {"op_vector_dot", "019b6cf8-b000-74b3-bf5b-c3c6f95e71e3", "DOT", "vector", "runtime.operator.vector_dot", false},
      {"op_vector_inner_product", "019b6cf8-b000-72f6-a22b-913f49cf5384", "INNER PRODUCT", "vector", "runtime.operator.vector_inner_product", false},
      {"op_vector_cosine_similarity", "019b6cf8-b000-7891-acb3-7d1ef90712f5", "COSINE SIMILARITY", "vector", "runtime.operator.vector_cosine_similarity", false},
      {"op_vector_cosine_distance", "019b6cf8-b000-786f-9ca0-e02e4c54d589", "COSINE DISTANCE", "vector", "runtime.operator.vector_cosine_distance", false},
      {"op_spatial_contains", "019b6cf8-b000-759c-83a5-eae5b7bf2c09", "SPATIAL_CONTAINS", "spatial", "runtime.operator.spatial_contains", false},
      {"op_spatial_intersects", "019b6cf8-b000-75c8-9a50-611bc921ea0f", "SPATIAL_INTERSECTS", "spatial", "runtime.operator.spatial_intersects", false},
      {"op_spatial_distance", "019b6cf8-b000-7e35-bc3d-69db22288f90", "SPATIAL_DISTANCE", "spatial", "runtime.operator.spatial_distance", false},
      {"op_search_match", "019b6cf8-b000-776b-a491-daa073f95a7b", "SEARCH_MATCH", "search", "runtime.operator.search_match", false},
      {"op_search_rank", "019b6cf8-b000-7975-99d1-9c78f42fa5a4", "SEARCH_RANK", "search", "runtime.operator.search_rank", false},
      {"op_graph_path_match", "019b6cf8-b000-710a-9673-90abb90a1eb0", "PATH_MATCH", "graph", "runtime.operator.graph_path_match", false},
      {"op_bit_and", "019b6cf8-b000-73ef-afee-a17b77365bb0", "&", "bitwise", "runtime.operator.bit_and", false},
      {"op_bit_or", "019b6cf8-b000-7726-a466-78bad74ae90f", "|", "bitwise", "runtime.operator.bit_or", false},
      {"operator.boolean.not", "018f0000-0000-7000-8000-000000001001", "NOT", "logical", "runtime.operator.not", false},
      {"operator.boolean.and", "018f0000-0000-7000-8000-000000001002", "AND", "logical", "runtime.operator.and", true},
      {"operator.boolean.or", "018f0000-0000-7000-8000-000000001003", "OR", "logical", "runtime.operator.or", true},
      {"operator.compare.eq", "018f0000-0000-7000-8000-000000001101", "=", "comparison", "runtime.operator.eq", false},
      {"operator.compare.ne", "018f0000-0000-7000-8000-000000001102", "<>", "comparison", "runtime.operator.ne", false},
      {"operator.compare.lt", "018f0000-0000-7000-8000-000000001103", "<", "comparison", "runtime.operator.lt", false},
      {"operator.compare.le", "018f0000-0000-7000-8000-000000001104", "<=", "comparison", "runtime.operator.le", false},
      {"operator.compare.gt", "018f0000-0000-7000-8000-000000001105", ">", "comparison", "runtime.operator.gt", false},
      {"operator.compare.ge", "018f0000-0000-7000-8000-000000001106", ">=", "comparison", "runtime.operator.ge", false},
      {"operator.numeric.add", "018f0000-0000-7000-8000-000000001201", "+", "arithmetic", "runtime.operator.add", false},
      {"operator.numeric.subtract", "018f0000-0000-7000-8000-000000001202", "-", "arithmetic", "runtime.operator.subtract", false},
      {"operator.numeric.multiply", "018f0000-0000-7000-8000-000000001203", "*", "arithmetic", "runtime.operator.multiply", false},
      {"operator.numeric.divide", "018f0000-0000-7000-8000-000000001204", "/", "arithmetic", "runtime.operator.divide", false},
      {"operator.numeric.modulo", "018f0000-0000-7000-8000-000000001205", "%", "arithmetic", "runtime.operator.modulo", false},
      {"operator.temporal.add", "018f0000-0000-7000-8000-000000001251", "+", "temporal", "runtime.operator.temporal_add", false},
      {"operator.temporal.subtract", "018f0000-0000-7000-8000-000000001252", "-", "temporal", "runtime.operator.temporal_subtract", false},
      {"operator.string.concat", "018f0000-0000-7000-8000-000000001301", "||", "string", "runtime.operator.concat", false},
      {"operator.string.like", "018f0000-0000-7000-8000-000000001302", "LIKE", "pattern", "runtime.operator.like", false},
      {"operator.string.regex", "018f0000-0000-7000-8000-000000001303", "REGEX", "pattern", "runtime.operator.regex", false},
      {"operator.string.ilike", "018f0000-0000-7000-8000-000000001304", "ILIKE", "pattern", "runtime.operator.ilike", false},
      {"operator.string.position", "018f0000-0000-7000-8000-000000001305", "POSITION", "string", "runtime.operator.position", false},
      {"operator.string.contains", "018f0000-0000-7000-8000-000000001306", "CONTAINS", "string", "runtime.operator.contains", false},
      {"operator.string.starts_with", "018f0000-0000-7000-8000-000000001307", "STARTS WITH", "string", "runtime.operator.starts_with", false},
      {"operator.string.ends_with", "018f0000-0000-7000-8000-000000001308", "ENDS WITH", "string", "runtime.operator.ends_with", false},
      {"operator.document.json_get", "018f0000-0000-7000-8000-000000001401", "JSON_GET", "document", "runtime.operator.json_get", false},
      {"operator.document.json_exists", "018f0000-0000-7000-8000-000000001402", "JSON_EXISTS", "document", "runtime.operator.json_exists", false},
      {"operator.document.json_contains", "018f0000-0000-7000-8000-000000001403", "JSON_CONTAINS", "document", "runtime.operator.json_contains", false},
      {"operator.document.json_merge", "018f0000-0000-7000-8000-000000001404", "JSON_MERGE", "document", "runtime.operator.json_merge", false},
      {"operator.document.json_patch", "018f0000-0000-7000-8000-000000001405", "JSON_PATCH", "document", "runtime.operator.json_patch", false},
      {"operator.collection.subscript", "018f0000-0000-7000-8000-000000001501", "[]", "array_list", "runtime.operator.array_subscript", false},
      {"operator.collection.contains", "018f0000-0000-7000-8000-000000001502", "CONTAINS ELEMENT", "array_list", "runtime.operator.collection_contains", false},
      {"operator.collection.overlap", "018f0000-0000-7000-8000-000000001503", "OVERLAPS", "array_list", "runtime.operator.collection_overlap", false},
      {"operator.collection.concat", "018f0000-0000-7000-8000-000000001504", "||", "array_list", "runtime.operator.collection_concat", false},
      {"operator.collection.cardinality", "018f0000-0000-7000-8000-000000001505", "CARDINALITY", "array_list", "runtime.operator.collection_cardinality", false},
      {"operator.vector.distance", "018f0000-0000-7000-8000-000000001601", "<->", "vector", "runtime.operator.vector_distance", false},
      {"operator.vector.squared_distance", "018f0000-0000-7000-8000-000000001602", "<#>", "vector", "runtime.operator.vector_squared_distance", false},
      {"operator.vector.dot", "018f0000-0000-7000-8000-000000001603", "DOT", "vector", "runtime.operator.vector_dot", false},
      {"operator.vector.inner_product", "018f0000-0000-7000-8000-000000001604", "INNER PRODUCT", "vector", "runtime.operator.vector_inner_product", false},
      {"operator.vector.cosine_similarity", "018f0000-0000-7000-8000-000000001605", "COSINE SIMILARITY", "vector", "runtime.operator.vector_cosine_similarity", false},
      {"operator.vector.cosine_distance", "018f0000-0000-7000-8000-000000001606", "COSINE DISTANCE", "vector", "runtime.operator.vector_cosine_distance", false},
      {"operator.spatial.contains", "018f0000-0000-7000-8000-000000001701", "SPATIAL_CONTAINS", "spatial", "runtime.operator.spatial_contains", false},
      {"operator.spatial.intersects", "018f0000-0000-7000-8000-000000001702", "SPATIAL_INTERSECTS", "spatial", "runtime.operator.spatial_intersects", false},
