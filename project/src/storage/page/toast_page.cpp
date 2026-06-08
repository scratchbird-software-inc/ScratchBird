// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "toast_page.hpp"

#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::disk::PageType;

Status ToastOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status ToastErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

ToastPlanResult ToastError(std::string diagnostic_code,
                           std::string message_key,
                           std::string detail = {}) {
  ToastPlanResult result;
  result.status = ToastErrorStatus();
  result.diagnostic = MakeToastPageDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

}  // namespace

ToastPlanResult PlanToastValue(u64 total_bytes, u32 page_size) {
  PageLayoutResult capacity = ComputePageLayoutCapacity(PageType::blob, page_size);
  if (!capacity.ok()) {
    ToastPlanResult result;
    result.status = capacity.status;
    result.diagnostic = capacity.diagnostic;
    return result;
  }
  if (capacity.capacity.usable_payload_bytes <= 32) {
    return ToastError("SB-TOAST-PAGE-CAPACITY-TOO-SMALL",
                      "storage.toast_page.capacity_too_small",
                      std::to_string(page_size));
  }

  ToastCapacityPlan plan;
  plan.page_size = page_size;
  plan.body_payload_bytes = capacity.capacity.usable_payload_bytes;
  plan.chunk_payload_bytes = capacity.capacity.usable_payload_bytes - 32;
  plan.total_bytes = total_bytes;
  plan.chunk_count = total_bytes == 0
                         ? 0
                         : static_cast<u32>((total_bytes + plan.chunk_payload_bytes - 1) / plan.chunk_payload_bytes);

  ToastPlanResult result;
  result.status = ToastOkStatus();
  result.plan = plan;
  return result;
}

DiagnosticRecord MakeToastPageDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.toast");
}

}  // namespace scratchbird::storage::page
