// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_operator_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
      {"operator.spatial.distance", "018f0000-0000-7000-8000-000000001703", "SPATIAL_DISTANCE", "spatial", "runtime.operator.spatial_distance", false},
      {"operator.search.match", "018f0000-0000-7000-8000-000000001801", "SEARCH_MATCH", "search", "runtime.operator.search_match", false},
      {"operator.search.rank", "018f0000-0000-7000-8000-000000001802", "SEARCH_RANK", "search", "runtime.operator.search_rank", false},
      {"operator.graph.path_match", "018f0000-0000-7000-8000-000000001901", "PATH_MATCH", "graph", "runtime.operator.graph_path_match", false},
  };
  return registry;
}

const SblrOperatorEntry* LookupSblrOperator(std::string_view operator_id) {
  for (const auto& entry : StandardSblrOperatorRegistry()) {
    if (entry.operator_id == operator_id) return &entry;
  }
  return nullptr;
}

SblrTruthValue SblrNot(SblrTruthValue value) {
  if (value == SblrTruthValue::unknown) return SblrTruthValue::unknown;
  return value == SblrTruthValue::true_value ? SblrTruthValue::false_value : SblrTruthValue::true_value;
}

SblrTruthValue SblrAnd(SblrTruthValue left, SblrTruthValue right) {
  if (left == SblrTruthValue::false_value || right == SblrTruthValue::false_value) return SblrTruthValue::false_value;
  if (left == SblrTruthValue::unknown || right == SblrTruthValue::unknown) return SblrTruthValue::unknown;
  return SblrTruthValue::true_value;
}

SblrTruthValue SblrOr(SblrTruthValue left, SblrTruthValue right) {
  if (left == SblrTruthValue::true_value || right == SblrTruthValue::true_value) return SblrTruthValue::true_value;
  if (left == SblrTruthValue::unknown || right == SblrTruthValue::unknown) return SblrTruthValue::unknown;
  return SblrTruthValue::false_value;
}

SblrTruthValue SblrXor(SblrTruthValue left, SblrTruthValue right) {
  if (left == SblrTruthValue::unknown || right == SblrTruthValue::unknown) return SblrTruthValue::unknown;
  return left == right ? SblrTruthValue::false_value : SblrTruthValue::true_value;
}

SblrValue MakeSblrTruthValue(SblrTruthValue value) {
  return BoolValue(value);
}

