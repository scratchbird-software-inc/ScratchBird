// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
                                               StringOperatorIs(operator_id, "op_position", "operator.string.position") ? "int64" :
                                               "boolean"));
  }
  if (StringOperatorIs(operator_id, "op_concat", "operator.string.concat")) {
    if (left.payload_kind == SblrValuePayloadKind::binary || right.payload_kind == SblrValuePayloadKind::binary) {
      if (left.payload_kind != SblrValuePayloadKind::binary || right.payload_kind != SblrValuePayloadKind::binary) {
        return OperatorDiagnosticFailure(operator_id,
                                         context,
                                         "SBLR.DESCRIPTOR_MISMATCH",
                                         "binary concat requires both operands to be binary");
      }
      auto bytes = left.binary_value;
      bytes.insert(bytes.end(), right.binary_value.begin(), right.binary_value.end());
      return ScalarResult(operator_id, BinaryValue(std::move(bytes)));
    }
    if (!IsTextualPayload(left) || !IsTextualPayload(right)) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.DESCRIPTOR_MISMATCH",
                                       "text concat requires text-compatible operands");
    }
    if (!TextMetadataCompatible(left, right)) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.CHARSET_COLLATION_MISMATCH",
                                       "text concat operands have incompatible charset or collation metadata");
    }
    return ScalarResult(operator_id, TextValueWithMetadata(TextPayload(left) + TextPayload(right), left, right));
  }
  if (left.payload_kind == SblrValuePayloadKind::binary || right.payload_kind == SblrValuePayloadKind::binary) {
    if (left.payload_kind != SblrValuePayloadKind::binary || right.payload_kind != SblrValuePayloadKind::binary) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.DESCRIPTOR_MISMATCH",
                                       "binary string operator requires both operands to be binary");
    }
    if (StringOperatorIs(operator_id, "op_position", "operator.string.position")) {
      if (left.binary_value.empty()) return ScalarResult(operator_id, Int64Value(1));
      auto it = std::search(right.binary_value.begin(), right.binary_value.end(),
                            left.binary_value.begin(), left.binary_value.end());
      return ScalarResult(operator_id, Int64Value(it == right.binary_value.end() ? 0 :
                                                  static_cast<std::int64_t>(std::distance(right.binary_value.begin(), it) + 1)));
    }
    if (StringOperatorIs(operator_id, "op_contains", "operator.string.contains")) {
      const bool matched = left.binary_value.empty() ||
                           std::search(right.binary_value.begin(), right.binary_value.end(),
                                       left.binary_value.begin(), left.binary_value.end()) != right.binary_value.end();
      return ScalarResult(operator_id, BoolValue(matched ? SblrTruthValue::true_value : SblrTruthValue::false_value));
    }
    if (StringOperatorIs(operator_id, "op_starts_with", "operator.string.starts_with")) {
      const bool matched = left.binary_value.size() <= right.binary_value.size() &&
                           std::equal(left.binary_value.begin(), left.binary_value.end(), right.binary_value.begin());
      return ScalarResult(operator_id, BoolValue(matched ? SblrTruthValue::true_value : SblrTruthValue::false_value));
    }
    if (StringOperatorIs(operator_id, "op_ends_with", "operator.string.ends_with")) {
      const bool matched = left.binary_value.size() <= right.binary_value.size() &&
                           std::equal(left.binary_value.rbegin(), left.binary_value.rend(), right.binary_value.rbegin());
      return ScalarResult(operator_id, BoolValue(matched ? SblrTruthValue::true_value : SblrTruthValue::false_value));
    }
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.DESCRIPTOR_MISMATCH",
                                     "LIKE and regex require text operands");
  }
  if (!IsTextualPayload(left) || !IsTextualPayload(right)) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.DESCRIPTOR_MISMATCH",
                                     "string operator requires text-compatible operands");
  }
  if (!TextMetadataCompatible(left, right)) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.CHARSET_COLLATION_MISMATCH",
                                     "string operands have incompatible charset or collation metadata");
  }
  const std::string left_text = TextPayload(left);
  const std::string right_text = TextPayload(right);
  const bool case_insensitive = StringOperatorIs(operator_id, "op_ilike", "operator.string.ilike") ||
                                CollationIsCaseInsensitive(left.collation_name) ||
                                CollationIsCaseInsensitive(right.collation_name);
  if (StringOperatorIs(operator_id, "op_like", "operator.string.like") ||
      StringOperatorIs(operator_id, "op_ilike", "operator.string.ilike")) {
    const auto matched = LikeMatch(left_text, right_text, case_insensitive);
    if (!matched.ok) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.INVALID_PATTERN", "invalid LIKE escape sequence");
    }
    return ScalarResult(operator_id, BoolValue(matched.matched ? SblrTruthValue::true_value
                                                               : SblrTruthValue::false_value));
  }
  if (StringOperatorIs(operator_id, "op_position", "operator.string.position")) {
    const auto haystack = case_insensitive ? [&right_text]() {
      std::string folded = right_text;
      for (auto& ch : folded) ch = FoldAscii(ch, true);
      return folded;
    }() : right_text;
    const auto needle = case_insensitive ? [&left_text]() {
      std::string folded = left_text;
      for (auto& ch : folded) ch = FoldAscii(ch, true);
      return folded;
    }() : left_text;
    const auto pos = haystack.find(needle);
    return ScalarResult(operator_id, Int64Value(pos == std::string::npos ? 0 : static_cast<std::int64_t>(pos + 1)));
  }
  if (StringOperatorIs(operator_id, "op_contains", "operator.string.contains")) {
    const auto pos_result = EvaluateSblrStringOperator("op_position", left, right, context);
    if (!pos_result.ok() || pos_result.scalar_values.empty()) return pos_result;
    return ScalarResult(operator_id, BoolValue(pos_result.scalar_values.front().int64_value > 0
                                                  ? SblrTruthValue::true_value
                                                  : SblrTruthValue::false_value));
  }
  if (StringOperatorIs(operator_id, "op_starts_with", "operator.string.starts_with")) {
    if (right_text.size() < left_text.size()) {
      return ScalarResult(operator_id, BoolValue(SblrTruthValue::false_value));
    }
    bool matched = true;
    for (std::size_t i = 0; i < left_text.size(); ++i) {
      if (FoldAscii(right_text[i], case_insensitive) != FoldAscii(left_text[i], case_insensitive)) {
        matched = false;
        break;
      }
    }
    return ScalarResult(operator_id, BoolValue(matched ? SblrTruthValue::true_value : SblrTruthValue::false_value));
  }
  if (StringOperatorIs(operator_id, "op_ends_with", "operator.string.ends_with")) {
    if (right_text.size() < left_text.size()) {
      return ScalarResult(operator_id, BoolValue(SblrTruthValue::false_value));
    }
    bool matched = true;
    const auto offset = right_text.size() - left_text.size();
    for (std::size_t i = 0; i < left_text.size(); ++i) {
      if (FoldAscii(right_text[offset + i], case_insensitive) != FoldAscii(left_text[i], case_insensitive)) {
        matched = false;
        break;
      }
    }
    return ScalarResult(operator_id, BoolValue(matched ? SblrTruthValue::true_value : SblrTruthValue::false_value));
  }
  try {
    const auto flags = case_insensitive ? (std::regex::ECMAScript | std::regex::icase) : std::regex::ECMAScript;
    return ScalarResult(operator_id, BoolValue(std::regex_search(left_text, std::regex(right_text, flags))
                                                  ? SblrTruthValue::true_value
                                                  : SblrTruthValue::false_value));
  } catch (const std::regex_error&) {
    return OperatorDiagnosticFailure(operator_id, context, "SBLR.INVALID_PATTERN", "invalid regular expression pattern");
  }
}

