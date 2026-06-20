// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-ASYNC-PAGE-IO-ANCHOR
#include "disk_device.hpp"
#include "resource_governance_admission.hpp"
#include "runtime_capabilities.hpp"
#include "runtime_platform.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class AsyncPageIoOperationKind {
  kReadPage,
  kWritePage,
  kFsync,
  kPublicationMarker
};

struct AsyncPageIoCapabilities {
  bool async_read_supported = false;
  bool async_write_supported = false;
  bool async_fsync_supported = false;
  bool write_combining_supported = false;
  bool publication_marker_supported = false;
  bool durable_sync_fence_supported = false;
  u64 max_batch_operations = 0;
  u64 max_batch_bytes = 0;
  u64 max_combined_writes = 0;
  scratchbird::core::platform::RuntimeCompatibilityDescriptor
      runtime_compatibility;
};

struct AsyncPageIoSelectionPolicy {
  bool enable_async_page_io = true;
  bool enable_write_combining = true;
  bool require_faster_than_sync = true;
  bool require_publication_marker_evidence = true;
  bool require_sync_fence_for_writes = true;
  u64 estimated_sync_micros = 0;
  u64 estimated_async_micros = 0;
  u32 minimum_speedup_basis_points = 1;
};

struct AsyncPageIoCancellation {
  bool cancel_before_selection = false;
  u64 cancel_after_considered_operations = 0;
};

struct AsyncPageIoOperation {
  AsyncPageIoOperationKind kind = AsyncPageIoOperationKind::kReadPage;
  std::string operation_id;
  u64 page_number = 0;
  u64 page_generation = 1;
  u64 descriptor_generation = 0;
  u64 byte_count = 0;
  std::vector<byte> payload;
  std::string filespace_class;
  std::string physical_plan_id;
  std::string plan_prefetch_item_id;
  std::string publication_marker;
  std::string expected_publication_marker;
  bool publication_marker_required = true;
  bool durable_transaction_inventory_authority = true;
  bool parser_or_reference_finality_authority = false;
  bool client_finality_authority = false;
  bool provider_finality_authority = false;
  bool write_ahead_log_finality_authority = false;
  bool timestamp_finality_authority = false;
  bool uuid_ordering_finality_authority = false;
  bool publication_marker_finality_authority = false;
};

struct AsyncPageIoRequest {
  u64 route_generation = 0;
  AsyncPageIoCapabilities capabilities;
  AsyncPageIoSelectionPolicy policy;
  AsyncPageIoCancellation cancellation;
  std::vector<AsyncPageIoOperation> operations;
  scratchbird::core::agents::ResourceGovernanceAdmissionRequest
      resource_governance;
};

struct AsyncPageIoBackendResult {
  Status status;
  std::vector<byte> read_payload;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct AsyncPageIoRouteBackend {
  std::function<AsyncPageIoBackendResult(const AsyncPageIoOperation&)>
      read_page;
  std::function<AsyncPageIoBackendResult(const AsyncPageIoOperation&)>
      write_page;
  std::function<AsyncPageIoBackendResult()> fsync;
};

struct AsyncPageIoCounters {
  u64 considered_operations = 0;
  u64 submitted_reads = 0;
  u64 submitted_writes = 0;
  u64 submitted_fsyncs = 0;
  u64 publication_markers = 0;
  u64 combined_writes = 0;
  u64 write_batches = 0;
  u64 write_tickets_issued = 0;
  u64 write_tickets_completed = 0;
  u64 write_worker_count = 0;
  u64 write_ticket_waits = 0;
  u64 skipped_operations = 0;
  u64 refused_operations = 0;
  u64 batch_bytes = 0;
};

struct AsyncPageIoReadResult {
  std::string operation_id;
  u64 page_number = 0;
  std::vector<byte> payload;
};

struct AsyncPageIoResult {
  Status status;
  bool selected = false;
  bool fallback_used = false;
  bool fail_closed = false;
  bool write_combining_applied = false;
  bool publication_marker_published = false;
  std::string selected_route;
  AsyncPageIoCounters counters;
  std::vector<std::string> executed_operation_ids;
  std::vector<AsyncPageIoReadResult> read_results;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* AsyncPageIoOperationKindName(AsyncPageIoOperationKind kind);

AsyncPageIoResult ExecuteAsyncPageIoBatch(
    const AsyncPageIoRequest& request,
    const AsyncPageIoRouteBackend& backend);

AsyncPageIoRouteBackend MakeFileDeviceAsyncPageIoBackend(
    scratchbird::storage::disk::FileDevice* device,
    u32 page_size);

DiagnosticRecord MakeAsyncPageIoDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

}  // namespace scratchbird::storage::page