SblrResult EvaluateSblrComparison(std::string_view operator_id,
                                  const SblrValue& left,
                                  const SblrValue& right,
                                  const SblrExecutionContext& context) {
  if (IsOperator(operator_id, "op_is_null", "operator.null.is_null")) {
    return ScalarResult(operator_id, BoolValue(left.is_null ? SblrTruthValue::true_value : SblrTruthValue::false_value));
  }
  if (IsOperator(operator_id, "op_is_distinct", "operator.compare.is_distinct")) {
    if (left.is_null != right.is_null) {
      return ScalarResult(operator_id, BoolValue(SblrTruthValue::true_value));
    }
    if (left.is_null && right.is_null) {
      return ScalarResult(operator_id, BoolValue(SblrTruthValue::false_value));
    }
  } else if (left.is_null || right.is_null) {
    return ScalarResult(operator_id, BoolValue(SblrTruthValue::unknown));
  }

  int cmp = 0;
  if (IsIntervalPayload(left) && IsIntervalPayload(right)) {
    std::int64_t left_seconds = 0;
    std::int64_t right_seconds = 0;
    if (!ParseIntervalSeconds(left, &left_seconds) || !ParseIntervalSeconds(right, &right_seconds)) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.INTERVAL_INVALID",
                                       "interval comparison operands are not canonical seconds or supported ISO day/time durations");
    }
    cmp = left_seconds < right_seconds ? -1 : (left_seconds > right_seconds ? 1 : 0);
  } else if (IsTemporalPayload(left) && IsTemporalPayload(right)) {
    TemporalParsedValue left_temporal;
    TemporalParsedValue right_temporal;
    if (!ParseTemporalValue(left, &left_temporal) || !ParseTemporalValue(right, &right_temporal)) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.TEMPORAL_INVALID",
                                       "temporal comparison operands are not supported canonical temporal values");
    }
    std::int64_t left_value = 0;
    std::int64_t right_value = 0;
    if (left.descriptor_id == "date") left_value = left_temporal.day_number * 86400;
    else if (left.descriptor_id == "time") left_value = left_temporal.time_seconds;
    else left_value = left_temporal.epoch_seconds;
    if (right.descriptor_id == "date") right_value = right_temporal.day_number * 86400;
    else if (right.descriptor_id == "time") right_value = right_temporal.time_seconds;
    else right_value = right_temporal.epoch_seconds;
    cmp = left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
  } else if (IsNumericPayload(left) && IsNumericPayload(right)) {
    auto backend_comparison = CompareDatatypeNumericValues(operator_id, left, right, context);
    if (backend_comparison.attempted) {
      if (!backend_comparison.ok) return backend_comparison.failure;
      cmp = backend_comparison.comparison;
    } else {
      cmp = CompareNumericPayloads(left, right);
    }
  } else if (left.payload_kind == SblrValuePayloadKind::binary &&
             right.payload_kind == SblrValuePayloadKind::binary) {
    cmp = left.binary_value < right.binary_value ? -1 : (left.binary_value > right.binary_value ? 1 : 0);
  } else if (left.descriptor_id == right.descriptor_id ||
             left.descriptor_id.empty() ||
             right.descriptor_id.empty() ||
             left.descriptor_id == "text" ||
             right.descriptor_id == "text") {
    auto backend_comparison = CompareDatatypeCharacterValues(operator_id, left, right, context);
    if (backend_comparison.attempted) {
      if (!backend_comparison.ok) return backend_comparison.failure;
      cmp = backend_comparison.comparison;
    } else {
      cmp = ComparableText(left).compare(ComparableText(right));
    }
  } else {
    return OperatorDiagnosticFailure(operator_id,
                                     context,
                                     "SBLR.DESCRIPTOR_MISMATCH",
                                     "comparison operands are not comparable by the current descriptor rules");
  }

  SblrTruthValue result = SblrTruthValue::false_value;
  if (IsOperator(operator_id, "op_eq", "operator.compare.eq")) result = cmp == 0 ? SblrTruthValue::true_value : SblrTruthValue::false_value;
  else if (IsOperator(operator_id, "op_ne", "operator.compare.ne")) result = cmp != 0 ? SblrTruthValue::true_value : SblrTruthValue::false_value;
  else if (IsOperator(operator_id, "op_lt", "operator.compare.lt")) result = cmp < 0 ? SblrTruthValue::true_value : SblrTruthValue::false_value;
  else if (IsOperator(operator_id, "op_le", "operator.compare.le")) result = cmp <= 0 ? SblrTruthValue::true_value : SblrTruthValue::false_value;
  else if (IsOperator(operator_id, "op_gt", "operator.compare.gt")) result = cmp > 0 ? SblrTruthValue::true_value : SblrTruthValue::false_value;
  else if (IsOperator(operator_id, "op_ge", "operator.compare.ge")) result = cmp >= 0 ? SblrTruthValue::true_value : SblrTruthValue::false_value;
  else if (IsOperator(operator_id, "op_is_distinct", "operator.compare.is_distinct")) result = cmp != 0 ? SblrTruthValue::true_value : SblrTruthValue::false_value;
  else return OperatorFailure(operator_id, context, "comparison operator is not registered");
  return ScalarResult(operator_id, BoolValue(result));
}

