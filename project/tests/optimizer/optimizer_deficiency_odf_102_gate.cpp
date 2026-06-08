// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "async_page_io.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::storage_page};
}

platform::Status ErrorStatus() {
  return {platform::StatusCode::platform_required_feature_missing,
          platform::Severity::error, platform::Subsystem::storage_page};
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "parser_finality_authority=true",
          "donor_finality_authority=true",
          "write_ahead_log_finality_authority=true",
          "timestamp_finality_authority=true",
          "uuid_ordering_finality_authority=true",
          "publication_marker_finality_authority=true",
          "finality_authority=true", "visibility_authority=true",
          "security_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-102 evidence leaked forbidden authority or document token");
    }
  }
}

struct BackendFixture {
  bool sync_fails = false;
  int reads = 0;
  int writes = 0;
  int syncs = 0;
  std::vector<std::string> write_ids;
};

page::AsyncPageIoRouteBackend Backend(BackendFixture* fixture) {
  page::AsyncPageIoRouteBackend backend;
  backend.read_page = [fixture](const page::AsyncPageIoOperation& operation) {
    ++fixture->reads;
    page::AsyncPageIoBackendResult result;
    result.status = OkStatus();
    result.read_payload.assign(static_cast<std::size_t>(operation.byte_count),
                               static_cast<platform::byte>(operation.page_number));
    return result;
  };
  backend.write_page = [fixture](const page::AsyncPageIoOperation& operation) {
    ++fixture->writes;
    fixture->write_ids.push_back(operation.operation_id);
    page::AsyncPageIoBackendResult result;
    result.status = OkStatus();
    return result;
  };
  backend.fsync = [fixture]() {
    ++fixture->syncs;
    page::AsyncPageIoBackendResult result;
    result.status = fixture->sync_fails ? ErrorStatus() : OkStatus();
    if (fixture->sync_fails) {
      result.diagnostic.status = result.status;
      result.diagnostic.diagnostic_code = "fixture_sync_failed";
      result.diagnostic.message_key = "fixture.sync_failed";
    }
    return result;
  };
  return backend;
}

page::AsyncPageIoCapabilities Capabilities() {
  page::AsyncPageIoCapabilities capabilities;
  capabilities.async_read_supported = true;
  capabilities.async_write_supported = true;
  capabilities.async_fsync_supported = true;
  capabilities.write_combining_supported = true;
  capabilities.publication_marker_supported = true;
  capabilities.durable_sync_fence_supported = true;
  capabilities.max_batch_operations = 16;
  capabilities.max_batch_bytes = 65536;
  capabilities.max_combined_writes = 8;
  return capabilities;
}

page::AsyncPageIoOperation Operation(page::AsyncPageIoOperationKind kind,
                                     std::string id,
                                     platform::u64 page_number) {
  page::AsyncPageIoOperation operation;
  operation.kind = kind;
  operation.operation_id = std::move(id);
  operation.page_number = page_number;
  operation.page_generation = 102;
  operation.descriptor_generation = 102;
  operation.byte_count = 4096;
  operation.filespace_class = "hot_row";
  operation.physical_plan_id = "odf102.plan.root";
  operation.plan_prefetch_item_id = "odf097.heap.page";
  operation.publication_marker = "odf102.publication.marker.1";
  operation.expected_publication_marker = "odf102.publication.marker.1";
  if (kind == page::AsyncPageIoOperationKind::kWritePage) {
    operation.payload.assign(4096, static_cast<platform::byte>(page_number));
  }
  return operation;
}

