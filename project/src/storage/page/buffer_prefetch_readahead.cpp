// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "buffer_prefetch_readahead.hpp"

#include <utility>

namespace scratchbird::storage::page {
namespace platform = scratchbird::core::platform;

namespace {

Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::storage_page};
}

Status ErrorStatus() {
  return {platform::StatusCode::platform_required_feature_missing,
          platform::Severity::error, platform::Subsystem::storage_page};
}

DiagnosticRecord Diagnostic(Status status,
                            std::string code,
                            std::string message_key,
                            std::string remediation_hint) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = "storage.page.buffer_prefetch_readahead";
  diagnostic.remediation_hint = std::move(remediation_hint);
  return diagnostic;
}

void AddCommonEvidence(const BufferPrefetchReadaheadRequest& request,
                       BufferPrefetchReadaheadResult* result) {
  result->evidence.push_back("buffer_prefetch.route_label=" +
                             request.route_label);
  result->evidence.push_back(
      std::string("buffer_prefetch.route_kind=") +
      BufferPrefetchRouteKindName(request.route_kind));
  result->evidence.push_back(
      std::string("buffer_prefetch.runtime_consumed=") +
      (request.runtime_consumed ? "true" : "false"));
  result->evidence.push_back(
      std::string("buffer_prefetch.contract_only=") +
      (request.contract_only ? "true" : "false"));
  result->evidence.push_back(
      "buffer_prefetch.mga_transaction_inventory_authority=engine");
  result->evidence.push_back(
      std::string("buffer_prefetch.transaction_inventory_proof_present=") +
      (request.authority.transaction_inventory_authoritative ? "true"
                                                             : "false"));
  result->evidence.push_back("buffer_prefetch.visibility_authority=false");
  result->evidence.push_back("buffer_prefetch.finality_authority=false");
  result->evidence.push_back("buffer_prefetch.recovery_authority=false");
  result->evidence.push_back("buffer_prefetch.security_authority=false");
  result->evidence.push_back(
      std::string("buffer_prefetch.engine_mga_snapshot_bound=") +
      (request.authority.engine_mga_snapshot_bound ? "true" : "false"));
  result->evidence.push_back(
      std::string("buffer_prefetch.security_recheck_required=") +
      (request.authority.security_recheck_required ? "true" : "false"));
}

BufferPrefetchReadaheadResult Refuse(
    const BufferPrefetchReadaheadRequest& request,
    std::string code,
    std::string detail) {
  BufferPrefetchReadaheadResult result;
  result.status = ErrorStatus();
  result.accepted = false;
  result.benchmark_clean = false;
  result.fail_closed = true;
  result.diagnostic_code = code;
  AddCommonEvidence(request, &result);
  result.evidence.push_back("buffer_prefetch.fail_closed=true");
  result.evidence.push_back("buffer_prefetch.refused=" + code);
  result.diagnostic = Diagnostic(
      result.status, std::move(code),
      "storage.page.buffer_prefetch_readahead.unsafe", std::move(detail));
  return result;
}

BufferPrefetchReadaheadResult Fallback(
    const BufferPrefetchReadaheadRequest& request,
    std::string code,
    std::string detail) {
  BufferPrefetchReadaheadResult result;
  result.status = OkStatus();
  result.accepted = false;
  result.benchmark_clean = false;
  result.fallback_used = true;
  result.diagnostic_code = code;
  AddCommonEvidence(request, &result);
  result.evidence.push_back("buffer_prefetch.fallback_used=true");
  result.evidence.push_back("buffer_prefetch.refused_benchmark_clean=" + code);
  result.diagnostic = Diagnostic(
      result.status, std::move(code),
      "storage.page.buffer_prefetch_readahead.fallback", std::move(detail));
  return result;
}

BufferPrefetchImprovement Improvement(
    const BufferPrefetchMeasurement& without_prefetch,
    const BufferPrefetchMeasurement& with_prefetch) {
  BufferPrefetchImprovement improvement;
  if (with_prefetch.cache_hits > without_prefetch.cache_hits) {
    improvement.cache_hit_delta =
        with_prefetch.cache_hits - without_prefetch.cache_hits;
    improvement.cache_hit_improved = true;
  }
  if (without_prefetch.io_read_ops > with_prefetch.io_read_ops) {
    improvement.io_read_ops_saved =
        without_prefetch.io_read_ops - with_prefetch.io_read_ops;
    improvement.io_improved = true;
  }
  if (without_prefetch.wait_time_us > with_prefetch.wait_time_us) {
    improvement.wait_time_us_saved =
        without_prefetch.wait_time_us - with_prefetch.wait_time_us;
    improvement.wait_improved = true;
  }
  return improvement;
}

}  // namespace

const char* BufferPrefetchRouteKindName(BufferPrefetchRouteKind route_kind) {
  switch (route_kind) {
    case BufferPrefetchRouteKind::kSequentialScan:
      return "sequential_scan";
    case BufferPrefetchRouteKind::kJoin:
      return "join";
    case BufferPrefetchRouteKind::kBulkIngest:
      return "bulk_ingest";
    case BufferPrefetchRouteKind::kAggregate:
      return "aggregate";
  }
  return "unknown";
}