SblrResult EvaluateSblrUnaryArithmetic(std::string_view operator_id,
                                       const SblrValue& operand,
                                       const SblrExecutionContext& context) {
  if (!IsAnyOperator(operator_id,
                     "op_unary_minus",
                     "operator.numeric.unary_minus",
                     "operator.arithmetic.unary_minus")) {
    return OperatorFailure(operator_id, context, "unary numeric operator is not registered");
  }
  if (operand.is_null) {
    return ScalarResult(operator_id,
                        NullValue(!operand.descriptor_id.empty() ? operand.descriptor_id : "int64"));
  }
  if (operand.has_int64_value) {
    if (operand.int64_value == std::numeric_limits<std::int64_t>::min()) {
      return OperatorDiagnosticFailure(operator_id,
                                       context,
                                       "SBLR.NUMERIC_OVERFLOW",
                                       "int64 unary minus overflow");
    }
    return ScalarResult(operator_id, Int64Value(-operand.int64_value));
  }
  long double operand_value = 0;
  if (!TryLongDouble(operand, &operand_value)) {
    return OperatorFailure(operator_id, context, "unary minus requires a numeric-compatible operand");
  }
  return ScalarResult(operator_id,
                      RealTextValue(operand.descriptor_id.empty() ? "decimal" : operand.descriptor_id,
                                    -operand_value));
}

