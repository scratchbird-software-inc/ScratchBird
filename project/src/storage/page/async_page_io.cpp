// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "async_page_io.hpp"

#include "page_manager.hpp"

#include <limits>
#include <map>
#include <string>
#include <utility>

namespace scratchbird::storage::page {
namespace platform = scratchbird::core::platform;
namespace agents = scratchbird::core::agents;

namespace {

Status AsyncOkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::storage_page};
}

Status AsyncErrorStatus() {
  return {platform::StatusCode::platform_required_feature_missing,
          platform::Severity::error, platform::Subsystem::storage_page};
}

bool AddWouldOverflow(u64 lhs, u64 rhs) {
  return lhs > std::numeric_limits<u64>::max() - rhs;
}

u64 OperationBytes(const AsyncPageIoOperation& operation) {
  if (operation.kind == AsyncPageIoOperationKind::kWritePage) {
    return operation.payload.size();
  }
  if (operation.kind == AsyncPageIoOperationKind::kReadPage) {
    return operation.byte_count;
  }
  return 0;
}

bool IsWrite(const AsyncPageIoOperation& operation) {
  return operation.kind == AsyncPageIoOperationKind::kWritePage;
}

bool IsRead(const AsyncPageIoOperation& operation) {
  return operation.kind == AsyncPageIoOperationKind::kReadPage;
}

bool IsFsync(const AsyncPageIoOperation& operation) {
  return operation.kind == AsyncPageIoOperationKind::kFsync;
}

bool IsPublicationMarker(const AsyncPageIoOperation& operation) {
  return operation.kind == AsyncPageIoOperationKind::kPublicationMarker;
}

bool IsKnownOperationKind(AsyncPageIoOperationKind kind) {
  switch (kind) {
    case AsyncPageIoOperationKind::kReadPage:
    case AsyncPageIoOperationKind::kWritePage:
    case AsyncPageIoOperationKind::kFsync:
    case AsyncPageIoOperationKind::kPublicationMarker:
      return true;
  }
  return false;
}

void AddAuthorityEvidence(AsyncPageIoResult* result) {
  result->evidence.push_back("async_page_io.diagnostic_only=true");
  result->evidence.push_back("async_page_io.finality_authority=false");
  result->evidence.push_back("async_page_io.visibility_authority=false");
  result->evidence.push_back("async_page_io.security_authority=false");
  result->evidence.push_back(
      "async_page_io.mga_authority=durable_transaction_inventory");
  result->evidence.push_back("async_page_io.parser_finality_authority=false");
  result->evidence.push_back("async_page_io.donor_finality_authority=false");
  result->evidence.push_back(
      "async_page_io.write_ahead_log_finality_authority=false");
  result->evidence.push_back("async_page_io.timestamp_finality_authority=false");
  result->evidence.push_back(
      "async_page_io.uuid_ordering_finality_authority=false");
  result->evidence.push_back(
      "async_page_io.publication_marker_finality_authority=false");
}

void AddCounterEvidence(AsyncPageIoResult* result) {
  result->evidence.push_back("async_page_io.considered_operations=" +
                             std::to_string(result->counters.considered_operations));
  result->evidence.push_back("async_page_io.submitted_reads=" +
                             std::to_string(result->counters.submitted_reads));
  result->evidence.push_back("async_page_io.submitted_writes=" +
                             std::to_string(result->counters.submitted_writes));
  result->evidence.push_back("async_page_io.submitted_fsyncs=" +
                             std::to_string(result->counters.submitted_fsyncs));
  result->evidence.push_back("async_page_io.publication_markers=" +
                             std::to_string(result->counters.publication_markers));
  result->evidence.push_back("async_page_io.combined_writes=" +
                             std::to_string(result->counters.combined_writes));
  result->evidence.push_back("async_page_io.batch_bytes=" +
                             std::to_string(result->counters.batch_bytes));
  result->evidence.push_back(
      std::string("async_page_io.write_combining_applied=") +
      (result->write_combining_applied ? "true" : "false"));
  result->evidence.push_back(
      std::string("async_page_io.publication_marker_published=") +
      (result->publication_marker_published ? "true" : "false"));
}

