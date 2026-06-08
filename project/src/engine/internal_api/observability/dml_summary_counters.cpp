// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/dml_summary_counters.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {

void AddDmlSummaryFallbackReason(EngineDmlSummaryCounters* counters,
                                 std::string reason) {
  if (counters == nullptr || reason.empty()) {
    return;
  }
  if (std::find(counters->fallback_reasons.begin(),
                counters->fallback_reasons.end(),
                reason) == counters->fallback_reasons.end()) {
    counters->fallback_reasons.push_back(std::move(reason));
  }
}

void AddDmlSummaryCounters(EngineDmlSummaryCounters* target,
                           const EngineDmlSummaryCounters& source) {
  if (target == nullptr) {
    return;
  }
  target->rows_changed += source.rows_changed;
  target->visible_rows_scanned += source.visible_rows_scanned;
  target->index_probes += source.index_probes;
  target->append_calls += source.append_calls;
  target->file_opens += source.file_opens;
  target->flushes += source.flushes;
  target->page_reservations += source.page_reservations;
  target->row_extent_reservations += source.row_extent_reservations;
  target->version_extent_reservations += source.version_extent_reservations;
  target->page_extent_reservations += source.page_extent_reservations;
  target->index_extent_reservations += source.index_extent_reservations;
  target->preallocation_requests += source.preallocation_requests;
  target->preallocation_granted_pages += source.preallocation_granted_pages;
  target->preallocation_capped += source.preallocation_capped;
  target->preallocation_refused += source.preallocation_refused;
  target->benchmark_clean = target->benchmark_clean && source.benchmark_clean;
  for (const auto& reason : source.fallback_reasons) {
    AddDmlSummaryFallbackReason(target, reason);
  }
}

void AddDmlSummaryEvidence(EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  const auto& counters = result->dml_summary;
  result->evidence.push_back({"dml_summary.schema_id",
                              "scratchbird.dml_summary_counters.v1"});
  result->evidence.push_back({"dml_summary.operation", result->operation_id});
  result->evidence.push_back({"dml_summary.benchmark_clean",
                              counters.benchmark_clean ? "true" : "false"});
  result->evidence.push_back({"dml_summary.rows_changed",
                              std::to_string(counters.rows_changed)});
  result->evidence.push_back({"dml_summary.visible_rows_scanned",
                              std::to_string(counters.visible_rows_scanned)});
  result->evidence.push_back({"dml_summary.index_probes",
                              std::to_string(counters.index_probes)});
  result->evidence.push_back({"dml_summary.append_calls",
                              std::to_string(counters.append_calls)});
  result->evidence.push_back({"dml_summary.file_opens",
                              std::to_string(counters.file_opens)});
  result->evidence.push_back({"dml_summary.flushes",
                              std::to_string(counters.flushes)});
  result->evidence.push_back({"dml_summary.page_reservations",
                              std::to_string(counters.page_reservations)});
  result->evidence.push_back({"dml_summary.row_extent_reservations",
                              std::to_string(counters.row_extent_reservations)});
  result->evidence.push_back({"dml_summary.version_extent_reservations",
                              std::to_string(counters.version_extent_reservations)});
  result->evidence.push_back({"dml_summary.page_extent_reservations",
                              std::to_string(counters.page_extent_reservations)});
  result->evidence.push_back({"dml_summary.index_extent_reservations",
                              std::to_string(counters.index_extent_reservations)});
  result->evidence.push_back({"dml_summary.preallocation_requests",
                              std::to_string(counters.preallocation_requests)});
  result->evidence.push_back({"dml_summary.preallocation_granted_pages",
                              std::to_string(counters.preallocation_granted_pages)});
  result->evidence.push_back({"dml_summary.preallocation_capped",
                              std::to_string(counters.preallocation_capped)});
  result->evidence.push_back({"dml_summary.preallocation_refused",
                              std::to_string(counters.preallocation_refused)});
  result->evidence.push_back({"dml_summary.fallback_reason_count",
                              std::to_string(counters.fallback_reasons.size())});
  if (counters.fallback_reasons.empty()) {
    result->evidence.push_back({"dml_summary.fallback_reason", "none"});
    return;
  }
  for (const auto& reason : counters.fallback_reasons) {
    result->evidence.push_back({"dml_summary.fallback_reason", reason});
  }
}

}  // namespace scratchbird::engine::internal_api
