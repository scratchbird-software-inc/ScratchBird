// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_family_benchmark_gate.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "index_family_benchmark_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

const std::vector<std::string>& ExpectedWorkloads() {
  static const std::vector<std::string> workloads = {
      "point_lookup",
      "range_lookup",
      "bulk_build",
      "update_delete_maintenance",
      "crash_reopen",
      "validate",
      "repair_rebuild",
      "cold_cache",
      "warm_cache",
      "fallback_disabled"};
  return workloads;
}

bool ContainsMarker(std::string_view value) {
  static const std::vector<std::string> markers = {
      "docs" "/execution-plans", "public_release_evidence", "docs" "/findings",
      "docs/reference", "execution_plan", "contract", "findings",
      "reference", "IRC-"};
  return std::any_of(markers.begin(), markers.end(), [&](const auto& marker) {
    return value.find(marker) != std::string_view::npos;
  });
}

bool HasEvidence(const idx::IndexFamilyBenchmarkEvidenceRow& row,
                 std::string_view needle) {
  return std::any_of(row.evidence.begin(), row.evidence.end(),
                     [&](const auto& evidence) {
                       return evidence.find(needle) != std::string_view::npos;
                     });
}

void RequireNoAuthority(const idx::IndexFamilyBenchmarkEvidenceRow& row) {
  Require(row.observational_only, row.family_id + " row is not observational");
  Require(!row.catalog_authority, row.family_id + " claimed catalog authority");
  Require(!row.execution_authority,
          row.family_id + " claimed execution authority");
  Require(!row.visibility_authority,
          row.family_id + " claimed visibility authority");
  Require(!row.security_authority,
          row.family_id + " claimed security authority");
  Require(!row.transaction_finality_authority,
          row.family_id + " claimed transaction finality authority");
  Require(!row.recovery_authority,
          row.family_id + " claimed recovery authority");
  Require(!row.parser_authority, row.family_id + " claimed parser authority");
  Require(!row.donor_authority, row.family_id + " claimed donor authority");
  Require(!row.provider_authority, row.family_id + " claimed provider authority");
  Require(std::find(row.evidence.begin(), row.evidence.end(),
                    "catalog_authority=false") != row.evidence.end(),
          row.family_id + " missing catalog non-authority evidence");
  Require(std::find(row.evidence.begin(), row.evidence.end(),
                    "execution_authority=false") != row.evidence.end(),
          row.family_id + " missing execution non-authority evidence");
}

void RequireNoRuntimeLeak(const idx::IndexFamilyBenchmarkEvidenceRow& row) {
  Require(row.runtime_dependency_free,
          row.family_id + "." + row.workload + " runtime dependency flag false");
  Require(!ContainsMarker(row.family_id), "family id leaked marker");
  Require(!ContainsMarker(row.workload), "workload leaked marker");
  Require(!ContainsMarker(row.route_operation), "route leaked marker");
  Require(!ContainsMarker(row.route_capability_kind),
          "route capability leaked marker");
  Require(!ContainsMarker(row.diagnostic_code), "diagnostic leaked marker");
  Require(!ContainsMarker(row.message_key), "message key leaked marker");
  for (const auto& evidence : row.evidence) {
    Require(!ContainsMarker(evidence),
            row.family_id + "." + row.workload +
                " leaked non-runtime marker in evidence");
  }
}

std::string Key(const idx::IndexFamilyBenchmarkEvidenceRow& row) {
  return row.family_id + ":" + row.workload;
}

std::map<std::string, const idx::IndexFamilyBenchmarkEvidenceRow*> RowMap(
    const idx::IndexFamilyBenchmarkGateResult& result) {
  std::map<std::string, const idx::IndexFamilyBenchmarkEvidenceRow*> rows;
  for (const auto& row : result.rows) {
    Require(rows.emplace(Key(row), &row).second,
            "duplicate benchmark row " + Key(row));
  }
  return rows;
}

const idx::IndexFamilyBenchmarkEvidenceRow& RequireRow(
    const std::map<std::string, const idx::IndexFamilyBenchmarkEvidenceRow*>& rows,
    const std::string& family,
    const std::string& workload) {
  const auto it = rows.find(family + ":" + workload);
  Require(it != rows.end(), "missing benchmark row " + family + ":" + workload);
  return *it->second;
}

void RequireCompleteFamilyRow(
    const idx::IndexFamilyBenchmarkEvidenceRow& row) {
  Require(row.runtime_available,
          row.family_id + "." + row.workload + " runtime unavailable");
  Require(row.benchmark_clean_admissible,
          row.family_id + "." + row.workload + " benchmark not admitted");
  Require(row.concrete_runtime_consumed,
          row.family_id + "." + row.workload + " did not consume runtime");
  Require(row.standalone_provider_gate,
          row.family_id + "." + row.workload + " standalone provider gate missing");
  Require(row.route_consumed_gate,
          row.family_id + "." + row.workload + " route consumed gate missing");
  Require(row.route_kind != idx::IndexRouteKind::unknown,
          row.family_id + "." + row.workload + " route kind missing");
  if (row.sql_query_route_consumed || row.nosql_route_consumed) {
    Require(row.optimizer_selected_gate,
            row.family_id + "." + row.workload +
                " optimizer-selected route proof missing");
  }
  Require(!row.fail_closed,
          row.family_id + "." + row.workload + " failed closed");
  Require(row.diagnostic_code == "INDEX.BENCHMARK.OK",
          row.family_id + "." + row.workload + " diagnostic drift");
  Require(row.message_key == "index.benchmark.ok",
          row.family_id + "." + row.workload + " message drift");
  Require(row.blocker == "none",
          row.family_id + "." + row.workload + " blocker drift");
  Require(row.p50_microseconds > 0 && row.p95_microseconds > 0 &&
              row.p99_microseconds > 0,
          row.family_id + "." + row.workload + " missing latency sample");
  Require(row.p50_microseconds <= row.p95_microseconds &&
              row.p95_microseconds <= row.p99_microseconds,
          row.family_id + "." + row.workload + " percentile order broken");
  Require(row.operation_count > 0,
          row.family_id + "." + row.workload + " operation count missing");
  Require(row.pages_or_containers_touched > 0,
          row.family_id + "." + row.workload +
              " page/container evidence missing");
  if (row.workload != "validate" && row.workload != "repair_rebuild") {
    Require(!HasEvidence(row, "validation_repair_route_consumed=true"),
            row.family_id + "." + row.workload +
                " fell back to generic validation sample");
  }
}