SblrResult EvaluateSblrDocumentOperator(std::string_view operator_id,
                                        const SblrValue& left,
                                        const SblrValue& right,
                                        const SblrExecutionContext& context) {
  if (!DocumentOperatorIs(operator_id, "op_json_get", "operator.document.json_get") &&
      !DocumentOperatorIs(operator_id, "op_json_get_text", "operator.document.json_get_text") &&
      !DocumentOperatorIs(operator_id, "op_json_exists", "operator.document.json_exists") &&
      !DocumentOperatorIs(operator_id, "op_json_contains", "operator.document.json_contains") &&
      !DocumentOperatorIs(operator_id, "op_json_merge", "operator.document.json_merge") &&
      !DocumentOperatorIs(operator_id, "op_json_patch", "operator.document.json_patch")) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SB_DIAG_OPERATOR_INVALID_INPUT",
                                     "document operator is not registered");
  }
  if (left.is_null || right.is_null) {
    if (DocumentOperatorIs(operator_id, "op_json_exists", "operator.document.json_exists") ||
        DocumentOperatorIs(operator_id, "op_json_contains", "operator.document.json_contains")) {
      return ScalarResult(operator_id, NullValue("boolean"));
    }
    if (DocumentOperatorIs(operator_id, "op_json_get_text", "operator.document.json_get_text")) {
      return ScalarResult(operator_id, NullValue("text"));
    }
    return ScalarResult(operator_id, NullValue("json_document"));
  }

  JsonValue left_document;
  if (!ParseJsonOperand(left, &left_document)) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.JSON_INVALID",
                                     "left document operand is not valid canonical JSON");
  }

  if (DocumentOperatorIs(operator_id, "op_json_get", "operator.document.json_get") ||
      DocumentOperatorIs(operator_id, "op_json_get_text", "operator.document.json_get_text") ||
      DocumentOperatorIs(operator_id, "op_json_exists", "operator.document.json_exists")) {
    const std::string path = TextPayload(right);
    if (path.empty()) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.JSON_PATH_INVALID", "JSON path operand is empty");
    }
    const JsonValue* found = JsonPathLookup(left_document, path);
    if (DocumentOperatorIs(operator_id, "op_json_exists", "operator.document.json_exists")) {
      return ScalarResult(operator_id, BoolValue(found == nullptr ? SblrTruthValue::false_value : SblrTruthValue::true_value));
    }
    if (DocumentOperatorIs(operator_id, "op_json_get_text", "operator.document.json_get_text")) {
      if (found == nullptr || found->kind == JsonValue::Kind::null_value) {
        return ScalarResult(operator_id, NullValue("text"));
      }
      if (found->kind == JsonValue::Kind::string || found->kind == JsonValue::Kind::number) {
        return ScalarResult(operator_id, TextValue(found->scalar));
      }
      if (found->kind == JsonValue::Kind::boolean) {
        return ScalarResult(operator_id, TextValue(found->bool_value ? "true" : "false"));
      }
      return ScalarResult(operator_id, TextValue(JsonCanonical(*found)));
    }
    if (found == nullptr) return ScalarResult(operator_id, NullValue("json_document"));
    return ScalarResult(operator_id, JsonDocumentValue(*found));
  }

  JsonValue right_document;
  if (!ParseJsonOperand(right, &right_document)) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.JSON_INVALID",
                                     "right document operand is not valid canonical JSON");
  }
  if (DocumentOperatorIs(operator_id, "op_json_contains", "operator.document.json_contains")) {
    return ScalarResult(operator_id,
                        BoolValue(JsonContains(left_document, right_document) ? SblrTruthValue::true_value
                                                                              : SblrTruthValue::false_value));
  }
  if (DocumentOperatorIs(operator_id, "op_json_merge", "operator.document.json_merge") ||
      DocumentOperatorIs(operator_id, "op_json_patch", "operator.document.json_patch")) {
    return ScalarResult(operator_id, JsonDocumentValue(JsonMerge(std::move(left_document), right_document)));
  }
  return OperatorDiagnosticFailure(operator_id,
                                   context,
                                   "SB_DIAG_OPERATOR_INVALID_INPUT",
                                   "document operator is unknown or unsupported by this profile");
}

