// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
    out.is_null = false;
    return ScalarResult(operator_id, std::move(out));
  }

  if (right.is_null) {
    if (CollectionOperatorIs(operator_id, "op_collection_contains", "operator.collection.contains", "op_array_contains") ||
        CollectionOperatorIs(operator_id, "op_collection_overlap", "operator.collection.overlap")) {
      return ScalarResult(operator_id, NullValue("boolean"));
    }
    return ScalarResult(operator_id, NullValue("array"));
  }

  if (CollectionOperatorIs(operator_id, "op_array_subscript", "operator.collection.subscript")) {
    std::int64_t index = 0;
    if (right.has_int64_value) {
      index = right.int64_value;
    } else if (!TryInt64Text(ComparableText(right), &index)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.COLLECTION_INDEX_INVALID", "collection subscript index must be an integer");
    }
    if (index < 1) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.COLLECTION_INDEX_INVALID", "collection subscript index is one-based and must be >= 1");
    }
    const auto zero_based = static_cast<std::uint64_t>(index - 1);
    if (left_collection.kind == CollectionValue::Kind::array) {
      if (zero_based >= left_collection.array_values.size()) return ScalarResult(operator_id, NullValue("json_document"));
      return ScalarResult(operator_id, JsonDocumentValue(left_collection.array_values[static_cast<std::size_t>(zero_based)]));
    }
    if (zero_based >= left_collection.set_values.size()) return ScalarResult(operator_id, NullValue("text"));
    return ScalarResult(operator_id, TextValue(left_collection.set_values[static_cast<std::size_t>(zero_based)]));
  }

  if (CollectionOperatorIs(operator_id, "op_collection_contains", "operator.collection.contains", "op_array_contains")) {
    JsonValue right_value;
    bool parsed_json = ParseJsonOperand(right, &right_value);
    bool matched = false;
    if (parsed_json) {
      matched = CollectionContains(left_collection, right_value);
    } else {
      matched = CollectionContainsText(left_collection, TextPayload(right));
    }
    return ScalarResult(operator_id, BoolValue(matched ? SblrTruthValue::true_value : SblrTruthValue::false_value));
  }

  CollectionValue right_collection;
  if (!ParseCollectionOperand(right, &right_collection)) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.COLLECTION_INVALID",
                                     "right operand is not a valid JSON array/list or SBSET1 set");
  }

  if (CollectionOperatorIs(operator_id, "op_collection_overlap", "operator.collection.overlap")) {
    bool matched = false;
    if (left_collection.kind == CollectionValue::Kind::set && right_collection.kind == CollectionValue::Kind::set) {
      for (const auto& candidate : right_collection.set_values) {
        if (std::find(left_collection.set_values.begin(), left_collection.set_values.end(), candidate) != left_collection.set_values.end()) {
          matched = true;
          break;
        }
      }
    } else {
      std::vector<JsonValue> right_values = right_collection.array_values;
      if (right_collection.kind == CollectionValue::Kind::set) {
        right_values.clear();
        for (const auto& text : right_collection.set_values) {
          JsonValue value;
          value.kind = JsonValue::Kind::string;
          value.scalar = text;
          right_values.push_back(std::move(value));
        }
      }
      for (const auto& candidate : right_values) {
        if (CollectionContains(left_collection, candidate)) {
          matched = true;
          break;
        }
      }
    }
    return ScalarResult(operator_id, BoolValue(matched ? SblrTruthValue::true_value : SblrTruthValue::false_value));
  }

  if (CollectionOperatorIs(operator_id, "op_collection_concat", "operator.collection.concat")) {
    if (left_collection.kind == CollectionValue::Kind::set && right_collection.kind == CollectionValue::Kind::set) {
      auto values = left_collection.set_values;
      values.insert(values.end(), right_collection.set_values.begin(), right_collection.set_values.end());
      return ScalarResult(operator_id, CollectionSetValue(std::move(values)));
    }
    if (left_collection.kind != CollectionValue::Kind::array || right_collection.kind != CollectionValue::Kind::array) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.DESCRIPTOR_MISMATCH",
                                       "collection concat requires both operands to be arrays/lists or both operands to be sets");
    }
    auto values = left_collection.array_values;
    values.insert(values.end(), right_collection.array_values.begin(), right_collection.array_values.end());
    return ScalarResult(operator_id, CollectionArrayValue(std::move(values)));
  }

  return OperatorDiagnosticFailure(operator_id,
                                   context,
                                   "SB_DIAG_OPERATOR_INVALID_INPUT",
                                   "collection operator is unknown or unsupported by this profile");
}

