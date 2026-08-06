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

bool ValidateLead() {
  bool passed = true;
  auto request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  auto result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"105", "<NULL>", "100", "107",
                                        "<NULL>", "<NULL>", "104", "<NULL>",
                                        "<NULL>"}) &&
          result.frame_and_exclusion_validated &&
          result.frame_and_exclusion_ignored_for_navigation &&
          result.function_uuid == ValueFunctionUuid(
              exec::CanonicalWindowValueFunction::lead) &&
          result.resolved_positions ==
              std::vector<std::uint64_t>(9, 1) &&
          result.converted_source_value_count == 9 &&
          result.converted_default_value_count == 0 &&
          result.used_implicit_navigation_offset &&
          !result.explicit_navigation_default_present &&
          result.every_function_operand_consumed &&
          result.partition_metadata_consumed_for_navigation &&
          result.partition_property_uuid ==
              request.frames.partition_property_uuid &&
          result.ordering_property_uuid ==
              request.frames.ordering_property_uuid &&
          result.frame_property_binding_evidence_uuid ==
              request.frames.frame_property_binding_evidence_uuid &&
          ValueDescriptorsMatch(result, request.result_column.descriptor),
      "LEAD default offset or partition-boundary behavior drifted");

  request = ValueRequest(
      exec::CanonicalWindowValueFunction::lead,
      WholePartitionFrame(exec::CanonicalWindowFrameExclusion::current_row));
  const auto excluded = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      excluded.diagnostic.ok && ValueTexts(excluded) == ValueTexts(result) &&
          request.frames.effective_frames[0].effective_row_indices.size() == 4,
      "LEAD used frame exclusion after validating it");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  const auto rows = request.frames.ordered_batch.rows.size();
  request.offset_values = RepeatedOperand(rows, "int64", "0", 5101);
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "105", "<NULL>", "100", "107",
                                        "102", "103", "104", "106"}),
      "LEAD offset zero did not select the current row");

  const std::vector<std::string> per_row_offsets = {
      "4", "3", "2", "1", "0", "0", "1", "0", "0"};
  std::vector<api::EngineTypedValue> offsets;
  for (std::size_t row = 0; row < per_row_offsets.size(); ++row) {
    offsets.push_back(
        ValueOperand("int64", per_row_offsets[row], 5103 + row));
  }
  request.offset_values = offsets;
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"107", "107", "107", "107", "107",
                                        "102", "104", "104", "106"}),
      "LEAD did not evaluate its offset independently per row");

  request.offset_values = RepeatedOperand(rows, "int64", "2", 5102);
  std::vector<api::EngineTypedValue> defaults;
  for (std::size_t row = 0; row < rows; ++row) {
    defaults.push_back(
        ValueOperand("int32",
                     std::to_string(-20 - static_cast<std::int64_t>(row)),
                     5110 + row));
  }
  request.default_values = defaults;
  result = exec::ExecuteCanonicalWindowValue(request);
  if (!result.diagnostic.ok) {
    std::cerr << "QOW-TEST-WIN-008-V1: "
              << result.diagnostic.diagnostic_code << ": "
              << result.diagnostic.detail << '\n';
  }
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"<NULL>", "100", "107", "-23", "-24",
                                        "-25", "-26", "-27", "-28"}) &&
          result.resolved_positions ==
              std::vector<std::uint64_t>(9, 2) &&
          result.converted_default_value_count == 9 &&
          !result.used_implicit_navigation_offset &&
          result.explicit_navigation_default_present &&
          ValueDescriptorsMatch(result, request.result_column.descriptor),
      "LEAD did not evaluate typed offset/default operands per current row");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  request.offset_values = RepeatedOperand(
      rows, "int64", "9223372036854775807", 5125);
  defaults.clear();
  for (std::size_t row = 0; row < rows; ++row) {
    defaults.push_back(
        ValueOperand("int32",
                     std::to_string(-30 - static_cast<std::int64_t>(row)),
                     5140 + row));
  }
  request.default_values = defaults;
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"-30", "-31", "-32", "-33", "-34",
                                        "-35", "-36", "-37", "-38"}),
      "maximum in-range LEAD offset did not select each current-row default");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lead,
                         WholePartitionFrame(), true);
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok && result.values.empty() &&
          result.resolved_positions.empty() &&
          result.converted_source_value_count == 0 &&
          result.used_implicit_navigation_offset &&
          !result.explicit_navigation_default_present &&
          result.every_function_operand_consumed,
      "empty LEAD input did not preserve omitted navigation operand state");
  return passed;
}

bool ValidateLeadRefusals() {
  bool passed = true;
  auto request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  const auto rows = request.frames.ordered_batch.rows.size();
  request.offset_values = RepeatedOperand(rows - 1, "int64", "1", 5130);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "short LEAD offset vector entered execution");

  request.offset_values = RepeatedOperand(rows, "int32", "1", 5131);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "non-int64 LEAD offset entered execution");

  request.offset_values = RepeatedOperand(rows, "int64", "-1", 5132);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "negative LEAD offset entered execution");

  request.offset_values = RepeatedOperand(
      rows, "int64", "9223372036854775808", 5133);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "overflowed LEAD offset entered execution");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  request.offset_values = RepeatedOperand(rows, "int64", "1", 5134);
  request.default_values = RepeatedOperand(rows, "text", "not-an-int", 5135);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-DEFAULT-TYPE"}),
      "incompatible LEAD default entered execution");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  request.offset_values = RepeatedOperand(rows, "int64", "1", 5137);
  request.default_values =
      RepeatedOperand(rows - 1, "int32", "7", 5138);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-DEFAULT-TYPE"}),
      "short LEAD default vector entered execution");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  request.nth_values = RepeatedOperand(rows, "int64", "1", 5136);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "LEAD accepted an NTH_VALUE operand");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lead);
  request.transaction_finality_claimed = true;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-AUTHORITY"}),
      "LEAD claimed transaction finality");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-008-V1
int main() {
  return ValidateLead() && ValidateLeadRefusals() ? EXIT_SUCCESS
                                                  : EXIT_FAILURE;
}
