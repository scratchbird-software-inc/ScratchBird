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

bool ValidateLastValue() {
  bool passed = true;
  auto request = ValueRequest(exec::CanonicalWindowValueFunction::last_value);
  auto result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"107", "107", "107", "107", "107",
                                        "102", "104", "104", "106"}) &&
          result.frame_and_exclusion_validated &&
          !result.frame_and_exclusion_ignored_for_navigation &&
          result.function_uuid == ValueFunctionUuid(
              exec::CanonicalWindowValueFunction::last_value) &&
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
      "LAST_VALUE did not select the last effective partition row");

  request = ValueRequest(exec::CanonicalWindowValueFunction::last_value,
                         PrefixFrame());
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "105", "<NULL>", "100", "107",
                                        "102", "103", "104", "106"}),
      "LAST_VALUE did not vary with the prefix frame or retain source NULL");

  request = ValueRequest(
      exec::CanonicalWindowValueFunction::last_value,
      PrefixFrame(exec::CanonicalWindowFrameExclusion::current_row));
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"<NULL>", "101", "105", "<NULL>",
                                        "100", "<NULL>", "<NULL>", "103",
                                        "<NULL>"}),
      "LAST_VALUE did not apply exclusion or type empty-frame NULL");

  request = ValueRequest(
      exec::CanonicalWindowValueFunction::last_value,
      WholePartitionFrame(exec::CanonicalWindowFrameExclusion::group));
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"107", "107", "107", "107", "100",
                                        "<NULL>", "<NULL>", "<NULL>",
                                        "<NULL>"}),
      "LAST_VALUE did not select from post-GROUP-exclusion membership");

  request = ValueRequest(exec::CanonicalWindowValueFunction::last_value,
                         WholePartitionFrame(), true);
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok && result.values.empty() &&
          result.converted_source_value_count == 0 &&
          result.effective_frame_membership_consumed &&
          result.every_function_operand_consumed,
      "empty LAST_VALUE input did not complete as a typed zero-row result");
  return passed;
}

bool ValidateLastValueRefusals() {
  bool passed = true;
  auto request = ValueRequest(exec::CanonicalWindowValueFunction::last_value);
  const auto rows = request.frames.ordered_batch.rows.size();
  request.default_values = RepeatedOperand(rows, "int64", "7", 5301);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FRAME"}),
      "LAST_VALUE accepted a navigation default");

  request = ValueRequest(exec::CanonicalWindowValueFunction::last_value);
  request.value_expression_descriptor_id = 999999;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR"}),
      "LAST_VALUE accepted an unresolved value descriptor handle");

  request = ValueRequest(exec::CanonicalWindowValueFunction::last_value);
  request.frames.effective_frames[1].effective_row_indices.push_back(8);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FRAME"}),
      "LAST_VALUE accepted an out-of-frame effective member");

  request = ValueRequest(exec::CanonicalWindowValueFunction::last_value);
  request.function_uuid =
      ValueFunctionUuid(exec::CanonicalWindowValueFunction::first_value);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR"}),
      "LAST_VALUE function UUID drift selected FIRST_VALUE");

  request = ValueRequest(exec::CanonicalWindowValueFunction::last_value);
  request.transaction_finality_claimed = true;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-AUTHORITY"}),
      "LAST_VALUE claimed transaction finality");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-010-V1
int main() {
  return ValidateLastValue() && ValidateLastValueRefusals() ? EXIT_SUCCESS
                                                            : EXIT_FAILURE;
}
