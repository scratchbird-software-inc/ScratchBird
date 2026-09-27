// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_special_forms.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.

SblrResult EvaluateSblrCoalesceForm(std::string_view operation_id, const std::vector<SblrValue>& values) {
  SblrResult out = MakeSblrSuccess(std::string(operation_id));
  for (const auto& value : values) {
    if (!value.is_null) {
      out.scalar_values.push_back(value);
      return out;
    }
  }
  out.scalar_values.push_back(NullValue());
  return out;
}

SblrResult EvaluateSblrCoalesceFormLazy(std::string_view operation_id,
                                        const std::vector<SblrExpressionThunk>& values,
                                        const SblrExecutionContext& context) {
  for (const auto& thunk : values) {
    SblrValue value;
    const auto result = EvaluateThunkScalar(operation_id, context, thunk, &value);
    if (!result.ok()) return result;
    if (!value.is_null) return ScalarResult(operation_id, std::move(value));
  }
  return ScalarResult(operation_id, NullValue());
}

SblrResult EvaluateSblrNullIfForm(std::string_view operation_id, const SblrValue& left, const SblrValue& right) {
  if (!NativeComparisonValid(left, right))
    return SpecialFormFailure(operation_id, {}, "SBLR.DESCRIPTOR_MISMATCH", "NULLIF requires matching native binary carriers");
  SblrResult out = MakeSblrSuccess(std::string(operation_id));
  if (ValuesEqual(left, right)) {
    out.scalar_values.push_back(NullValue(left.descriptor_id));
  } else {
    out.scalar_values.push_back(left);
  }
  return out;
}

SblrResult EvaluateSblrExistsForm(std::string_view operation_id, bool row_exists) {
  return ScalarResult(operation_id, MakeSblrTruthValue(row_exists ? SblrTruthValue::true_value : SblrTruthValue::false_value));
}

SblrResult EvaluateSblrInListForm(std::string_view operation_id,
                                  const SblrValue& probe,
                                  const std::vector<SblrValue>& candidates) {
  if (!NativeComparisonValid(probe, probe))
    return SpecialFormFailure(operation_id, {}, "SBLR.DESCRIPTOR_MISMATCH", "IN probe has an invalid native carrier");
  if (probe.is_null) {
    return ScalarResult(operation_id, MakeSblrTruthValue(SblrTruthValue::unknown));
  }
  bool saw_null = false;
  for (const auto& candidate : candidates) {
    if (!NativeComparisonValid(probe, candidate))
      return SpecialFormFailure(operation_id, {}, "SBLR.DESCRIPTOR_MISMATCH", "IN requires matching native binary carriers");
    if (candidate.is_null) {
      saw_null = true;
      continue;
    }
    if (ValuesEqual(probe, candidate)) {
      return ScalarResult(operation_id, MakeSblrTruthValue(SblrTruthValue::true_value));
    }
  }
  return ScalarResult(operation_id, MakeSblrTruthValue(saw_null ? SblrTruthValue::unknown : SblrTruthValue::false_value));
}

SblrResult EvaluateSblrInListFormLazy(std::string_view operation_id,
                                      SblrExpressionThunk probe,
                                      const std::vector<SblrExpressionThunk>& candidates,
                                      const SblrExecutionContext& context) {
  SblrValue probe_value;
  const auto probe_result = EvaluateThunkScalar(operation_id, context, probe, &probe_value);
  if (!probe_result.ok()) return probe_result;
  if (!NativeComparisonValid(probe_value, probe_value))
    return SpecialFormFailure(operation_id, context, "SBLR.DESCRIPTOR_MISMATCH", "IN probe has an invalid native carrier");
  if (probe_value.is_null) {
    return ScalarResult(operation_id, MakeSblrTruthValue(SblrTruthValue::unknown));
  }
  bool saw_null = false;
  for (const auto& candidate_thunk : candidates) {
    SblrValue candidate;
    const auto candidate_result = EvaluateThunkScalar(operation_id, context, candidate_thunk, &candidate);
    if (!candidate_result.ok()) return candidate_result;
    if (!NativeComparisonValid(probe_value, candidate))
      return SpecialFormFailure(operation_id, context, "SBLR.DESCRIPTOR_MISMATCH", "IN requires matching native binary carriers");
    if (candidate.is_null) {
      saw_null = true;
      continue;
    }
    if (ValuesEqual(probe_value, candidate)) {
      return ScalarResult(operation_id, MakeSblrTruthValue(SblrTruthValue::true_value));
    }
  }
  return ScalarResult(operation_id, MakeSblrTruthValue(saw_null ? SblrTruthValue::unknown : SblrTruthValue::false_value));
}

SblrResult EvaluateSblrBetweenForm(std::string_view operation_id,
                                   const SblrValue& value,
                                   const SblrValue& lower,
                                   const SblrValue& upper) {
  if (!NativeComparisonValid(value, lower) || !NativeComparisonValid(value, upper))
    return SpecialFormFailure(operation_id, {}, "SBLR.DESCRIPTOR_MISMATCH", "BETWEEN requires matching native binary carriers");
  if (value.is_null || lower.is_null || upper.is_null) {
    return ScalarResult(operation_id, MakeSblrTruthValue(SblrTruthValue::unknown));
  }
  const bool inside = CompareValues(value, lower) >= 0 && CompareValues(value, upper) <= 0;
  return ScalarResult(operation_id, MakeSblrTruthValue(inside ? SblrTruthValue::true_value : SblrTruthValue::false_value));
}

SblrResult EvaluateSblrBetweenFormLazy(std::string_view operation_id,
                                       SblrExpressionThunk value,
                                       SblrExpressionThunk lower,
                                       SblrExpressionThunk upper,
                                       const SblrExecutionContext& context) {
  SblrValue value_scalar;
  SblrValue lower_scalar;
  SblrValue upper_scalar;
  auto result = EvaluateThunkScalar(operation_id, context, value, &value_scalar);
  if (!result.ok()) return result;
  result = EvaluateThunkScalar(operation_id, context, lower, &lower_scalar);
  if (!result.ok()) return result;
  result = EvaluateThunkScalar(operation_id, context, upper, &upper_scalar);
  if (!result.ok()) return result;
  return EvaluateSblrBetweenForm(operation_id, value_scalar, lower_scalar, upper_scalar);
}

SblrResult EvaluateSblrSubstringForm(std::string_view operation_id,
                                     const SblrValue& value,