SblrResult EvaluateSblrVectorOperator(std::string_view operator_id,
                                      const SblrValue& left,
                                      const SblrValue& right,
                                      const SblrExecutionContext& context) {
  if (!VectorOperatorIs(operator_id, "op_vector_distance", "operator.vector.distance") &&
      !VectorOperatorIs(operator_id, "op_vector_squared_distance", "operator.vector.squared_distance") &&
      !VectorOperatorIs(operator_id, "op_vector_dot", "operator.vector.dot") &&
      !VectorOperatorIs(operator_id, "op_vector_inner_product", "operator.vector.inner_product") &&
      !VectorOperatorIs(operator_id, "op_vector_cosine_similarity", "operator.vector.cosine_similarity") &&
      !VectorOperatorIs(operator_id, "op_vector_cosine_distance", "operator.vector.cosine_distance")) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SB_DIAG_OPERATOR_INVALID_INPUT",
                                     "vector operator is not registered");
  }
  if (left.is_null || right.is_null) {
    return ScalarResult(operator_id, NullValue("real64"));
  }

  VectorValue left_vector;
  VectorValue right_vector;
  if (!ParseVectorOperand(left, &left_vector) || !ParseVectorOperand(right, &right_vector)) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.VECTOR_INVALID",
                                     "vector operands must be dense numeric JSON arrays or comma-separated numeric descriptor payloads");
  }
  if (left_vector.elements.size() != right_vector.elements.size()) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.VECTOR_DIMENSION_MISMATCH",
                                     "vector operands must have identical dimensions");
  }

  const long double dot = VectorDotProduct(left_vector, right_vector);
  if (VectorOperatorIs(operator_id, "op_vector_dot", "operator.vector.dot") ||
      VectorOperatorIs(operator_id, "op_vector_inner_product", "operator.vector.inner_product")) {
    return ScalarResult(operator_id, VectorScalarValue(dot));
  }
  const long double squared_distance = VectorSquaredDistance(left_vector, right_vector);
  if (VectorOperatorIs(operator_id, "op_vector_squared_distance", "operator.vector.squared_distance")) {
    return ScalarResult(operator_id, VectorScalarValue(squared_distance));
  }
  if (VectorOperatorIs(operator_id, "op_vector_distance", "operator.vector.distance")) {
    return ScalarResult(operator_id, VectorScalarValue(std::sqrt(squared_distance)));
  }

  const long double left_norm = VectorNorm(left_vector);
  const long double right_norm = VectorNorm(right_vector);
  if (left_norm == 0.0L || right_norm == 0.0L) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.VECTOR_ZERO_NORM",
                                     "cosine vector operators reject zero-norm operands");
  }
  const long double similarity = dot / (left_norm * right_norm);
  if (VectorOperatorIs(operator_id, "op_vector_cosine_similarity", "operator.vector.cosine_similarity")) {
    return ScalarResult(operator_id, VectorScalarValue(similarity));
  }
  if (VectorOperatorIs(operator_id, "op_vector_cosine_distance", "operator.vector.cosine_distance")) {
    return ScalarResult(operator_id, VectorScalarValue(1.0L - similarity));
  }
  return OperatorDiagnosticFailure(operator_id,
                                   context,
                                   "SB_DIAG_OPERATOR_INVALID_INPUT",
                                   "vector operator is unknown or unsupported by this profile");
}

