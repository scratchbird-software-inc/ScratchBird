// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "buffer_prefetch_route_executor.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace page = scratchbird::storage::page;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-241 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

page::PlanAwarePrefetchDescriptor Descriptor(
    std::string item_id,
    page::PlanAwarePrefetchFamily family,
    std::uint64_t generation = 241) {
  page::PlanAwarePrefetchDescriptor descriptor;
  descriptor.family = family;
  descriptor.item_id = std::move(item_id);
  descriptor.physical_plan_node_id = "physical-node:" + descriptor.item_id;
  descriptor.physical_plan_descriptor_digest =
      "digest:orh241:" + descriptor.item_id;
  descriptor.physical_plan_generation = 241;
  descriptor.descriptor_generation = generation;
  descriptor.byte_cost = 4096;
  descriptor.page_cost = 1;
  descriptor.diagnostic_only_authority = true;
  return descriptor;
}

page::BufferPrefetchReadaheadRequest Request(
    page::BufferPrefetchRouteKind route_kind,
    std::string route_label,
    page::PlanAwarePrefetchFamily family) {
  page::BufferPrefetchReadaheadRequest request;
  request.route_kind = route_kind;
  request.route_label = std::move(route_label);
  request.runtime_consumed = true;
  request.contract_only = false;
  request.prefetch.physical_plan_id = "plan:orh241:" + request.route_label;
  request.prefetch.physical_plan_generation = 241;
  request.prefetch.budget.max_bytes = 65536;
  request.prefetch.budget.max_pages = 16;
  request.prefetch.budget.max_items = 8;
  request.prefetch.budget.max_outstanding = 8;
  request.prefetch.descriptors.push_back(
      Descriptor("prefetch:" + request.route_label, family));
  request.hot_page_pinning.expected_page_generation = 42;
  request.hot_page_pinning.observed_page_generation = 42;
  request.hot_page_pinning.expected_epoch = 99;
  request.hot_page_pinning.observed_epoch = 99;
  request.hot_page_pinning.hot_pages_requested = 4;
  request.hot_page_pinning.hot_pages_pinned = 4;
  request.hot_page_pinning.pinning_runtime_consumed = true;
  request.without_prefetch.cache_lookups = 128;
  request.without_prefetch.cache_hits = 64;
  request.without_prefetch.io_read_ops = 80;
  request.without_prefetch.wait_time_us = 1200;
  request.with_prefetch.cache_lookups = 128;
  request.with_prefetch.cache_hits = 110;
  request.with_prefetch.io_read_ops = 22;
  request.with_prefetch.wait_time_us = 280;
  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_recheck_required = true;
  return request;
}

void RequireAccepted(const page::BufferPrefetchReadaheadResult& result,
                     std::string_view label,
                     std::string_view route_kind) {
  Require(result.ok(), std::string(label) + " route was not benchmark-clean");
  Require(result.accepted && result.benchmark_clean,
          std::string(label) + " route did not set benchmark-clean evidence");
  Require(!result.fallback_used && !result.fail_closed,
          std::string(label) + " route unexpectedly fell back/refused");
  Require(result.prefetch_result.ok(),
          std::string(label) + " route did not consume plan prefetch");
  Require(result.prefetch_result.counters.scheduled_items >= 1,
          std::string(label) + " route scheduled no prefetch descriptors");
  Require(result.improvement.cache_hit_improved &&
              result.improvement.io_improved &&
              result.improvement.wait_improved,
          std::string(label) + " route did not measure all improvements");
  Require(HasEvidence(result.evidence,
                      std::string("buffer_prefetch.route_kind=") +
                          std::string(route_kind)),
          std::string(label) + " route-kind evidence missing");
  Require(HasEvidence(result.evidence, "plan_aware_prefetch.scheduled="),
          std::string(label) + " nested prefetch schedule evidence missing");
  Require(HasEvidence(result.evidence,
                      "buffer_prefetch.prefetch_and_pinning_advisory_only=true"),
          std::string(label) + " advisory-only evidence missing");
  Require(HasEvidence(result.evidence,
                      "executor.buffer_prefetch.route_consumed=true"),
          std::string(label) + " executor route consumption evidence missing");
  Require(HasEvidence(result.evidence,
                      "executor.buffer_prefetch.mga_inventory_remains_authority=true"),
          std::string(label) + " executor MGA authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "buffer_prefetch.mga_transaction_inventory_authority=engine"),
          std::string(label) + " MGA authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "buffer_prefetch.transaction_inventory_proof_present=true"),
          std::string(label) + " MGA proof evidence missing");
  Require(HasEvidence(result.evidence,
                      "buffer_prefetch.security_recheck_required=true"),
          std::string(label) + " security recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "plan_aware_prefetch.finality_authority=false"),
          std::string(label) + " prefetch finality authority drifted");
  Require(HasEvidence(result.evidence,
                      "plan_aware_prefetch.visibility_authority=false"),
          std::string(label) + " prefetch visibility authority drifted");
}