AsyncPageIoResult Fallback(const AsyncPageIoRequest& request,
                           std::string diagnostic_code,
                           std::string message_key,
                           std::string detail = {}) {
  AsyncPageIoResult result;
  result.status = AsyncOkStatus();
  result.fallback_used = true;
  result.selected_route = "sync_page_io_fallback";
  result.counters.refused_operations = request.operations.size();
  result.evidence.push_back("async_page_io.selected=false");
  result.evidence.push_back("async_page_io.fallback_used=true");
  result.evidence.push_back("async_page_io.fallback_reason=" +
                            diagnostic_code);
  AddAuthorityEvidence(&result);
  AddCounterEvidence(&result);
  result.diagnostic = MakeAsyncPageIoDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

AsyncPageIoResult Refuse(const AsyncPageIoRequest& request,
                         std::string diagnostic_code,
                         std::string message_key,
                         std::string detail = {}) {
  AsyncPageIoResult result;
  result.status = AsyncErrorStatus();
  result.fail_closed = true;
  result.counters.refused_operations = request.operations.size();
  result.evidence.push_back("async_page_io.selected=false");
  result.evidence.push_back("async_page_io.fail_closed=true");
  result.evidence.push_back("async_page_io.refusal_reason=" +
                            diagnostic_code);
  AddAuthorityEvidence(&result);
  AddCounterEvidence(&result);
  result.diagnostic = MakeAsyncPageIoDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

void AppendGovernanceEvidence(
    AsyncPageIoResult* result,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  result->evidence.insert(result->evidence.end(), governance.evidence.begin(),
                          governance.evidence.end());
  result->evidence.push_back("async_page_io.resource_governance_action=" +
                             std::string(agents::ResourceGovernanceActionName(
                                 governance.action)));
}

void AppendCompatibilityEvidence(
    AsyncPageIoResult* result,
    const platform::RuntimeCompatibilityResult& compatibility) {
  result->evidence.insert(result->evidence.end(),
                          compatibility.evidence.begin(),
                          compatibility.evidence.end());
}

platform::RuntimeCompatibilityDescriptor AsyncPageIoCompatibility(
    const AsyncPageIoRequest& request) {
  auto descriptor = request.capabilities.runtime_compatibility;
  if (descriptor.route_id.empty()) {
    descriptor = platform::CurrentRuntimeCompatibilityDescriptor(
        "storage.page.async_page_io");
  }
  descriptor.route_id = descriptor.route_id.empty()
                            ? "storage.page.async_page_io"
                            : descriptor.route_id;
  descriptor.source_component = "storage.page.async_page_io";
  descriptor.accelerator_requested = true;
  descriptor.deterministic_scalar_fallback_available = true;
  if (descriptor.provider_accelerator_capabilities.empty()) {
    if (request.capabilities.async_read_supported) {
      descriptor.provider_accelerator_capabilities.push_back("async_read");
    }
    if (request.capabilities.async_write_supported) {
      descriptor.provider_accelerator_capabilities.push_back("async_write");
    }
    if (request.capabilities.async_fsync_supported &&
        request.capabilities.durable_sync_fence_supported) {
      descriptor.provider_accelerator_capabilities.push_back("durable_sync_fence");
    }
    if (request.capabilities.write_combining_supported) {
      descriptor.provider_accelerator_capabilities.push_back("write_combining");
    }
    if (request.capabilities.publication_marker_supported) {
      descriptor.provider_accelerator_capabilities.push_back("publication_marker");
    }
  }
  return descriptor;
}

AsyncPageIoResult GovernanceFallback(
    const AsyncPageIoRequest& request,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  auto result = Fallback(request, "async_page_io_odf106_quota_degrade",
                         "storage.page.async_page_io.odf106_quota_degrade",
                         governance.diagnostic_code);
  AppendGovernanceEvidence(&result, governance);
  return result;
}

AsyncPageIoResult GovernanceRefusal(
    const AsyncPageIoRequest& request,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  auto result = Refuse(request, "async_page_io_odf106_quota_refused",
                       "storage.page.async_page_io.odf106_quota_refused",
                       governance.diagnostic_code);
  AppendGovernanceEvidence(&result, governance);
  return result;
}

bool FasterEnough(const AsyncPageIoSelectionPolicy& policy) {
  if (!policy.require_faster_than_sync) {
    return true;
  }
  if (policy.estimated_sync_micros == 0 || policy.estimated_async_micros == 0 ||
      policy.estimated_async_micros >= policy.estimated_sync_micros) {
    return false;
  }
  const u64 saved = policy.estimated_sync_micros - policy.estimated_async_micros;
  return static_cast<long double>(saved) * 10000.0L >=
         static_cast<long double>(policy.estimated_sync_micros) *
             static_cast<long double>(policy.minimum_speedup_basis_points);
}

bool HasExternalAuthorityClaim(const AsyncPageIoOperation& operation) {
  return !operation.durable_transaction_inventory_authority ||
         operation.parser_or_donor_finality_authority ||
         operation.client_finality_authority ||
         operation.provider_finality_authority ||
         operation.write_ahead_log_finality_authority ||
         operation.timestamp_finality_authority ||
         operation.uuid_ordering_finality_authority ||
         operation.publication_marker_finality_authority;
}

bool PublicationEvidenceMatches(const AsyncPageIoOperation& operation) {
  if (!operation.publication_marker_required) {
    return true;
  }
  return !operation.expected_publication_marker.empty() &&
         operation.publication_marker == operation.expected_publication_marker;
}

bool HasDuplicateWritePages(const std::vector<AsyncPageIoOperation>& operations) {
  std::map<u64, bool> seen;
  for (const auto& operation : operations) {
    if (!IsWrite(operation)) {
      continue;
    }
    if (seen[operation.page_number]) {
      return true;
    }
    seen[operation.page_number] = true;
  }
  return false;
}

AsyncPageIoResult BackendFailure(const AsyncPageIoRequest& request,
                                 const AsyncPageIoBackendResult& backend_result,
                                 std::string diagnostic_code,
                                 std::string message_key) {
  AsyncPageIoResult result = Refuse(request, std::move(diagnostic_code),
                                   std::move(message_key),
                                   backend_result.diagnostic.diagnostic_code);
  result.evidence.push_back("async_page_io.backend_diagnostic=" +
                            backend_result.diagnostic.diagnostic_code);
  return result;
}

std::vector<AsyncPageIoOperation> CombineWrites(
    const std::vector<AsyncPageIoOperation>& operations,
    AsyncPageIoCounters* counters) {
  std::vector<AsyncPageIoOperation> combined;
  std::map<u64, std::size_t> write_index_by_page;

  for (const auto& operation : operations) {
    if (!IsWrite(operation)) {
      combined.push_back(operation);
      write_index_by_page.clear();
      continue;
    }
    auto found = write_index_by_page.find(operation.page_number);
    if (found == write_index_by_page.end()) {
      write_index_by_page[operation.page_number] = combined.size();
      combined.push_back(operation);
      continue;
    }
    ++counters->combined_writes;
    combined[found->second] = operation;
  }
  return combined;
}

}  // namespace

const char* AsyncPageIoOperationKindName(AsyncPageIoOperationKind kind) {
  switch (kind) {
    case AsyncPageIoOperationKind::kReadPage:
      return "read_page";
    case AsyncPageIoOperationKind::kWritePage:
      return "write_page";
    case AsyncPageIoOperationKind::kFsync:
      return "fsync";
    case AsyncPageIoOperationKind::kPublicationMarker:
      return "publication_marker";
  }
  return "unknown";
}

AsyncPageIoResult ExecuteAsyncPageIoBatch(
    const AsyncPageIoRequest& request,
    const AsyncPageIoRouteBackend& backend) {
  auto governance_request = request.resource_governance;
  governance_request.expected_family =
      agents::ResourceGovernanceFamily::kAsyncPageIo;
  const auto governance = agents::AdmitResourceGovernance(governance_request);
  if (governance.action == agents::ResourceGovernanceAction::kFailClosed) {
    return GovernanceRefusal(request, governance);
  }
  if (governance.action == agents::ResourceGovernanceAction::kSlowdownDegrade ||
      governance.action ==
          agents::ResourceGovernanceAction::kExactScalarFallback ||
      governance.action == agents::ResourceGovernanceAction::kCancel) {
    return GovernanceFallback(request, governance);
  }
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      AsyncPageIoCompatibility(request));
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::exact_scalar_fallback) {
    auto result = Fallback(
        request, "async_page_io_odf107_runtime_fallback",
        "storage.page.async_page_io.odf107_runtime_fallback",
        compatibility.diagnostic_code);
    AppendGovernanceEvidence(&result, governance);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  }
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::fail_closed) {
    auto result = Refuse(
        request, "async_page_io_odf107_runtime_refused",
        "storage.page.async_page_io.odf107_runtime_refused",
        compatibility.diagnostic_code);
    AppendGovernanceEvidence(&result, governance);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  }
  if (!request.policy.enable_async_page_io) {
    auto result = Fallback(request, "async_page_io_disabled_by_policy",
                           "storage.page.async_page_io.disabled_by_policy",
                           "async page I/O policy disabled");
    AppendGovernanceEvidence(&result, governance);
    return result;
  }
  if (request.cancellation.cancel_before_selection) {
    auto result = Fallback(request, "async_page_io_cancelled",
                           "storage.page.async_page_io.cancelled",
                           "cancelled before async route selection");
    AppendGovernanceEvidence(&result, governance);
    return result;
  }
  if (request.route_generation == 0) {
    auto result = Fallback(request, "async_page_io_stale_request",
                           "storage.page.async_page_io.stale_request",
                           "route generation is required");
    AppendGovernanceEvidence(&result, governance);
    return result;
  }
  if (request.operations.size() > request.capabilities.max_batch_operations) {
    auto result = Fallback(request, "async_page_io_capacity_exceeded",
                           "storage.page.async_page_io.capacity_exceeded",
                           "operation count exceeds async batch capacity");
    AppendGovernanceEvidence(&result, governance);
    return result;
  }

  bool has_read = false;
  bool has_write = false;
  bool has_fsync = false;
  bool has_publication_marker = false;
  u64 batch_bytes = 0;
  u64 write_count = 0;
  u64 considered_operations = 0;

  for (const auto& operation : request.operations) {
    ++considered_operations;
    if (!IsKnownOperationKind(operation.kind)) {
      return Fallback(request, "async_page_io_unsupported_operation",
                      "storage.page.async_page_io.unsupported_operation",
                      operation.operation_id);
    }
    if (operation.operation_id.empty() || operation.descriptor_generation !=
                                            request.route_generation ||
        operation.page_generation == 0) {
      return Fallback(request, "async_page_io_stale_request",
                      "storage.page.async_page_io.stale_request",
                      "operation identity or generation is stale");
    }
    if (HasExternalAuthorityClaim(operation)) {
      return Refuse(request, "async_page_io_unsafe_finality_authority",
                    "storage.page.async_page_io.unsafe_finality_authority",
                    "async I/O cannot own transaction finality");
    }
    if (!PublicationEvidenceMatches(operation)) {
      return Fallback(request, "async_page_io_publication_marker_mismatch",
                      "storage.page.async_page_io.publication_marker_mismatch",
                      operation.operation_id);
    }

    has_read = has_read || IsRead(operation);
    has_write = has_write || IsWrite(operation);
    has_fsync = has_fsync || IsFsync(operation);
    has_publication_marker = has_publication_marker ||
                             IsPublicationMarker(operation);
    write_count += IsWrite(operation) ? 1 : 0;

    const u64 operation_bytes = OperationBytes(operation);
    if (AddWouldOverflow(batch_bytes, operation_bytes)) {
      return Fallback(request, "async_page_io_capacity_exceeded",
                      "storage.page.async_page_io.capacity_exceeded",
                      "operation byte accounting overflow");
    }
    batch_bytes += operation_bytes;
    if (request.cancellation.cancel_after_considered_operations != 0 &&
        considered_operations >
            request.cancellation.cancel_after_considered_operations) {
      return Fallback(request, "async_page_io_cancelled",
                      "storage.page.async_page_io.cancelled",
                      "cancelled during async route selection");
    }
  }

  if (batch_bytes > request.capabilities.max_batch_bytes) {
    return Fallback(request, "async_page_io_capacity_exceeded",
                    "storage.page.async_page_io.capacity_exceeded",
                    "batch bytes exceed async route capacity");
  }
  if (has_read && (!request.capabilities.async_read_supported ||
                   !backend.read_page)) {
    return Fallback(request, "async_page_io_unsupported",
                    "storage.page.async_page_io.unsupported",
                    "async page reads are unsupported");
  }
  if (has_write && (!request.capabilities.async_write_supported ||
                    !backend.write_page)) {
    return Fallback(request, "async_page_io_unsupported",
                    "storage.page.async_page_io.unsupported",
                    "async page writes are unsupported");
  }
  if ((has_fsync || (has_write && request.policy.require_sync_fence_for_writes)) &&
      (!request.capabilities.async_fsync_supported ||
       !request.capabilities.durable_sync_fence_supported || !backend.fsync)) {
    return Fallback(request, "async_page_io_unsupported",
                    "storage.page.async_page_io.unsupported",
                    "async sync fence is unsupported");
  }
  if ((has_publication_marker || request.policy.require_publication_marker_evidence) &&
      !request.capabilities.publication_marker_supported) {
    return Fallback(request, "async_page_io_unsupported",
                    "storage.page.async_page_io.unsupported",
                    "publication marker evidence is unsupported");
  }
  if (request.policy.enable_write_combining &&
      HasDuplicateWritePages(request.operations) &&
      (!request.capabilities.write_combining_supported ||
       request.capabilities.max_combined_writes == 0 ||
       write_count > request.capabilities.max_combined_writes)) {
    return Fallback(request, "async_page_io_write_combine_refused",
                    "storage.page.async_page_io.write_combine_refused",
                    "write combining is unavailable or over capacity");
  }
  if (!FasterEnough(request.policy)) {
    return Fallback(request, "async_page_io_not_faster",
                    "storage.page.async_page_io.not_faster",
                    "async route estimate did not beat synchronous route");
  }

  AsyncPageIoResult result;
  result.status = AsyncOkStatus();
  result.selected = true;
  result.selected_route = "async_batched_page_io";
  result.counters.batch_bytes = batch_bytes;
  result.evidence.push_back("async_page_io.selected=true");
  result.evidence.push_back("async_page_io.route=async_batched_page_io");
  result.evidence.push_back("async_page_io.route_generation=" +
                            std::to_string(request.route_generation));
  result.evidence.push_back("async_page_io.odf061_page_fsync_integrated=true");
  result.evidence.push_back("async_page_io.odf097_plan_prefetch_integrated=true");
  AppendGovernanceEvidence(&result, governance);
  AppendCompatibilityEvidence(&result, compatibility);

  AsyncPageIoCounters combine_counters;
  std::vector<AsyncPageIoOperation> operations = request.operations;
  if (request.policy.enable_write_combining &&
      request.capabilities.write_combining_supported) {
    operations = CombineWrites(request.operations, &combine_counters);
    result.counters.combined_writes = combine_counters.combined_writes;
    result.write_combining_applied = result.counters.combined_writes > 0;
  }

  bool write_submitted = false;
  bool fsync_submitted = false;
  bool marker_seen = false;

  for (const auto& operation : operations) {
    ++result.counters.considered_operations;
    result.evidence.push_back(
        "async_page_io.considered=" + operation.operation_id + ";kind=" +
        AsyncPageIoOperationKindName(operation.kind));
    if (!operation.filespace_class.empty()) {
      result.evidence.push_back("async_page_io.odf061_filespace_class=" +
                                operation.filespace_class);
    }
    if (!operation.plan_prefetch_item_id.empty()) {
      result.evidence.push_back("async_page_io.odf097_prefetch_item=" +
                                operation.plan_prefetch_item_id);
    }
    if (!operation.physical_plan_id.empty()) {
      result.evidence.push_back("async_page_io.odf097_physical_plan=" +
                                operation.physical_plan_id);
    }

    if (IsRead(operation)) {
      AsyncPageIoBackendResult read = backend.read_page(operation);
      if (!read.ok()) {
        return BackendFailure(request, read, "async_page_io_backend_read_failed",
                              "storage.page.async_page_io.backend_read_failed");
      }
      ++result.counters.submitted_reads;
      result.executed_operation_ids.push_back(operation.operation_id);
      result.read_results.push_back(
          {operation.operation_id, operation.page_number,
           std::move(read.read_payload)});
      continue;
    }

    if (IsWrite(operation)) {
      if (marker_seen) {
        return Refuse(request, "async_page_io_write_after_publication_marker",
                      "storage.page.async_page_io.write_after_publication_marker",
                      operation.operation_id);
      }
      AsyncPageIoBackendResult write = backend.write_page(operation);
      if (!write.ok()) {
        return BackendFailure(request, write,
                              "async_page_io_backend_write_failed",
                              "storage.page.async_page_io.backend_write_failed");
      }
      ++result.counters.submitted_writes;
      write_submitted = true;
      result.executed_operation_ids.push_back(operation.operation_id);
      continue;
    }

    if (IsFsync(operation)) {
      AsyncPageIoBackendResult synced = backend.fsync();
      if (!synced.ok()) {
        return BackendFailure(request, synced,
                              "async_page_io_sync_fence_failed",
                              "storage.page.async_page_io.sync_fence_failed");
      }
      ++result.counters.submitted_fsyncs;
      fsync_submitted = true;
      result.executed_operation_ids.push_back(operation.operation_id);
      continue;
    }

    if (IsPublicationMarker(operation)) {
      if (request.policy.require_sync_fence_for_writes && !fsync_submitted) {
        return Refuse(request,
                      "async_page_io_publication_before_sync_fence",
                      "storage.page.async_page_io.publication_before_sync_fence",
                      operation.operation_id);
      }
      ++result.counters.publication_markers;
      marker_seen = true;
      result.executed_operation_ids.push_back(operation.operation_id);
    }
  }

  if (write_submitted && request.policy.require_sync_fence_for_writes &&
      !fsync_submitted) {
    AsyncPageIoBackendResult synced = backend.fsync();
    if (!synced.ok()) {
      return BackendFailure(request, synced, "async_page_io_sync_fence_failed",
                            "storage.page.async_page_io.sync_fence_failed");
    }
    ++result.counters.submitted_fsyncs;
    fsync_submitted = true;
  }

  result.publication_marker_published = marker_seen && fsync_submitted;
  AddAuthorityEvidence(&result);
  AddCounterEvidence(&result);
  result.diagnostic = MakeAsyncPageIoDiagnostic(
      result.status, "ok", "storage.page.async_page_io.executed",
      "async batched page I/O route executed");
  return result;
}

