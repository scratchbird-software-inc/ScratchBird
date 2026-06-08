// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "prepared_execution_template.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_native_specialization.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrHotPathAuthorityContext {
  bool parser_sql_execution_authority = false;
  bool donor_execution_authority = false;
  bool client_execution_authority = false;
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_recheck_required = false;
  bool template_visibility_or_finality_authority = false;
  bool specialization_visibility_or_finality_authority = false;
  bool superinstruction_visibility_or_finality_authority = false;
  bool batch_visibility_or_finality_authority = false;
};

struct SblrHotPathSuperinstructionPlan {
  std::vector<std::string> fused_opcodes;
  std::string superinstruction_id;
  bool available = false;
  bool safe = false;
  bool exact_scalar_fallback_available = true;
  std::uint64_t scalar_dispatches = 0;
  std::uint64_t fused_dispatches = 0;
};

struct SblrHotPathBatchPlan {
  std::uint64_t repeated_rows = 0;
  std::uint64_t scalar_dispatches_per_row = 0;
  std::uint64_t batched_dispatches_total = 0;
  bool row_ordering_preserved = false;
  bool result_contract_hash_matches = false;
  std::string expected_result_contract_hash;
  std::string observed_result_contract_hash;
};

struct SblrHotPathProfilerEvidence {
  std::string source_label;
  bool measured = false;
  std::uint64_t sample_count = 0;
  std::uint64_t baseline_dispatch_us = 0;
  std::uint64_t optimized_dispatch_us = 0;
};

struct SblrHotPathExecutionRequest {
  std::string route_label;
  SblrOperationEnvelope envelope;
  scratchbird::engine::internal_api::EngineRequestContext context;
  scratchbird::engine::internal_api::EngineApiRequest api_request;
  scratchbird::engine::executor::PreparedTemplateCache* template_cache = nullptr;
  SblrNativeSpecializationRequest native_specialization;
  SblrHotPathSuperinstructionPlan superinstruction;
  SblrHotPathBatchPlan batch;
  SblrHotPathProfilerEvidence profiler;
  SblrHotPathAuthorityContext authority;
};

struct SblrHotPathExecutionResult {
  bool ok = false;
  bool benchmark_clean = false;
  bool fallback_used = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string detail;
  scratchbird::engine::executor::PreparedTemplatePrepareResult first_prepare;
  scratchbird::engine::executor::PreparedTemplatePrepareResult reused_prepare;
  scratchbird::engine::executor::PreparedTemplateBindResult bind;
  SblrNativeSpecializationResult specialization;
  std::uint64_t dispatch_us_saved = 0;
  std::uint64_t opcode_dispatches_saved = 0;
  std::vector<std::string> evidence;
};

SblrHotPathExecutionResult ExecuteSblrHotPath(
    const SblrHotPathExecutionRequest& request);

}  // namespace scratchbird::engine::sblr