BufferPrefetchReadaheadResult EvaluateBufferPrefetchReadaheadRoute(
    const BufferPrefetchReadaheadRequest& request) {
  if (request.route_label.empty()) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_UNSAFE_ROUTE_LABEL",
                  "route label is required for prefetch/read-ahead evidence");
  }
  if (!request.runtime_consumed || request.contract_only) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_NO_RUNTIME",
                  "prefetch/read-ahead evidence must be runtime consumed");
  }
  if (request.authority.parser_client_or_donor_authority) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_UNSAFE_AUTHORITY",
                  "parser, client, and donor metadata cannot authorize "
                  "prefetch/read-ahead routes");
  }
  if (request.authority.prefetch_visibility_or_finality_authority ||
      request.authority.prefetch_recovery_authority ||
      request.authority.prefetch_security_authority) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_UNSAFE_AUTHORITY",
                  "prefetch/read-ahead and hot-page pinning are advisory only");
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_MGA_UNPROVEN",
                  "engine MGA snapshot and transaction inventory authority are "
                  "required before admitting the route");
  }
  if (!request.authority.security_recheck_required) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_SECURITY_UNPROVEN",
                  "security recheck evidence is required");
  }
  if (request.hot_page_pinning.unsafe_generation_or_epoch ||
      request.hot_page_pinning.expected_page_generation !=
          request.hot_page_pinning.observed_page_generation ||
      request.hot_page_pinning.expected_epoch !=
          request.hot_page_pinning.observed_epoch) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_UNSAFE_EPOCH",
                  "hot-page pinning generation or epoch is stale");
  }
  if (!request.hot_page_pinning.pinning_runtime_consumed ||
      request.hot_page_pinning.hot_pages_pinned == 0 ||
      request.hot_page_pinning.hot_pages_pinned >
          request.hot_page_pinning.hot_pages_requested) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_PINNING_UNPROVEN",
                  "hot-page pinning must be consumed by the runtime route");
  }
  if (request.resource_pressure) {
    return Fallback(request, "ORH_BUFFER_PREFETCH_READAHEAD_RESOURCE_FALLBACK",
                    "resource pressure forced authoritative non-prefetch "
                    "fallback");
  }
  if (request.cancellation_requested) {
    return Fallback(request, "ORH_BUFFER_PREFETCH_READAHEAD_CANCELLED",
                    "cancellation requested before benchmark-clean admission");
  }

  BufferPrefetchReadaheadResult result;
  result.status = OkStatus();
  result.prefetch_result = ExecutePlanAwarePrefetch(request.prefetch);
  if (!result.prefetch_result.ok() ||
      result.prefetch_result.counters.scheduled_items == 0) {
    auto refused = Refuse(
        request, "ORH_BUFFER_PREFETCH_READAHEAD_PREFETCH_NOT_CONSUMED",
        "plan-aware prefetch scheduler did not consume runtime descriptors");
    refused.prefetch_result = result.prefetch_result;
    refused.evidence.push_back("buffer_prefetch.prefetch_refusal=" +
                               result.prefetch_result.diagnostic.diagnostic_code);
    refused.evidence.insert(refused.evidence.end(),
                            result.prefetch_result.evidence.begin(),
                            result.prefetch_result.evidence.end());
    return refused;
  }

  result.improvement =
      Improvement(request.without_prefetch, request.with_prefetch);
  if (!result.improvement.cache_hit_improved ||
      !result.improvement.io_improved ||
      !result.improvement.wait_improved) {
    return Refuse(request, "ORH_BUFFER_PREFETCH_READAHEAD_NO_IMPROVEMENT",
                  "cache-hit, IO, and wait-time improvement are required");
  }

  result.accepted = true;
  result.benchmark_clean = true;
  result.diagnostic_code = "ORH_BUFFER_PREFETCH_READAHEAD_OK";
  AddCommonEvidence(request, &result);
  result.evidence.push_back("buffer_prefetch.benchmark_clean=true");
  result.evidence.push_back("buffer_prefetch.prefetch_scheduled_items=" +
                            std::to_string(
                                result.prefetch_result.counters.scheduled_items));
  result.evidence.push_back("buffer_prefetch.hot_pages_pinned=" +
                            std::to_string(
                                request.hot_page_pinning.hot_pages_pinned));
  result.evidence.push_back("buffer_prefetch.cache_hit_delta=" +
                            std::to_string(result.improvement.cache_hit_delta));
  result.evidence.push_back("buffer_prefetch.io_read_ops_saved=" +
                            std::to_string(
                                result.improvement.io_read_ops_saved));
  result.evidence.push_back("buffer_prefetch.wait_time_us_saved=" +
                            std::to_string(
                                result.improvement.wait_time_us_saved));
  result.evidence.push_back(
      "buffer_prefetch.prefetch_and_pinning_advisory_only=true");
  result.evidence.insert(result.evidence.end(),
                         result.prefetch_result.evidence.begin(),
                         result.prefetch_result.evidence.end());
  result.diagnostic = Diagnostic(
      result.status, result.diagnostic_code,
      "storage.page.buffer_prefetch_readahead.accepted",
      "runtime consumed prefetch/read-ahead and hot-page pinning with measured "
      "cache-hit, IO, and wait improvement");
  return result;
}

}  // namespace scratchbird::storage::page