SblrResult EvaluateSblrSpecializedOperatorBridge(std::string_view operator_id,
                                                 const SblrValue& left,
                                                 const SblrValue& right,
                                                 const SblrExecutionContext& context) {
  if (!SpecializedOperatorIs(operator_id, "op_spatial_contains", "operator.spatial.contains") &&
      !SpecializedOperatorIs(operator_id, "op_spatial_intersects", "operator.spatial.intersects") &&
      !SpecializedOperatorIs(operator_id, "op_spatial_distance", "operator.spatial.distance") &&
      !SpecializedOperatorIs(operator_id, "op_search_match", "operator.search.match") &&
      !SpecializedOperatorIs(operator_id, "op_search_rank", "operator.search.rank") &&
      !SpecializedOperatorIs(operator_id, "op_graph_path_match", "operator.graph.path_match")) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SB_DIAG_OPERATOR_INVALID_INPUT",
                                     "specialized operator bridge is not registered");
  }

  const auto admission = AdmitSpecializedOperator(operator_id, left, context);
  if (!admission.ok()) return admission;

  if (left.is_null || right.is_null) {
    if (SpecializedOperatorIs(operator_id, "op_spatial_distance", "operator.spatial.distance") ||
        SpecializedOperatorIs(operator_id, "op_search_rank", "operator.search.rank")) {
      return ScalarResult(operator_id, NullValue("real64"));
    }
    return ScalarResult(operator_id, NullValue("boolean"));
  }

  if (SpecializedOperatorIs(operator_id, "op_spatial_contains", "operator.spatial.contains") ||
      SpecializedOperatorIs(operator_id, "op_spatial_intersects", "operator.spatial.intersects") ||
      SpecializedOperatorIs(operator_id, "op_spatial_distance", "operator.spatial.distance")) {
    Point2d left_point;
    Point2d right_point;
    if (!ParsePointOperand(left, &left_point) || !ParsePointOperand(right, &right_point)) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.SPATIAL_POINT_INVALID",
                                       "spatial bridge currently requires point payloads encoded as [x,y], x,y, or POINT(x y)");
    }
    const long double dx = left_point.x - right_point.x;
    const long double dy = left_point.y - right_point.y;
    const long double squared = dx * dx + dy * dy;
    if (SpecializedOperatorIs(operator_id, "op_spatial_distance", "operator.spatial.distance")) {
      return ScalarResult(operator_id, VectorScalarValue(std::sqrt(squared)));
    }
    const bool equal_point = squared == 0.0L;
    return ScalarResult(operator_id, BoolValue(equal_point ? SblrTruthValue::true_value : SblrTruthValue::false_value));
  }

  if (SpecializedOperatorIs(operator_id, "op_search_match", "operator.search.match") ||
      SpecializedOperatorIs(operator_id, "op_search_rank", "operator.search.rank")) {
    const std::string document = TextPayload(left);
    const std::string query = TextPayload(right);
    const std::uint64_t occurrences = SearchOccurrenceCount(document, query);
    if (SpecializedOperatorIs(operator_id, "op_search_match", "operator.search.match")) {
      return ScalarResult(operator_id, BoolValue(occurrences == 0 ? SblrTruthValue::false_value
                                                                  : SblrTruthValue::true_value));
    }
    return ScalarResult(operator_id, VectorScalarValue(static_cast<long double>(occurrences)));
  }

  if (SpecializedOperatorIs(operator_id, "op_graph_path_match", "operator.graph.path_match")) {
    JsonValue path;
    if (!ParseJsonOperand(left, &path) || path.kind != JsonValue::Kind::array) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.GRAPH_PATH_INVALID",
                                       "graph path bridge requires the left operand to be a JSON array path");
    }
    JsonValue candidate;
    if (!ParseJsonOperand(right, &candidate)) {
      candidate.kind = JsonValue::Kind::string;
      candidate.scalar = TextPayload(right);
    }
    return ScalarResult(operator_id, BoolValue(GraphPathContains(path, candidate) ? SblrTruthValue::true_value
                                                                                  : SblrTruthValue::false_value));
  }

  return OperatorDiagnosticFailure(operator_id,
                                   context,
                                   "SB_DIAG_OPERATOR_INVALID_INPUT",
                                   "specialized operator bridge is unknown or unsupported by this profile");
}

}  // namespace scratchbird::engine::sblr
