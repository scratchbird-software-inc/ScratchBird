// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLOUD_EXTERNAL_EFFECT_OUTBOX
// External effects are admitted only after MGA commit evidence. Provider
// delivery is never transaction, visibility, durability, or recovery authority.

struct EngineExternalEffectCommitEvidence {
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t transaction_inventory_generation = 0;
  std::string commit_evidence_hash;
  std::string finality_mode;
  bool mga_commit_visible = false;
  bool durable_commit_evidence = false;
};

struct EngineExternalEffectOutboxRecord {
  std::string outbox_event_uuid;
  std::string source_transaction_uuid;
  std::uint64_t source_local_transaction_id = 0;
  std::uint64_t transaction_inventory_generation = 0;
  std::string source_object_ref;
  std::string effect_class;
  std::string provider_profile_uuid;
  std::string idempotency_key;
  std::string redaction_policy_uuid;
  std::string payload_hash;
  std::string payload_metadata;
  std::uint64_t attempt_count = 0;
  std::uint64_t next_retry_epoch_ms = 0;
  std::uint64_t admitted_epoch_ms = 0;
  std::uint64_t delivered_epoch_ms = 0;
  std::string final_state = "pending";
  std::string audit_event_uuid;
  std::string last_diagnostic_code;
};

struct EngineExternalEffectOutboxAdmission {
  EngineExternalEffectCommitEvidence commit_evidence;
  std::string source_object_ref;
  std::string effect_class;
  std::string provider_profile_uuid;
  std::string idempotency_key;
  std::string redaction_policy_uuid;
  std::string payload_hash;
  std::string payload_metadata;
  std::string audit_event_uuid;
  std::uint64_t now_epoch_ms = 0;
  std::uint64_t max_pending_records = 1024;
};

struct EngineExternalEffectDeliveryAttempt {
  std::string idempotency_key;
  bool provider_success = false;
  bool retryable_failure = true;
  std::string diagnostic_code = "SB-EDGE-PROVIDER-DELIVERY-FAILED";
  std::uint64_t now_epoch_ms = 0;
  std::uint64_t max_attempts = 3;
  std::uint64_t retry_backoff_ms = 1000;
};

struct EngineExternalEffectOutboxResult {
  bool ok = false;
  bool deduplicated = false;
  EngineExternalEffectOutboxRecord record;
  std::vector<EngineApiDiagnostic> diagnostics;
};

EngineExternalEffectOutboxResult AdmitExternalEffectAfterCommit(
    const EngineExternalEffectOutboxAdmission& admission);
EngineExternalEffectOutboxResult RecordExternalEffectDeliveryAttempt(
    const EngineExternalEffectDeliveryAttempt& attempt);
std::vector<EngineExternalEffectOutboxRecord> InspectExternalEffectOutbox();
std::uint64_t PendingExternalEffectOutboxCount(const std::string& provider_profile_uuid);
void ResetExternalEffectOutboxForTests();

}  // namespace scratchbird::engine::internal_api