page::AsyncPageIoRequest Request() {
  page::AsyncPageIoRequest request;
  request.route_generation = 102;
  request.capabilities = Capabilities();
  request.policy.estimated_sync_micros = 1000;
  request.policy.estimated_async_micros = 700;
  request.policy.minimum_speedup_basis_points = 50;
  request.operations = {
      Operation(page::AsyncPageIoOperationKind::kReadPage, "read.1", 1),
      Operation(page::AsyncPageIoOperationKind::kWritePage, "write.older", 2),
      Operation(page::AsyncPageIoOperationKind::kWritePage, "write.newer", 2),
      Operation(page::AsyncPageIoOperationKind::kFsync, "fsync.1", 0),
      Operation(page::AsyncPageIoOperationKind::kPublicationMarker,
                "publish.1", 0)};
  request.operations[2].page_generation = 103;
  request.operations[2].payload[0] = 77;
  request.resource_governance.operation_id = "odf102.async_page_io";
  request.resource_governance.descriptor.descriptor_id =
      "odf106.async_page_io.runtime_quota";
  request.resource_governance.descriptor.family =
      agents::ResourceGovernanceFamily::kAsyncPageIo;
  request.resource_governance.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.resource_governance.descriptor.source_path_or_label =
      "runtime.policy.odf106.async_page_io";
  request.resource_governance.descriptor.descriptor_generation = 102;
  request.resource_governance.descriptor.expected_generation = 102;
  request.resource_governance.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kSlowdownDegrade;
  request.resource_governance.descriptor.benchmark_clean = true;
  request.resource_governance.descriptor.runtime_dependency_present = true;
  request.resource_governance.descriptor.limits = {
      65536, 1, 1, 65536, 16, 2, 16, 1, 1, 16, 16, 4, 1000000};
  request.resource_governance.requested = {
      8192, 0, 0, 12288, 5, 1, 5, 0, 0, 5, 5, 1, 1000};
  return request;
}

void SuccessSelectsAsyncBatchAndCombinesWrites() {
  BackendFixture fixture;
  const auto result = page::ExecuteAsyncPageIoBatch(Request(),
                                                   Backend(&fixture));
  Require(result.ok(), "ODF-102 async route did not execute");
  Require(result.selected && !result.fallback_used,
          "ODF-102 async route was not selected");
  Require(result.write_combining_applied,
          "ODF-102 duplicate page write was not combined");
  Require(result.publication_marker_published,
          "ODF-102 publication marker was not published after sync fence");
  Require(fixture.reads == 1 && fixture.writes == 1 && fixture.syncs == 1,
          "ODF-102 backend read/write/sync counts changed");
  Require(fixture.write_ids.size() == 1 &&
              fixture.write_ids.front() == "write.newer",
          "ODF-102 write combining did not retain newest write");
  Require(result.counters.submitted_reads == 1 &&
              result.counters.submitted_writes == 1 &&
              result.counters.submitted_fsyncs == 1 &&
              result.counters.combined_writes == 1,
          "ODF-102 route counters changed");
  Require(EvidenceHas(result.evidence,
                      "async_page_io.odf061_page_fsync_integrated=true"),
          "ODF-102 ODF-061 page/fsync evidence missing");
  Require(EvidenceHas(result.evidence,
                      "async_page_io.odf061_filespace_class=hot_row"),
          "ODF-102 ODF-061 filespace evidence missing");
  Require(EvidenceHas(result.evidence,
                      "async_page_io.odf097_plan_prefetch_integrated=true"),
          "ODF-102 ODF-097 integration evidence missing");
  Require(EvidenceHas(result.evidence,
                      "async_page_io.mga_authority=durable_transaction_inventory"),
          "ODF-102 MGA authority evidence missing");
  Require(EvidenceHas(result.evidence, "resource_governance.route=odf106"),
          "ODF-102 ODF-106 governance admission evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void FallbacksHaveExactDiagnostics() {
  BackendFixture fixture;

  auto request = Request();
  request.policy.estimated_async_micros = 1200;
  auto result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 not-faster route did not fall back");
  Require(result.diagnostic.diagnostic_code == "async_page_io_not_faster",
          "ODF-102 not-faster diagnostic changed");

  request = Request();
  request.capabilities.async_read_supported = false;
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 unsupported async read did not fall back");
  Require(result.diagnostic.diagnostic_code == "async_page_io_unsupported",
          "ODF-102 unsupported diagnostic changed");

  request = Request();
  request.operations.front().descriptor_generation = 101;
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 stale descriptor did not fall back");
  Require(result.diagnostic.diagnostic_code == "async_page_io_stale_request",
          "ODF-102 stale diagnostic changed");

  request = Request();
  request.cancellation.cancel_before_selection = true;
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 cancellation did not fall back");
  Require(result.diagnostic.diagnostic_code == "async_page_io_cancelled",
          "ODF-102 cancellation diagnostic changed");

  request = Request();
  request.capabilities.max_batch_operations = 1;
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 capacity overflow did not fall back");
  Require(result.diagnostic.diagnostic_code == "async_page_io_capacity_exceeded",
          "ODF-102 capacity diagnostic changed");

  request = Request();
  request.capabilities.write_combining_supported = false;
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 write-combine refusal did not fall back");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_write_combine_refused",
          "ODF-102 write-combine diagnostic changed");

  request = Request();
  request.operations.back().publication_marker = "other.marker";
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 publication marker mismatch did not fall back");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_publication_marker_mismatch",
          "ODF-102 publication marker diagnostic changed");

  request = Request();
  request.operations.front().kind =
      static_cast<page::AsyncPageIoOperationKind>(999);
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(result.ok() && !result.selected && result.fallback_used,
          "ODF-102 corrupt operation kind did not fall back");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_unsupported_operation",
          "ODF-102 corrupt operation diagnostic changed");
}

