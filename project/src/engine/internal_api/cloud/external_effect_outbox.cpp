// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/external_effect_outbox.hpp"

#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::vector<EngineExternalEffectOutboxRecord>& Outbox() {
  static std::vector<EngineExternalEffectOutboxRecord> outbox;
  return outbox;
}

std::mutex& OutboxMutex() {
  static std::mutex mutex;
  return mutex;
}

std::uint64_t& OutboxSequence() {
  static std::uint64_t sequence = 0;
  return sequence;
}

EngineApiDiagnostic Diagnostic(std::string code, std::string detail, bool error = true) {
  EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(code);
  diagnostic.message_key = diagnostic.code;
  diagnostic.detail = std::move(detail);
  diagnostic.error = error;
  return diagnostic;
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= kFnvPrime;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableId(std::string_view prefix, std::string_view material) {
  return std::string(prefix) + Hex64(Fnv1a64(material));
}

bool HasAfterCommitEvidence(const EngineExternalEffectCommitEvidence& evidence) {
  return evidence.mga_commit_visible &&
         evidence.durable_commit_evidence &&
         !evidence.transaction_uuid.empty() &&
         evidence.local_transaction_id > 0 &&
         evidence.transaction_inventory_generation > 0 &&
         !evidence.commit_evidence_hash.empty() &&
         !evidence.finality_mode.empty();
}

bool IsPendingState(std::string_view state) {
  return state == "pending" || state == "retry_pending";
}

EngineExternalEffectOutboxRecord* FindByIdempotencyKeyLocked(
    const std::string& idempotency_key) {
  for (auto& record : Outbox()) {
    if (record.idempotency_key == idempotency_key) {
      return &record;
    }
  }
  return nullptr;
}

std::uint64_t PendingCountLocked(const std::string& provider_profile_uuid) {
  std::uint64_t count = 0;
  for (const auto& record : Outbox()) {
    if (!provider_profile_uuid.empty() &&
        record.provider_profile_uuid != provider_profile_uuid) {
      continue;
    }
    if (IsPendingState(record.final_state)) {
      ++count;
    }
  }
  return count;
}

}  // namespace

EngineExternalEffectOutboxResult AdmitExternalEffectAfterCommit(
    const EngineExternalEffectOutboxAdmission& admission) {
  EngineExternalEffectOutboxResult result;

  if (!HasAfterCommitEvidence(admission.commit_evidence)) {
    result.diagnostics.push_back(Diagnostic("SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED",
                                            "durable_mga_commit_evidence_required"));
    return result;
  }
  if (admission.effect_class.empty() ||
      admission.provider_profile_uuid.empty() ||
      admission.idempotency_key.empty() ||
      admission.redaction_policy_uuid.empty() ||
      admission.payload_hash.empty()) {
    result.diagnostics.push_back(Diagnostic("SB-EDGE-OUTBOX-ADMISSION-INVALID",
                                            "effect_provider_idempotency_redaction_payload_required"));
    return result;
  }

  std::lock_guard<std::mutex> lock(OutboxMutex());
  if (auto* existing = FindByIdempotencyKeyLocked(admission.idempotency_key)) {
    result.ok = true;
    result.deduplicated = true;
    result.record = *existing;
    return result;
  }
  if (PendingCountLocked(admission.provider_profile_uuid) >= admission.max_pending_records) {
    result.diagnostics.push_back(Diagnostic("SB-EDGE-OUTBOX-BACKPRESSURE",
                                            "provider_pending_outbox_limit_exceeded"));
    return result;
  }

  const std::uint64_t sequence = ++OutboxSequence();
  EngineExternalEffectOutboxRecord record;
  record.outbox_event_uuid = StableId("edge-outbox-",
                                      admission.idempotency_key + "|" +
                                          std::to_string(sequence));
  record.source_transaction_uuid = admission.commit_evidence.transaction_uuid;
  record.source_local_transaction_id = admission.commit_evidence.local_transaction_id;
  record.transaction_inventory_generation =
      admission.commit_evidence.transaction_inventory_generation;
  record.source_object_ref = admission.source_object_ref;
  record.effect_class = admission.effect_class;
  record.provider_profile_uuid = admission.provider_profile_uuid;
  record.idempotency_key = admission.idempotency_key;
  record.redaction_policy_uuid = admission.redaction_policy_uuid;
  record.payload_hash = admission.payload_hash;
  record.payload_metadata = admission.payload_metadata;
  record.admitted_epoch_ms = admission.now_epoch_ms;
  record.final_state = "pending";
  record.audit_event_uuid =
      admission.audit_event_uuid.empty()
          ? StableId("edge-audit-", admission.idempotency_key)
          : admission.audit_event_uuid;

  Outbox().push_back(record);
  result.ok = true;
  result.record = record;
  return result;
}

EngineExternalEffectOutboxResult RecordExternalEffectDeliveryAttempt(
    const EngineExternalEffectDeliveryAttempt& attempt) {
  EngineExternalEffectOutboxResult result;
  if (attempt.idempotency_key.empty()) {
    result.diagnostics.push_back(Diagnostic("SB-EDGE-OUTBOX-ADMISSION-INVALID",
                                            "idempotency_key_required"));
    return result;
  }

  std::lock_guard<std::mutex> lock(OutboxMutex());
  auto* record = FindByIdempotencyKeyLocked(attempt.idempotency_key);
  if (record == nullptr) {
    result.diagnostics.push_back(Diagnostic("SB-EDGE-OUTBOX-ADMISSION-INVALID",
                                            "idempotency_key_not_found"));
    return result;
  }

  if (record->final_state == "delivered") {
    result.ok = true;
    result.deduplicated = true;
    result.record = *record;
    return result;
  }

  ++record->attempt_count;
  if (attempt.provider_success) {
    record->final_state = "delivered";
    record->delivered_epoch_ms = attempt.now_epoch_ms;
    record->next_retry_epoch_ms = 0;
    record->last_diagnostic_code.clear();
    result.ok = true;
    result.record = *record;
    return result;
  }

  record->last_diagnostic_code = attempt.diagnostic_code.empty()
                                     ? "SB-EDGE-PROVIDER-DELIVERY-FAILED"
                                     : attempt.diagnostic_code;
  if (attempt.retryable_failure && record->attempt_count < attempt.max_attempts) {
    record->final_state = "retry_pending";
    record->next_retry_epoch_ms =
        attempt.now_epoch_ms + (attempt.retry_backoff_ms * record->attempt_count);
  } else {
    record->final_state = "failed";
    record->next_retry_epoch_ms = 0;
  }

  result.record = *record;
  result.diagnostics.push_back(Diagnostic(record->last_diagnostic_code,
                                          "provider_delivery_failed"));
  return result;
}

std::vector<EngineExternalEffectOutboxRecord> InspectExternalEffectOutbox() {
  std::lock_guard<std::mutex> lock(OutboxMutex());
  return Outbox();
}

std::uint64_t PendingExternalEffectOutboxCount(const std::string& provider_profile_uuid) {
  std::lock_guard<std::mutex> lock(OutboxMutex());
  return PendingCountLocked(provider_profile_uuid);
}

void ResetExternalEffectOutboxForTests() {
  std::lock_guard<std::mutex> lock(OutboxMutex());
  Outbox().clear();
  OutboxSequence() = 0;
}

}  // namespace scratchbird::engine::internal_api
