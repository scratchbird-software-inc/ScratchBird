// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-WIRE-RESULT-BATCH-TRANSFER-ANCHOR
#include "vectorized_result_batch.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::wire {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::engine::executor::ResultBatchLayoutKind;
using scratchbird::engine::executor::VectorizedResultBatch;

inline constexpr u32 kResultBatchTransferDescriptorVersion = 1;

struct ResultBatchColumnTransferDescriptor {
  std::string name;
  ResultBatchLayoutKind layout = ResultBatchLayoutKind::unknown;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 encoded_byte_count = 0;
  u64 validity_byte_count = 0;
  u64 fixed_width_bytes = 0;
  u64 offset_count = 0;
  u64 dictionary_value_count = 0;
  u64 run_count = 0;
  u64 child_count = 0;
  u64 list_value_count = 0;
  std::vector<ResultBatchColumnTransferDescriptor> children;
};

struct ResultBatchTransferDescriptor {
  u32 version = kResultBatchTransferDescriptorVersion;
  u64 row_count = 0;
  u64 column_count = 0;
  u64 encoded_byte_count = 0;
  std::vector<ResultBatchColumnTransferDescriptor> columns;
};

struct ResultBatchTransferResult {
  Status status;
  bool fail_closed = false;
  ResultBatchTransferDescriptor descriptor;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

ResultBatchTransferResult BuildResultBatchTransferDescriptor(
    const VectorizedResultBatch& batch);

std::vector<std::uint8_t> SerializeResultBatchTransferDescriptor(
    const ResultBatchTransferDescriptor& descriptor);

const char* ResultBatchTransferDescriptorFieldVersion();

}  // namespace scratchbird::wire
