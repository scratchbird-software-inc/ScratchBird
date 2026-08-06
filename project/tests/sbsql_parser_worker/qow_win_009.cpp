// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_007_FIXTURE_ONLY
#include "qow_win_007.cpp"

namespace {

exec::CanonicalWindowFrameDescriptor OnePrecedingFrame(
    const exec::CanonicalWindowFrameExclusion exclusion =
        exec::CanonicalWindowFrameExclusion::no_others) {
  return ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 exec::EncodeInt64Value(1)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row), exclusion);
}

exec::CanonicalWindowFrameDescriptor CurrentRowFrame(
    const exec::CanonicalWindowFrameExclusion exclusion =
        exec::CanonicalWindowFrameExclusion::no_others) {
  return ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row), exclusion);
}

bool ValidateFirstValue() {
  bool passed = true;
  auto request =
      ValueRequest(exec::CanonicalWindowValueFunction::first_value);
  auto result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "101", "101", "101", "101",
                                        "102", "103", "103", "106"}) &&
          result.frame_and_exclusion_validated &&
          !result.frame_and_exclusion_ignored_for_navigation &&
          result.function_uuid == ValueFunctionUuid(
              exec::CanonicalWindowValueFunction::first_value) &&
          result.resolved_positions.empty() &&
          !result.resolved_nth_origin.has_value() &&
          !result.resolved_null_treatment.has_value() &&
          result.converted_source_value_count == 9 &&
          result.every_function_operand_consumed &&
          !result.partition_metadata_consumed_for_navigation &&
          result.effective_frame_membership_consumed &&
          result.frame_property_binding_evidence_uuid ==
              request.frames.frame_property_binding_evidence_uuid &&
          ValueDescriptorsMatch(result, request.result_column.descriptor),
      "FIRST_VALUE did not select the first effective partition row");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value,
                         OnePrecedingFrame());
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "101", "105", "<NULL>", "100",
                                        "102", "103", "103", "106"}),
      "FIRST_VALUE ignored the row-varying effective frame");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value,
                         CurrentRowFrame());
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "105", "<NULL>", "100", "107",
                                        "102", "103", "104", "106"}),
      "FIRST_VALUE did not retain a typed NULL source value");

  request = ValueRequest(
      exec::CanonicalWindowValueFunction::first_value,
      PrefixFrame(exec::CanonicalWindowFrameExclusion::current_row));
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"<NULL>", "101", "101", "101", "101",
                                        "<NULL>", "<NULL>", "103", "<NULL>"}),
      "FIRST_VALUE did not apply exclusion or type empty-frame NULL");

  request = ValueRequest(
      exec::CanonicalWindowValueFunction::first_value,
      WholePartitionFrame(exec::CanonicalWindowFrameExclusion::group));
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"<NULL>", "<NULL>", "101", "101",
                                        "101", "<NULL>", "<NULL>",
                                        "<NULL>", "<NULL>"}),
      "FIRST_VALUE did not select from post-GROUP-exclusion membership");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value,
                         WholePartitionFrame(), true);
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok && result.values.empty() &&
          result.converted_source_value_count == 0 &&
          result.effective_frame_membership_consumed &&
          result.every_function_operand_consumed,
      "empty FIRST_VALUE input did not complete as a typed zero-row result");
  return passed;
}

bool ValidateFirstValueRefusals() {
  bool passed = true;
  auto request =
      ValueRequest(exec::CanonicalWindowValueFunction::first_value);
  const auto rows = request.frames.ordered_batch.rows.size();
  request.offset_values = RepeatedOperand(rows, "int64", "1", 5201);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FRAME"}),
      "FIRST_VALUE accepted a navigation offset");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value);
  request.result_column.nullable = false;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR"}),
      "FIRST_VALUE accepted a descriptor unable to represent empty-frame NULL");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value);
  request.null_treatment =
      exec::CanonicalWindowNullTreatment::respect_nulls;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FRAME"}),
      "FIRST_VALUE accepted an unrelated NULL-treatment operand");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value);
  request.frames.effective_frames[0].effective_row_indices = {1, 0};
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FRAME"}),
      "FIRST_VALUE accepted forged effective-frame ordering");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value);
  request.function_uuid =
      ValueFunctionUuid(exec::CanonicalWindowValueFunction::last_value);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR"}),
      "FIRST_VALUE function UUID drift selected LAST_VALUE");

  request = ValueRequest(exec::CanonicalWindowValueFunction::first_value);
  request.recovery_authority_claimed = true;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-AUTHORITY"}),
      "FIRST_VALUE claimed recovery authority");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-009-V1
int main() {
  return ValidateFirstValue() && ValidateFirstValueRefusals() ? EXIT_SUCCESS
                                                              : EXIT_FAILURE;
}
