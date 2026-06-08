// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: ODF038_DML_BENCHMARK_CLEAN_SUMMARY_COUNTERS
// Compact DML counters for benchmark-clean result/evidence surfaces. These
// counters are observability-only and do not own transaction finality,
// visibility, parser execution, or storage mutation authority.

void AddDmlSummaryFallbackReason(EngineDmlSummaryCounters* counters,
                                 std::string reason);
void AddDmlSummaryCounters(EngineDmlSummaryCounters* target,
                           const EngineDmlSummaryCounters& source);
void AddDmlSummaryEvidence(EngineApiResult* result);

}  // namespace scratchbird::engine::internal_api
