// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-EXECUTOR-VECTORIZED-RESULT-BATCH-ANCHOR
#include "runtime_platform.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::memory {
class ReservationBackedMemoryResource;
}

namespace scratchbird::engine::executor {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class ResultBatchLayoutKind : u32 {
  unknown = 0,
  fixed_width = 1,
  variable_width = 2,
  dictionary = 3,
  run_end = 4,
  struct_view = 5,
  list_view = 6
};

struct ResultBatchColumn {
  std::string name;
  ResultBatchLayoutKind layout = ResultBatchLayoutKind::unknown;
  u64 row_count = 0;
  std::vector<std::uint8_t> validity_bitmap;
  std::vector<std::uint8_t> redaction_bitmap;

  u64 fixed_width_bytes = 0;
  std::vector<std::uint8_t> fixed_width_data;

  std::vector<u64> offsets;
  std::vector<std::uint8_t> variable_data;

  std::vector<u32> dictionary_ids;
  std::vector<std::string> dictionary_values;

  std::vector<u64> run_ends;
  std::vector<std::string> run_values;

  std::vector<ResultBatchColumn> children;
};

struct ResultBatchColumnDiagnostics {
  std::string name;
  ResultBatchLayoutKind layout = ResultBatchLayoutKind::unknown;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 redaction_count = 0;
  u64 encoded_byte_count = 0;
  u64 validity_byte_count = 0;
  u64 redaction_byte_count = 0;
  u64 dictionary_value_count = 0;
  u64 run_count = 0;
  u64 child_count = 0;
  u64 list_value_count = 0;
};

struct VectorizedResultBatch {
  u64 row_count = 0;
  std::vector<ResultBatchColumn> columns;
  std::vector<ResultBatchColumnDiagnostics> column_diagnostics;
  std::vector<std::string> evidence;
};

struct VectorizedResultBatchResult {
  Status status;
  bool fail_closed = false;
  VectorizedResultBatch batch;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

class VectorizedResultBatchBuilder {
 public:
  explicit VectorizedResultBatchBuilder(u64 row_count);

  u64 row_count() const { return row_count_; }

  void AddColumn(ResultBatchColumn column);
  VectorizedResultBatchResult Finalize() const;

 private:
  u64 row_count_ = 0;
  std::vector<ResultBatchColumn> columns_;
};

const char* ResultBatchLayoutKindName(ResultBatchLayoutKind layout);

std::vector<std::uint8_t> MakeResultBatchValidityBitmap(
    u64 row_count,
    const std::vector<u64>& null_ordinals = {});

std::vector<std::uint8_t> MakeResultBatchRedactionBitmap(
    u64 row_count,
    const std::vector<u64>& redacted_ordinals = {});

ResultBatchColumn MakeFixedWidthResultBatchColumn(
    std::string name,
    u64 row_count,
    u64 fixed_width_bytes,
    std::vector<std::uint8_t> data,
    std::vector<std::uint8_t> validity_bitmap);

ResultBatchColumn MakeVariableWidthResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u64> offsets,
    std::vector<std::uint8_t> data,
    std::vector<std::uint8_t> validity_bitmap);

ResultBatchColumn MakeDictionaryResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u32> dictionary_ids,
    std::vector<std::string> dictionary_values,
    std::vector<std::uint8_t> validity_bitmap);

ResultBatchColumn MakeRunEndResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u64> run_ends,
    std::vector<std::string> run_values,
    std::vector<std::uint8_t> validity_bitmap);

ResultBatchColumn MakeStructViewResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<ResultBatchColumn> children,
    std::vector<std::uint8_t> validity_bitmap);

ResultBatchColumn MakeListViewResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u64> offsets,
    ResultBatchColumn child,
    std::vector<std::uint8_t> validity_bitmap);

VectorizedResultBatchResult FinalizeVectorizedResultBatch(
    u64 row_count,
    std::vector<ResultBatchColumn> columns);
VectorizedResultBatchResult FinalizeVectorizedResultBatchFromReservedResource(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    u64 row_count,
    std::vector<ResultBatchColumn> columns,
    bool engine_mga_authoritative,
    bool parser_or_donor_authority,
    bool memory_finality_or_visibility_authority,
    bool debug_or_relaxed_path);

DiagnosticRecord MakeVectorizedResultBatchDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::engine::executor
