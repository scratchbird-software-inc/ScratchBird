// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ORH_STREAMING_CURSOR_MANAGER
// ORH_SECURE_CONTINUATION_TOKEN
#include "runtime_platform.hpp"
#include "result_cursor_plan_memory_governance.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scratchbird::wire {

namespace memory = scratchbird::core::memory;

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

struct StreamingCursorCreditState {
  u64 frame_credit = 0;
  u64 row_credit = 0;
  u64 byte_credit = 0;
  bool backpressure_active = true;
};

struct StreamingCursorState {
  std::string cursor_id;
  std::string plan_result_contract_hash;
  u64 catalog_epoch = 0;
  u64 descriptor_epoch = 0;
  std::string transaction_snapshot_class;
  std::string transaction_uuid;
  u64 local_transaction_id = 0;
  u64 snapshot_visible_through_local_transaction_id = 0;
  u64 security_epoch = 0;
  u64 redaction_epoch = 0;
  std::string route_kind;
  u64 frame_sequence = 0;
  u64 expiry_deadline_unix_millis = 0;
  bool cancellation_requested = false;
  StreamingCursorCreditState client_credit;
  bool mga_visibility_or_finality_authority = false;
  bool advisory_metadata_only = true;
  memory::ResultCursorPlanMemoryGovernor* memory_governor = nullptr;
  memory::HierarchicalMemoryBudgetLedger* memory_ledger = nullptr;
  memory::ResultCursorPlanMemoryPolicy memory_policy;
  memory::ResultCursorPlanMemoryScope memory_scope;
  memory::ResultCursorPlanMemoryEpochs memory_epochs;
  std::string memory_lease_id;
  u64 cursor_memory_bytes = 0;
  u64 outstanding_frame_bytes = 0;
  u64 outstanding_frame_count = 0;
};

struct StreamingCursorResult {
  Status status;
  bool fail_closed = false;
  StreamingCursorState state;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct StreamingCursorBinding {
  std::string cursor_id;
  std::string plan_result_contract_hash;
  u64 catalog_epoch = 0;
  u64 descriptor_epoch = 0;
  std::string transaction_snapshot_class;
  std::string transaction_uuid;
  u64 local_transaction_id = 0;
  u64 snapshot_visible_through_local_transaction_id = 0;
  u64 security_epoch = 0;
  u64 redaction_epoch = 0;
  std::string route_kind;
  u64 frame_sequence = 0;
  u64 expiry_deadline_unix_millis = 0;
};

struct StreamingCursorOpenRequest {
  StreamingCursorState state;
  u64 now_unix_millis = 0;
  u64 cursor_memory_bytes = 0;
  bool require_memory_governance = false;
  bool cluster_route_requested = false;
};

struct StreamingCursorFetchRequest {
  StreamingCursorBinding expected;
  u64 now_unix_millis = 0;
};

struct StreamingCursorFrameDelivery {
  StreamingCursorBinding expected;
  u64 row_count = 0;
  u64 byte_count = 0;
  u64 now_unix_millis = 0;
  bool require_memory_governance = false;
};

class StreamingCursorManager {
 public:
  StreamingCursorResult OpenCursor(const StreamingCursorOpenRequest& request);
  StreamingCursorResult GrantCredit(const std::string& cursor_id,
                                    StreamingCursorCreditState credit);
  StreamingCursorResult CancelCursor(const std::string& cursor_id);
  StreamingCursorResult ValidateFetch(
      const StreamingCursorFetchRequest& request) const;
  StreamingCursorResult RecordFrameDelivery(
      const StreamingCursorFrameDelivery& delivery);
  std::optional<StreamingCursorState> Lookup(const std::string& cursor_id) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, StreamingCursorState> cursors_;
};

struct ContinuationTokenSecret {
  std::string key_id;
  std::string secret_material;
};

struct ContinuationTokenResult {
  Status status;
  bool fail_closed = false;
  std::string token;
  StreamingCursorBinding binding;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

StreamingCursorBinding StreamingCursorBindingFromState(
    const StreamingCursorState& state);

ContinuationTokenResult IssueContinuationToken(
    const StreamingCursorBinding& binding,
    const ContinuationTokenSecret& secret);

ContinuationTokenResult ValidateContinuationToken(
    const std::string& token,
    const StreamingCursorBinding& expected,
    const ContinuationTokenSecret& secret,
    u64 now_unix_millis);

}  // namespace scratchbird::wire