void UnsafeFinalityAndSyncFenceFailClosed() {
  BackendFixture fixture;
  auto request = Request();
  request.operations.front().parser_or_donor_finality_authority = true;
  auto result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(!result.ok() && result.fail_closed,
          "ODF-102 unsafe finality claim was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_unsafe_finality_authority",
          "ODF-102 unsafe finality diagnostic changed");

  request = Request();
  request.operations.front().write_ahead_log_finality_authority = true;
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(!result.ok() && result.fail_closed,
          "ODF-102 write-ahead finality claim was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_unsafe_finality_authority",
          "ODF-102 write-ahead finality diagnostic changed");

  BackendFixture failing_sync;
  failing_sync.sync_fails = true;
  result = page::ExecuteAsyncPageIoBatch(Request(), Backend(&failing_sync));
  Require(!result.ok() && result.fail_closed,
          "ODF-102 sync fence failure was accepted");
  Require(result.diagnostic.diagnostic_code == "async_page_io_sync_fence_failed",
          "ODF-102 sync fence diagnostic changed");

  request = Request();
  std::swap(request.operations[3], request.operations[4]);
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(!result.ok() && result.fail_closed,
          "ODF-102 publication before sync fence was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_publication_before_sync_fence",
          "ODF-102 publication-before-sync diagnostic changed");

  request = Request();
  request.operations.erase(request.operations.begin() + 3);
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(!result.ok() && result.fail_closed,
          "ODF-102 implicit post-marker sync fence was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_publication_before_sync_fence",
          "ODF-102 implicit post-marker sync diagnostic changed");

  request = Request();
  auto late_write = request.operations[2];
  request.operations[2] = request.operations[3];
  request.operations[3] = request.operations[4];
  request.operations[4] = late_write;
  result = page::ExecuteAsyncPageIoBatch(request, Backend(&fixture));
  Require(!result.ok() && result.fail_closed,
          "ODF-102 write after publication marker was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_write_after_publication_marker",
          "ODF-102 write-after-publication diagnostic changed");
}

void NoExternalFinalityInvariant() {
  BackendFixture fixture;
  const auto result = page::ExecuteAsyncPageIoBatch(Request(),
                                                   Backend(&fixture));
  Require(result.ok(), "ODF-102 invariant fixture failed");
  Require(EvidenceHas(result.evidence,
                      "async_page_io.parser_finality_authority=false"),
          "ODF-102 parser finality invariant evidence missing");
  Require(EvidenceHas(result.evidence,
                      "async_page_io.write_ahead_log_finality_authority=false"),
          "ODF-102 write-ahead finality invariant evidence missing");
  Require(EvidenceHas(result.evidence,
                      "async_page_io.uuid_ordering_finality_authority=false"),
          "ODF-102 UUID ordering finality invariant evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

}  // namespace

int main() {
  SuccessSelectsAsyncBatchAndCombinesWrites();
  FallbacksHaveExactDiagnostics();
  UnsafeFinalityAndSyncFenceFailClosed();
  NoExternalFinalityInvariant();
  return EXIT_SUCCESS;
}
