// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TOAST-PAGE-ANCHOR
#include "page_layout.hpp"
#include "runtime_platform.hpp"

#include <string>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

struct ToastValueReference {
  TypedUuid value_uuid;
  u64 total_bytes = 0;
  u32 chunk_count = 0;
  u32 chunk_payload_bytes = 0;
  u64 first_page_number = 0;
  std::string content_hash;
};

struct ToastCapacityPlan {
  u32 page_size = 0;
  u32 body_payload_bytes = 0;
  u32 chunk_payload_bytes = 0;
  u32 chunk_count = 0;
  u64 total_bytes = 0;
};

struct ToastPlanResult {
  Status status;
  ToastCapacityPlan plan;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

ToastPlanResult PlanToastValue(u64 total_bytes, u32 page_size);
DiagnosticRecord MakeToastPageDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {});

}  // namespace scratchbird::storage::page