void TestPositiveRoutes() {
  RequireAccepted(
      exec::ConsumeBufferPrefetchReadaheadRoute(
          Request(page::BufferPrefetchRouteKind::kSequentialScan,
                  "orh241.sequential_scan.heap_readahead",
                  page::PlanAwarePrefetchFamily::kHeapPage)),
      "sequential scan", "sequential_scan");
  RequireAccepted(
      exec::ConsumeBufferPrefetchReadaheadRoute(
          Request(page::BufferPrefetchRouteKind::kJoin,
                  "orh241.join.heap_probe_hot_pages",
                  page::PlanAwarePrefetchFamily::kHeapPage)),
      "join", "join");
  RequireAccepted(
      exec::ConsumeBufferPrefetchReadaheadRoute(
          Request(page::BufferPrefetchRouteKind::kBulkIngest,
                  "orh241.bulk_ingest.extent_readahead",
                  page::PlanAwarePrefetchFamily::kExtentMetadata)),
      "bulk ingest", "bulk_ingest");
  RequireAccepted(
      exec::ConsumeBufferPrefetchReadaheadRoute(
          Request(page::BufferPrefetchRouteKind::kAggregate,
                  "orh241.aggregate.large_payload_prefetch",
                  page::PlanAwarePrefetchFamily::kLargePayload)),
      "aggregate", "aggregate");
}

void RequireRejected(const page::BufferPrefetchReadaheadResult& result,
                     std::string_view diagnostic,
                     std::string_view message) {
  Require(!result.benchmark_clean, std::string(message) +
                                      " was marked benchmark-clean");
  Require(result.diagnostic_code.find(diagnostic) != std::string::npos,
          std::string(message) + " diagnostic mismatch: " +
              result.diagnostic_code);
}

