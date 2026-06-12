// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "candidate_set.hpp"
#include "runtime_filter_pushdown.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

struct RuntimeFilterProviderRequest {
  scratchbird::engine::optimizer::RuntimeFilterDescriptor descriptor;
  scratchbird::core::index::CandidateSetAuthorityContext authority;
};

struct RuntimeFilterProviderResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  bool unsupported = false;
  bool returns_final_rows = false;
  bool exact_recheck_evidence_present = false;
  bool mga_recheck_evidence_present = false;
  bool security_recheck_evidence_present = false;
  bool parser_or_reference_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool write_ahead_log_finality_or_visibility_authority = false;
  std::vector<scratchbird::core::index::CandidateSetRow> candidate_rows;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

using RuntimeFilterProvider =
    std::function<RuntimeFilterProviderResult(
        const RuntimeFilterProviderRequest&)>;

struct RuntimeFilterProviderSet {
  RuntimeFilterProvider scan_provider;
  RuntimeFilterProvider physical_provider;
  RuntimeFilterProvider exact_fallback_provider;
};

struct RuntimeFilterCounters {
  std::uint64_t input_rows = 0;
  std::uint64_t candidate_rows = 0;
  std::uint64_t pruned_rows = 0;
  std::uint64_t pushed_filter_count = 0;
  std::uint64_t fallback_count = 0;
  std::uint64_t exact_recheck_count = 0;
};

struct RuntimeFilterExecutionResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  std::string diagnostic_code;
  RuntimeFilterCounters counters;
  scratchbird::core::index::CandidateSet candidate_rows;
  std::vector<scratchbird::core::platform::TypedUuid> final_row_uuids;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

RuntimeFilterExecutionResult ExecuteRuntimeFilterPushdown(
    const std::vector<scratchbird::engine::optimizer::RuntimeFilterDescriptor>&
        filters,
    const scratchbird::core::index::CandidateSetAuthorityContext& authority,
    const RuntimeFilterProviderSet& providers);

RuntimeFilterProviderResult MakeUnsupportedRuntimeFilterProviderResult(
    std::string evidence);

}  // namespace scratchbird::engine::executor
