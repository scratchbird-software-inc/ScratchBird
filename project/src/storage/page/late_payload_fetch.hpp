// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-LATE-PAYLOAD-FETCH-CONTRACT-ANCHOR
#include "large_payload.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;

struct LatePayloadReference {
  TypedUuid row_uuid;
  LargePayloadDescriptor descriptor;
  u64 observer_snapshot_visible_through_local_transaction_id = 0;
  bool descriptor_evidence_present = false;
  bool descriptor_fresh = false;
  bool exact_predicate_rechecked_by_engine = false;
  bool mga_visibility_rechecked_by_engine = false;
  bool security_authorized_by_engine = false;
  bool security_snapshot_bound = false;
  bool redaction_policy_bound = false;
  bool protected_payload = false;
  bool redaction_required = false;
  bool unredacted_payload_authorized_by_security = false;
  bool parser_or_reference_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool wal_recovery_or_finality_authority = false;
  std::string redaction_reason;
};

struct LatePayloadFetchRequest {
  LargePayloadStore* large_payload_store = nullptr;
  LatePayloadReference reference;
  bool requester_final_authorized_and_pruned = false;
  bool allow_full_payload_bytes = false;
  bool use_cache = true;
  bool prefetch_on_miss = false;
  std::string reason;
};

struct LatePayloadFetchResult {
  Status status;
  bool fail_closed = false;
  bool fetched = false;
  bool redacted = false;
  TypedUuid row_uuid;
  LargePayloadDescriptor descriptor;
  std::vector<byte> payload_bytes;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

LatePayloadFetchResult FetchLateMaterializationPayload(
    const LatePayloadFetchRequest& request);

DiagnosticRecord MakeLatePayloadFetchDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::storage::page