void TestNegativeRoutes() {
  auto stale_prefetch = Request(page::BufferPrefetchRouteKind::kSequentialScan,
                                "orh241.negative.stale_prefetch_window",
                                page::PlanAwarePrefetchFamily::kHeapPage);
  stale_prefetch.prefetch.descriptors.front().descriptor_generation = 240;
  auto stale_result =
      exec::ConsumeBufferPrefetchReadaheadRoute(stale_prefetch);
  RequireRejected(stale_result,
                  "ORH_BUFFER_PREFETCH_READAHEAD_PREFETCH_NOT_CONSUMED",
                  "stale prefetch window");
  Require(HasEvidence(stale_result.evidence,
                      "plan_prefetch_stale_descriptor_generation"),
          "stale prefetch window missing nested generation diagnostic");

  auto unsafe_epoch = Request(page::BufferPrefetchRouteKind::kJoin,
                              "orh241.negative.unsafe_page_epoch",
                              page::PlanAwarePrefetchFamily::kHeapPage);
  unsafe_epoch.hot_page_pinning.observed_epoch += 1;
  RequireRejected(exec::ConsumeBufferPrefetchReadaheadRoute(unsafe_epoch),
                  "ORH_BUFFER_PREFETCH_READAHEAD_UNSAFE_EPOCH",
                  "unsafe page epoch");

  auto external_authority =
      Request(page::BufferPrefetchRouteKind::kBulkIngest,
              "orh241.negative.parser_client_reference_authority",
              page::PlanAwarePrefetchFamily::kExtentMetadata);
  external_authority.authority.parser_client_or_reference_authority = true;
  RequireRejected(exec::ConsumeBufferPrefetchReadaheadRoute(external_authority),
                  "ORH_BUFFER_PREFETCH_READAHEAD_UNSAFE_AUTHORITY",
                  "parser/client/reference authority");

  auto finality_authority =
      Request(page::BufferPrefetchRouteKind::kAggregate,
              "orh241.negative.prefetch_finality_authority",
              page::PlanAwarePrefetchFamily::kLargePayload);
  finality_authority.authority.prefetch_visibility_or_finality_authority = true;
  RequireRejected(exec::ConsumeBufferPrefetchReadaheadRoute(finality_authority),
                  "ORH_BUFFER_PREFETCH_READAHEAD_UNSAFE_AUTHORITY",
                  "prefetch finality authority");

  auto missing_mga = Request(page::BufferPrefetchRouteKind::kSequentialScan,
                             "orh241.negative.missing_mga_snapshot",
                             page::PlanAwarePrefetchFamily::kHeapPage);
  missing_mga.authority.engine_mga_snapshot_bound = false;
  RequireRejected(exec::ConsumeBufferPrefetchReadaheadRoute(missing_mga),
                  "ORH_BUFFER_PREFETCH_READAHEAD_MGA_UNPROVEN",
                  "missing MGA snapshot");

  auto resource_pressure = Request(page::BufferPrefetchRouteKind::kJoin,
                                   "orh241.negative.resource_pressure",
                                   page::PlanAwarePrefetchFamily::kHeapPage);
  resource_pressure.resource_pressure = true;
  const auto pressure_result =
      exec::ConsumeBufferPrefetchReadaheadRoute(resource_pressure);
  Require(pressure_result.fallback_used,
          "resource pressure did not use fallback");
  RequireRejected(pressure_result,
                  "ORH_BUFFER_PREFETCH_READAHEAD_RESOURCE_FALLBACK",
                  "resource pressure fallback");

  auto cancellation = Request(page::BufferPrefetchRouteKind::kBulkIngest,
                              "orh241.negative.cancellation",
                              page::PlanAwarePrefetchFamily::kExtentMetadata);
  cancellation.cancellation_requested = true;
  const auto cancellation_result =
      exec::ConsumeBufferPrefetchReadaheadRoute(cancellation);
  Require(cancellation_result.fallback_used,
          "cancellation did not use fallback");
  RequireRejected(cancellation_result,
                  "ORH_BUFFER_PREFETCH_READAHEAD_CANCELLED",
                  "cancellation fallback");

  auto contract_only = Request(page::BufferPrefetchRouteKind::kAggregate,
                               "orh241.negative.contract_only",
                               page::PlanAwarePrefetchFamily::kLargePayload);
  contract_only.runtime_consumed = false;
  contract_only.contract_only = true;
  RequireRejected(exec::ConsumeBufferPrefetchReadaheadRoute(contract_only),
                  "ORH_BUFFER_PREFETCH_READAHEAD_NO_RUNTIME",
                  "no-runtime/contract-only evidence");

  auto no_improvement = Request(page::BufferPrefetchRouteKind::kSequentialScan,
                                "orh241.negative.no_measured_improvement",
                                page::PlanAwarePrefetchFamily::kHeapPage);
  no_improvement.with_prefetch = no_improvement.without_prefetch;
  RequireRejected(exec::ConsumeBufferPrefetchReadaheadRoute(no_improvement),
                  "ORH_BUFFER_PREFETCH_READAHEAD_NO_IMPROVEMENT",
                  "missing measured improvement");
}

}  // namespace

int main() {
  TestPositiveRoutes();
  TestNegativeRoutes();
  std::cout << "ORH-241 buffer prefetch/read-ahead gate passed\n";
  return EXIT_SUCCESS;
}
