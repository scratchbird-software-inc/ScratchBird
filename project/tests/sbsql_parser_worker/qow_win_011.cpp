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

exec::CanonicalWindowValueRequest NthRequest(
    exec::CanonicalWindowFrameDescriptor frame = WholePartitionFrame(),
    const std::string& nth = "2") {
  auto request = ValueRequest(exec::CanonicalWindowValueFunction::nth_value,
                              std::move(frame));
  request.nth_values = RepeatedOperand(
      request.frames.ordered_batch.rows.size(), "int64", nth, 5401);
  request.nth_origin = exec::CanonicalWindowNthOrigin::from_first;
  request.null_treatment =
      exec::CanonicalWindowNullTreatment::respect_nulls;
  return request;
}

bool ValidateNthValue() {
  bool passed = true;
  auto request = NthRequest();
  auto result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"105", "105", "105", "105", "105",
                                        "<NULL>", "104", "104", "<NULL>"}) &&
          result.frame_and_exclusion_validated &&
          !result.frame_and_exclusion_ignored_for_navigation,
      "NTH_VALUE did not select the second whole-partition value");

  request = NthRequest(PrefixFrame());
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"<NULL>", "105", "105", "105", "105",
                                        "<NULL>", "<NULL>", "104", "<NULL>"}),
      "NTH_VALUE ignored the row-varying effective frame");

  const std::vector<std::string> per_row_n = {"1", "2", "3", "4", "5",
                                               "1", "1", "2", "1"};
  std::vector<api::EngineTypedValue> nth_values;
  for (std::size_t row = 0; row < per_row_n.size(); ++row) {
    nth_values.push_back(ValueOperand("int64", per_row_n[row], 5410 + row));
  }
  request.nth_values = nth_values;
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "105", "<NULL>", "100", "107",
                                        "102", "103", "104", "106"}),
      "NTH_VALUE did not evaluate n per row with RESPECT NULLS");

  request = NthRequest(
      WholePartitionFrame(exec::CanonicalWindowFrameExclusion::current_row),
      "1");
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"105", "101", "101", "101", "101",
                                        "<NULL>", "104", "103", "<NULL>"}),
      "NTH_VALUE did not consume effective exclusion membership");
  return passed;
}

bool ValidateNthValueRefusals() {
  bool passed = true;
  auto request = NthRequest();
  const auto rows = request.frames.ordered_batch.rows.size();
  request.nth_values.reset();
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NTH"}),
      "omitted NTH_VALUE n was defaulted");

  request = NthRequest(WholePartitionFrame(), "0");
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NTH"}),
      "zero NTH_VALUE n entered execution");

  request.nth_values = RepeatedOperand(rows, "int64", "-1", 5430);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NTH"}),
      "negative NTH_VALUE n entered execution");

  auto null_n = ValueOperand("int64", "1", 5431);
  null_n.setState(api::EngineValueState::sql_null);
  null_n.encoded_value.clear();
  request.nth_values = std::vector<api::EngineTypedValue>(rows, null_n);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NTH"}),
      "NULL NTH_VALUE n entered execution");

  request.nth_values = RepeatedOperand(
      rows, "int64", "9223372036854775808", 5432);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NTH"}),
      "overflowed NTH_VALUE n entered execution");

  request = NthRequest();
  request.nth_origin.reset();
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NULL-TREATMENT"}),
      "NTH_VALUE accepted omitted origin state");

  request = NthRequest();
  request.null_treatment.reset();
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NULL-TREATMENT"}),
      "NTH_VALUE accepted omitted NULL-treatment state");

  request = NthRequest();
  request.nth_origin = exec::CanonicalWindowNthOrigin::from_last;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NULL-TREATMENT"}),
      "NTH_VALUE accepted FROM LAST");

  request = NthRequest();
  request.null_treatment = exec::CanonicalWindowNullTreatment::ignore_nulls;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NULL-TREATMENT"}),
      "NTH_VALUE accepted IGNORE NULLS");

  request = NthRequest();
  request.offset_values = RepeatedOperand(rows, "int64", "1", 5433);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-NTH"}),
      "NTH_VALUE accepted a navigation offset");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-011-V1
int main() {
  return ValidateNthValue() && ValidateNthValueRefusals() ? EXIT_SUCCESS
                                                          : EXIT_FAILURE;
}