AsyncPageIoRouteBackend MakeFileDeviceAsyncPageIoBackend(
    scratchbird::storage::disk::FileDevice* device,
    u32 page_size) {
  AsyncPageIoRouteBackend backend;
  backend.read_page = [device, page_size](
                          const AsyncPageIoOperation& operation) {
    AsyncPageIoBackendResult result;
    if (device == nullptr || page_size == 0) {
      result.status = AsyncErrorStatus();
      result.diagnostic = MakeAsyncPageIoDiagnostic(
          result.status, "async_page_io_file_device_unavailable",
          "storage.page.async_page_io.file_device_unavailable");
      return result;
    }
    const u64 bytes = operation.byte_count == 0 ? page_size
                                                : operation.byte_count;
    const auto page_offset = CheckedPageOffset(page_size, operation.page_number);
    if (!page_offset.ok()) {
      result.status = page_offset.status;
      result.diagnostic = page_offset.diagnostic;
      return result;
    }
    const auto extent = scratchbird::storage::disk::CheckFileDeviceExtent(
        page_offset.offset,
        static_cast<std::size_t>(bytes));
    if (!extent.ok()) {
      result.status = extent.status;
      result.diagnostic = extent.diagnostic;
      return result;
    }
    result.read_payload.resize(static_cast<std::size_t>(bytes));
    auto read = device->ReadAt(page_offset.offset,
                               result.read_payload.data(),
                               result.read_payload.size());
    result.status = read.status;
    result.diagnostic = read.diagnostic;
    return result;
  };
  backend.write_page = [device, page_size](
                           const AsyncPageIoOperation& operation) {
    AsyncPageIoBackendResult result;
    if (device == nullptr || page_size == 0) {
      result.status = AsyncErrorStatus();
      result.diagnostic = MakeAsyncPageIoDiagnostic(
          result.status, "async_page_io_file_device_unavailable",
          "storage.page.async_page_io.file_device_unavailable");
      return result;
    }
    const auto page_offset = CheckedPageOffset(page_size, operation.page_number);
    if (!page_offset.ok()) {
      result.status = page_offset.status;
      result.diagnostic = page_offset.diagnostic;
      return result;
    }
    const auto extent = scratchbird::storage::disk::CheckFileDeviceExtent(
        page_offset.offset,
        operation.payload.size());
    if (!extent.ok()) {
      result.status = extent.status;
      result.diagnostic = extent.diagnostic;
      return result;
    }
    auto write = device->WriteAt(page_offset.offset,
                                 operation.payload.data(),
                                 operation.payload.size());
    result.status = write.status;
    result.diagnostic = write.diagnostic;
    return result;
  };
  backend.fsync = [device]() {
    AsyncPageIoBackendResult result;
    if (device == nullptr) {
      result.status = AsyncErrorStatus();
      result.diagnostic = MakeAsyncPageIoDiagnostic(
          result.status, "async_page_io_file_device_unavailable",
          "storage.page.async_page_io.file_device_unavailable");
      return result;
    }
    auto synced = device->Sync();
    result.status = synced.status;
    result.diagnostic = synced.diagnostic;
    return result;
  };
  return backend;
}

DiagnosticRecord MakeAsyncPageIoDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = "storage.page.async_page_io";
  diagnostic.remediation_hint = std::move(detail);
  return diagnostic;
}

}  // namespace scratchbird::storage::page