SblrResult EvaluateSblrCollectionOperator(std::string_view operator_id,
                                          const SblrValue& left,
                                          const SblrValue& right,
                                          const SblrExecutionContext& context) {
  if (!CollectionOperatorIs(operator_id, "op_array_subscript", "operator.collection.subscript") &&
      !CollectionOperatorIs(operator_id, "op_collection_contains", "operator.collection.contains", "op_array_contains") &&
      !CollectionOperatorIs(operator_id, "op_collection_overlap", "operator.collection.overlap") &&
      !CollectionOperatorIs(operator_id, "op_collection_concat", "operator.collection.concat") &&
      !CollectionOperatorIs(operator_id, "op_collection_cardinality", "operator.collection.cardinality")) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SB_DIAG_OPERATOR_INVALID_INPUT",
                                     "collection operator is not registered");
  }
  if (left.is_null) {
    if (CollectionOperatorIs(operator_id, "op_collection_cardinality", "operator.collection.cardinality")) {
      return ScalarResult(operator_id, NullValue("uint64"));
    }
    if (CollectionOperatorIs(operator_id, "op_collection_contains", "operator.collection.contains", "op_array_contains") ||
        CollectionOperatorIs(operator_id, "op_collection_overlap", "operator.collection.overlap")) {
      return ScalarResult(operator_id, NullValue("boolean"));
    }
    return ScalarResult(operator_id, NullValue("array"));
  }

  CollectionValue left_collection;
  if (!ParseCollectionOperand(left, &left_collection)) {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.COLLECTION_INVALID",
                                     "left operand is not a valid JSON array/list or SBSET1 set");
  }

  if (CollectionOperatorIs(operator_id, "op_collection_cardinality", "operator.collection.cardinality")) {
    SblrValue out;
    out.descriptor_id = "uint64";
    out.payload_kind = SblrValuePayloadKind::unsigned_integer;
    out.has_uint64_value = true;
    out.uint64_value = CollectionCardinality(left_collection);
    out.encoded_value = std::to_string(out.uint64_value);
    out.text_value = out.encoded_value;