void RequireBlockedFamilyRow(
    const idx::IndexFamilyBenchmarkEvidenceRow& row,
    const idx::IndexFamilyPhysicalCapabilityState& state) {
  Require(!row.runtime_available,
          row.family_id + "." + row.workload + " promoted runtime");
  Require(!row.benchmark_clean_admissible,
          row.family_id + "." + row.workload + " promoted benchmark clean");
  Require(!row.concrete_runtime_consumed,
          row.family_id + "." + row.workload + " consumed runtime");
  Require(!row.route_consumed_gate,
          row.family_id + "." + row.workload + " consumed blocked route");
  Require(!row.standalone_provider_gate,
          row.family_id + "." + row.workload +
              " promoted standalone provider gate");
  Require(row.fail_closed,
          row.family_id + "." + row.workload + " did not fail closed");
  Require(row.diagnostic_code == state.blocker_diagnostic_code,
          row.family_id + "." + row.workload + " blocker diagnostic mismatch");
  Require(row.message_key == state.blocker_message_key,
          row.family_id + "." + row.workload + " blocker message mismatch");
  Require(row.blocker ==
              idx::IndexFamilyPhysicalCapabilityBlockerName(state.blocker),
          row.family_id + "." + row.workload + " blocker mismatch");
  Require(row.p50_microseconds == 0 && row.p95_microseconds == 0 &&
              row.p99_microseconds == 0,
          row.family_id + "." + row.workload + " refusal recorded latency");
  Require(row.operation_count == 0 && row.rows_examined == 0 &&
              row.rows_returned == 0 && row.rows_materialized == 0,
          row.family_id + "." + row.workload + " refusal recorded work");
}

void VerifyEveryFamilyAndWorkloadCovered(
    const idx::IndexFamilyBenchmarkGateResult& result) {
  const auto rows = RowMap(result);
  const auto& descriptors = idx::BuiltinIndexFamilyDescriptors();
  Require(result.rows.size() == descriptors.size() * ExpectedWorkloads().size(),
          "benchmark evidence row count mismatch");

  bool saw_complete = false;
  bool saw_blocked = false;
  for (const auto& descriptor : descriptors) {
    const auto* state =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr,
            "missing capability state for " + descriptor.id);
    for (const auto& workload : ExpectedWorkloads()) {
      const auto& row = RequireRow(rows, descriptor.id, workload);
      Require(row.family_id == descriptor.id, "family id mismatch");
      Require(row.workload == workload, "workload mismatch");
      RequireNoAuthority(row);
      RequireNoRuntimeLeak(row);

      if (state->runtime_available && state->benchmark_clean &&
          state->blocker == idx::IndexFamilyPhysicalCapabilityBlocker::none) {
        saw_complete = true;
        RequireCompleteFamilyRow(row);
      } else {
        saw_blocked = true;
        RequireBlockedFamilyRow(row, *state);
      }
    }
  }
  Require(saw_complete, "no complete family benchmark rows found");
  Require(saw_blocked, "no blocked family benchmark rows found");
}

void VerifyFallbackDisabledEvidence(
    const idx::IndexFamilyBenchmarkGateResult& result) {
  const auto rows = RowMap(result);
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    const auto& row = RequireRow(rows, descriptor.id, "fallback_disabled");
    Require(row.fallback_disabled,
            descriptor.id + " fallback-disabled flag missing");
    const auto* state =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr, "missing fallback state " + descriptor.id);
    if (state->benchmark_clean) {
      Require(row.concrete_runtime_consumed && !row.fail_closed,
              descriptor.id + " fallback-disabled clean route did not pass");
    } else {
      Require(row.fail_closed && !row.concrete_runtime_consumed,
              descriptor.id + " fallback-disabled blocker did not refuse");
    }
  }
}

void VerifyCacheClassifications(
    const idx::IndexFamilyBenchmarkGateResult& result) {
  const auto rows = RowMap(result);
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    Require(RequireRow(rows, descriptor.id, "cold_cache").cache_classification ==
                "cold",
            descriptor.id + " cold cache classification drift");
    Require(RequireRow(rows, descriptor.id, "warm_cache").cache_classification ==
                "warm",
            descriptor.id + " warm cache classification drift");
    Require(RequireRow(rows, descriptor.id, "crash_reopen").cache_classification ==
                "cold",
            descriptor.id + " crash reopen classification drift");
  }
}

}  // namespace

int main() {
  idx::IndexFamilyBenchmarkGateRequest request;
  request.sample_iterations = 3;
  request.include_fallback_disabled_rows = true;
  const auto result = idx::BuildIndexFamilyBenchmarkEvidence(request);
  Require(result.ok(), "benchmark gate result refused: " +
                           result.diagnostic.diagnostic_code);
  VerifyEveryFamilyAndWorkloadCovered(result);
  VerifyFallbackDisabledEvidence(result);
  VerifyCacheClassifications(result);
  std::cout << "index_family_benchmark_gate=passed\n";
  return EXIT_SUCCESS;
}
