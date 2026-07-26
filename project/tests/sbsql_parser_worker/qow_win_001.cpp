// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_002_FIXTURE_ONLY
#include "qow_win_002.cpp"

#include "query/plan_api.hpp"

namespace {

bool DescriptorRefused(exec::CanonicalWindowRuntimeRequest request) {
  const auto result = exec::ExecuteCanonicalWindowRuntime(request);
  return !result.diagnostic.ok &&
         result.diagnostic.diagnostic_code ==
             "QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR" &&
         result.values.empty() &&
         result.executed_strategy ==
             exec::CanonicalWindowRuntimeStrategy::unknown &&
         !result.every_descriptor_field_consumed &&
         !result.retained_strategy_reached;
}

bool ValidateMissingAndUnknownRefusal() {
  exec::CanonicalWindowRuntimeRequest request;
  bool passed = Require401(DescriptorRefused(request),
                           "missing window descriptor selected a default");

  request.ranking = RuntimeRankingRequest(
      exec::CanonicalWindowRankingFunction::row_number,
      exec::CanonicalWindowRuntimeFunction::row_number);
  request.descriptor.function =
      exec::CanonicalWindowRuntimeFunction::row_number;
  passed &= Require401(DescriptorRefused(request),
                       "partial window descriptor selected ROW_NUMBER");

  request.descriptor =
      RuntimeDescriptor(exec::CanonicalWindowRuntimeFunction::row_number);
  request.descriptor.builtin_id = "sb.window.unknown";
  passed &= Require401(DescriptorRefused(request),
                       "unknown builtin id selected ROW_NUMBER");

  request.descriptor =
      RuntimeDescriptor(exec::CanonicalWindowRuntimeFunction::row_number);
  request.descriptor.function_uuid =
      RuntimeDescriptor(exec::CanonicalWindowRuntimeFunction::rank)
          .function_uuid;
  passed &= Require401(DescriptorRefused(request),
                       "mismatched UUID selected ROW_NUMBER");

  request.descriptor =
      RuntimeDescriptor(exec::CanonicalWindowRuntimeFunction::row_number);
  request.descriptor.abi_version = 2;
  passed &= Require401(DescriptorRefused(request),
                       "unknown window ABI version entered execution");

  api::EnginePlanOperationRequest legacy;
  passed &= Require401(legacy.window_function.empty() && legacy.window_n == 0,
                       "legacy request ABI still defaults window state");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-001-V1
int main() {
  return ValidateMissingAndUnknownRefusal() ? EXIT_SUCCESS : EXIT_FAILURE;
}
