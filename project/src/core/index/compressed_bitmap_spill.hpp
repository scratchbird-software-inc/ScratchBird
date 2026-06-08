// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "candidate_set.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

enum class CompressedBitmapSpillClassification : u32 {
  clean_reopen = 1,
  repaired_reopen = 2,
  truncated = 3,
  corrupt = 4,
  stale = 5,
  repair_refused = 6
};

enum class CompressedBitmapPopcountBackendKind : u32 {
  scalar = 1,
  avx512_vpopcntdq = 2,
  arm_neon_vcnt = 3
};

struct CompressedBitmapSpillDescriptor {
  u64 spill_epoch = 0;
  u64 source_generation = 0;
};

struct CompressedBitmapRepairAdmission {
  bool repair_admitted = false;
  bool descriptor_match_proven = false;
  bool authoritative_rebuild_input_proven = false;
  u64 admitted_spill_epoch = 0;
  u64 admitted_source_generation = 0;
  std::string proof_detail;
};

struct CompressedBitmapPopcountResult {
  Status status;
  bool fail_closed = false;
  u64 cardinality = 0;
  CompressedBitmapPopcountBackendKind backend =
      CompressedBitmapPopcountBackendKind::scalar;
  bool simd_available = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct CompressedBitmapSpillResult {
  Status status;
  bool fail_closed = false;
  CompressedBitmapSpillClassification classification =
      CompressedBitmapSpillClassification::corrupt;
  CandidateSet output;
  std::vector<byte> artifact;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* CompressedBitmapSpillClassificationName(
    CompressedBitmapSpillClassification classification);
const char* CompressedBitmapPopcountBackendName(
    CompressedBitmapPopcountBackendKind backend);

CompressedBitmapPopcountResult CountCompressedBitmapWithSelectedBackend(
    const CandidateSet& set);

CompressedBitmapSpillResult SerializeCompressedBitmapSpill(
    const CandidateSet& set,
    CompressedBitmapSpillDescriptor descriptor);

CompressedBitmapSpillResult OpenCompressedBitmapSpill(
    const std::vector<byte>& artifact,
    CompressedBitmapSpillDescriptor expected_descriptor,
    const CandidateSetAuthorityContext& authority);

CompressedBitmapSpillResult RepairOrOpenCompressedBitmapSpill(
    const std::vector<byte>& artifact,
    CompressedBitmapSpillDescriptor expected_descriptor,
    const CandidateSetAuthorityContext& authority,
    const CandidateSet* authoritative_rebuild_input,
    const CompressedBitmapRepairAdmission& admission);

}  // namespace scratchbird::core::index