SblrResult EvaluateSblrArithmetic(std::string_view operator_id,
                                  const SblrValue& left,
                                  const SblrValue& right,
                                  const SblrExecutionContext& context) {
  if (left.is_null || right.is_null) {
    if (IsTemporalPayload(left) || IsTemporalPayload(right) || IsIntervalPayload(left) || IsIntervalPayload(right)) {
      return ScalarResult(operator_id, NullValue(IsIntervalPayload(left) || IsIntervalPayload(right) ? "interval" :
                                                 !left.descriptor_id.empty() ? left.descriptor_id : right.descriptor_id));
    }
    return ScalarResult(operator_id, NullValue("int64"));
  }
  if (IsTemporalPayload(left) || IsTemporalPayload(right) || IsIntervalPayload(left) || IsIntervalPayload(right) ||
      operator_id == "operator.temporal.add" || operator_id == "operator.temporal.subtract") {
    return EvaluateSblrTemporalArithmetic(operator_id, left, right, context);
  }
  const bool use_exact_int64 = left.has_int64_value && right.has_int64_value;
  std::int64_t result = 0;
  if (IsAnyOperator(operator_id, "op_add", "operator.numeric.add", "operator.arithmetic.add")) {
    if (!use_exact_int64) {
      if (NeedsDatatypeNumericBackend(left, right)) {
        return EvaluateDatatypeNumericArithmetic(operator_id, left, right, context);
      }
      long double left_value = 0;
      long double right_value = 0;
      if (!TryLongDouble(left, &left_value) || !TryLongDouble(right, &right_value)) {
        return OperatorFailure(operator_id, context, "numeric add requires numeric-compatible operands");
      }
      return ScalarResult(operator_id, RealTextValue("decimal", left_value + right_value));
    }
    if (AddWouldOverflow(left.int64_value, right.int64_value)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.NUMERIC_OVERFLOW", "int64 addition overflow");
    }
    result = left.int64_value + right.int64_value;
  } else if (IsAnyOperator(operator_id, "op_sub", "operator.numeric.subtract", "operator.arithmetic.subtract")) {
    if (!use_exact_int64) {
      if (NeedsDatatypeNumericBackend(left, right)) {
        return EvaluateDatatypeNumericArithmetic(operator_id, left, right, context);
      }
      long double left_value = 0;
      long double right_value = 0;
      if (!TryLongDouble(left, &left_value) || !TryLongDouble(right, &right_value)) {
        return OperatorFailure(operator_id, context, "numeric subtract requires numeric-compatible operands");
      }
      return ScalarResult(operator_id, RealTextValue("decimal", left_value - right_value));
    }
    if (SubtractWouldOverflow(left.int64_value, right.int64_value)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.NUMERIC_OVERFLOW", "int64 subtraction overflow");
    }
    result = left.int64_value - right.int64_value;
  } else if (IsAnyOperator(operator_id, "op_mul", "operator.numeric.multiply", "operator.arithmetic.multiply")) {
    if (!use_exact_int64) {
      if (NeedsDatatypeNumericBackend(left, right)) {
        return EvaluateDatatypeNumericArithmetic(operator_id, left, right, context);
      }
      long double left_value = 0;
      long double right_value = 0;
      if (!TryLongDouble(left, &left_value) || !TryLongDouble(right, &right_value)) {
        return OperatorFailure(operator_id, context, "numeric multiply requires numeric-compatible operands");
      }
      return ScalarResult(operator_id, RealTextValue("decimal", left_value * right_value));
    }
    if (MultiplyWouldOverflow(left.int64_value, right.int64_value)) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.NUMERIC_OVERFLOW", "int64 multiplication overflow");
    }
    result = left.int64_value * right.int64_value;
  } else if (IsAnyOperator(operator_id, "op_div", "operator.numeric.divide", "operator.arithmetic.divide")) {
    if (!use_exact_int64) {
      if (NeedsDatatypeNumericBackend(left, right)) {
        return EvaluateDatatypeNumericArithmetic(operator_id, left, right, context);
      }
      long double left_value = 0;
      long double right_value = 0;
      if (!TryLongDouble(left, &left_value) || !TryLongDouble(right, &right_value)) {
        return OperatorFailure(operator_id, context, "numeric divide requires numeric-compatible operands");
      }
      if (right_value == 0.0L) {
        return OperatorDiagnosticFailure(operator_id, context, "SBLR.DIVIDE_BY_ZERO", "division by zero");
      }
      return ScalarResult(operator_id, RealTextValue("decimal", left_value / right_value));
    }
    if (right.int64_value == 0) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.DIVIDE_BY_ZERO", "division by zero");
    }
    if (left.int64_value == std::numeric_limits<std::int64_t>::min() && right.int64_value == -1) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.NUMERIC_OVERFLOW", "int64 division overflow");
    }
    result = left.int64_value / right.int64_value;
  } else if (IsAnyOperator(operator_id, "op_mod", "operator.numeric.modulo", "operator.arithmetic.modulo")) {
    if (!use_exact_int64) {
      return OperatorFailure(operator_id, context, "modulo requires exact integer operands in this implementation slice");
    }
    if (right.int64_value == 0) {
      return OperatorDiagnosticFailure(operator_id, context, "SBLR.DIVIDE_BY_ZERO", "modulo by zero");
    }
    if (left.int64_value == std::numeric_limits<std::int64_t>::min() && right.int64_value == -1) {
      result = 0;
    } else {
      result = left.int64_value % right.int64_value;
    }
  } else if (IsAnyOperator(operator_id, "op_bit_and", "operator.bitwise.and", "operator.numeric.bit_and")) {
    if (!use_exact_int64) {
      return OperatorFailure(operator_id, context, "bitwise and requires exact integer operands in this implementation slice");
    }
    result = left.int64_value & right.int64_value;
  } else if (IsAnyOperator(operator_id, "op_bit_or", "operator.bitwise.or", "operator.numeric.bit_or")) {
    if (!use_exact_int64) {
      return OperatorFailure(operator_id, context, "bitwise or requires exact integer operands in this implementation slice");
    }
    result = left.int64_value | right.int64_value;
  } else {
    return OperatorFailure(operator_id, context, "numeric operator is not registered");
  }
  return ScalarResult(operator_id, Int64Value(result));
}

SblrResult EvaluateSblrStringOperator(std::string_view operator_id,
                                      const SblrValue& left,
                                      const SblrValue& right,
                                      const SblrExecutionContext& context) {
  if (!StringOperatorIs(operator_id, "op_concat", "operator.string.concat") &&
      !StringOperatorIs(operator_id, "op_like", "operator.string.like") &&
      !StringOperatorIs(operator_id, "op_ilike", "operator.string.ilike") &&
      !StringOperatorIs(operator_id, "op_regex", "operator.string.regex", "op_regex_match") &&
      !StringOperatorIs(operator_id, "op_position", "operator.string.position") &&
      !StringOperatorIs(operator_id, "op_contains", "operator.string.contains") &&
      !StringOperatorIs(operator_id, "op_starts_with", "operator.string.starts_with") &&
      !StringOperatorIs(operator_id, "op_ends_with", "operator.string.ends_with")) {
    return OperatorFailure(operator_id, context, "string operator is not registered");
  }
  if (left.is_null || right.is_null) {
    return ScalarResult(operator_id, NullValue(StringOperatorIs(operator_id, "op_concat", "operator.string.concat") ? "text" :
