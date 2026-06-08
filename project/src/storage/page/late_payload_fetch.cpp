// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-LATE-PAYLOAD-FETCH-CONTRACT-ANCHOR
#include "late_payload_fetch.hpp"

#include "uuid.hpp"

#include <utility>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status ErrorStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::storage_page};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() && left.kind == right.kind &&
         left.value == right.value;
}

bool ValidRowUuid(const TypedUuid& value) {
  return value.valid() && value.kind == UuidKind::row;
}

std::string UuidText(const TypedUuid& value) {
  if (!value.valid()) {
    return "invalid";
  }
  return scratchbird::core::uuid::UuidToString(value.value);
}

bool DescriptorMatchesRecord(const LargePayloadDescriptor& descriptor,
                             const LargePayloadGenerationRecord& record) {
  return SameUuid(descriptor.payload_uuid, record.descriptor.payload_uuid) &&
         SameUuid(descriptor.owner_object_uuid,
                  record.descriptor.owner_object_uuid) &&
         SameUuid(descriptor.generation_scope_uuid,
                  record.descriptor.generation_scope_uuid) &&
         SameUuid(descriptor.filespace_uuid, record.descriptor.filespace_uuid) &&
         descriptor.generation == record.descriptor.generation &&
         descriptor.creator_local_transaction_id ==
             record.descriptor.creator_local_transaction_id &&
         descriptor.byte_count == record.descriptor.byte_count &&
         descriptor.content_hash == record.descriptor.content_hash &&
         descriptor.filespace_class == record.descriptor.filespace_class &&
         descriptor.page_family == record.descriptor.page_family &&
         descriptor.inline_payload == record.descriptor.inline_payload &&
         descriptor.family == record.descriptor.family;
}

void AppendBaseEvidence(std::vector<std::string>* evidence,
                        const LatePayloadReference& reference) {
  evidence->push_back("late_payload_fetch.row_uuid=" +
                      UuidText(reference.row_uuid));
  evidence->push_back("late_payload_fetch.descriptor_checked=true");
  evidence->push_back("payload_finality_authority=false");
  evidence->push_back("payload_visibility_authority=false");
  evidence->push_back("mga_visibility_authority=engine_transaction_inventory");
}

LatePayloadFetchResult Refuse(const LatePayloadReference& reference,
                              std::string diagnostic_code,
                              std::string message_key,
                              std::string detail) {
  LatePayloadFetchResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.row_uuid = reference.row_uuid;
  result.descriptor = reference.descriptor;
  AppendBaseEvidence(&result.evidence, reference);
  result.evidence.push_back("late_payload_fetch.fail_closed=true");
  result.evidence.push_back("late_payload_fetch.refused=" + diagnostic_code);
  result.diagnostic =
      MakeLatePayloadFetchDiagnostic(result.status, std::move(diagnostic_code),
                                     std::move(message_key), std::move(detail));
  return result;
}

bool HasUnsafeAuthorityDrift(const LatePayloadReference& reference) {
  return reference.parser_or_donor_finality_or_visibility_authority ||
         reference.client_finality_or_visibility_authority ||
         reference.provider_finality_or_visibility_authority ||
         reference.wal_recovery_or_finality_authority;
}

LatePayloadFetchResult ValidateReference(
    const LatePayloadFetchRequest& request,
    const LargePayloadGenerationRecord** matched_record) {
  const auto& reference = request.reference;
  if (request.large_payload_store == nullptr) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.MISSING_STORE",
                  "storage.page.late_payload_fetch.missing_store",
                  "large payload store is required");
  }
  if (!request.requester_final_authorized_and_pruned) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.FINAL_ROWS_REQUIRED",
                  "storage.page.late_payload_fetch.final_rows_required",
                  "payload fetch requires final authorized top-k row");
  }
  if (HasUnsafeAuthorityDrift(reference)) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.UNSAFE_AUTHORITY",
                  "storage.page.late_payload_fetch.unsafe_authority",
                  "payload descriptor attempted visibility or finality authority");
  }
  if (!ValidRowUuid(reference.row_uuid)) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.ROW_UUID_REQUIRED",
                  "storage.page.late_payload_fetch.row_uuid_required",
                  "exact row UUID is required");
  }
  if (!reference.descriptor.payload_uuid.valid() ||
      reference.descriptor.generation == 0 ||
      reference.descriptor.byte_count == 0) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.DESCRIPTOR_STALE",
                  "storage.page.late_payload_fetch.descriptor_stale",
                  "payload descriptor is missing durable identity");
  }
  if (!reference.descriptor_evidence_present || !reference.descriptor_fresh) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.DESCRIPTOR_STALE",
                  "storage.page.late_payload_fetch.descriptor_stale",
                  "fresh descriptor evidence is required");
  }
  if (!reference.exact_predicate_rechecked_by_engine ||
      !reference.mga_visibility_rechecked_by_engine ||
      !reference.security_authorized_by_engine) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.RECHECK_REQUIRED",
                  "storage.page.late_payload_fetch.recheck_required",
                  "exact, MGA, and security recheck evidence is required");
  }
  if (!reference.security_snapshot_bound || !reference.redaction_policy_bound) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.REDACTION_GATE_REQUIRED",
                  "storage.page.late_payload_fetch.redaction_gate_required",
                  "security snapshot and redaction policy must be bound");
  }
  if (reference.observer_snapshot_visible_through_local_transaction_id == 0) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.MGA_SNAPSHOT_REQUIRED",
                  "storage.page.late_payload_fetch.mga_snapshot_required",
                  "MGA snapshot visibility boundary is required");
  }

  const auto* record =
      FindLargePayloadGeneration(*request.large_payload_store,
                                 reference.descriptor);
  if (record == nullptr || !DescriptorMatchesRecord(reference.descriptor, *record)) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.DESCRIPTOR_STALE",
                  "storage.page.late_payload_fetch.descriptor_stale",
                  "payload descriptor no longer matches stored generation");
  }
  *matched_record = record;
  LatePayloadFetchResult ok;
  ok.status = OkStatus();
  return ok;
}

}  // namespace

LatePayloadFetchResult FetchLateMaterializationPayload(
    const LatePayloadFetchRequest& request) {
  const LargePayloadGenerationRecord* matched_record = nullptr;
  auto validation = ValidateReference(request, &matched_record);
  if (!validation.ok()) {
    return validation;
  }

  const auto& reference = request.reference;
  if (reference.redaction_required) {
    if (request.allow_full_payload_bytes) {
      return Refuse(reference,
                    "SB_LATE_PAYLOAD_FETCH.UNREDACTED_PROTECTED_PAYLOAD",
                    "storage.page.late_payload_fetch.unredacted_protected",
                    "redacted payload requested with full bytes enabled");
    }
    LatePayloadFetchResult result;
    result.status = OkStatus();
    result.row_uuid = reference.row_uuid;
    result.descriptor = reference.descriptor;
    result.redacted = true;
    AppendBaseEvidence(&result.evidence, reference);
    result.evidence.push_back("late_payload_fetch.redacted=true");
    result.evidence.push_back("late_payload_fetch.payload_bytes_exposed=false");
    result.evidence.push_back("late_payload_fetch.cache_not_touched=true");
    result.diagnostic = MakeLatePayloadFetchDiagnostic(
        result.status, "ok", "storage.page.late_payload_fetch.redacted",
        reference.redaction_reason.empty() ? "payload redacted by policy"
                                           : reference.redaction_reason);
    return result;
  }

  if (reference.protected_payload &&
      !reference.unredacted_payload_authorized_by_security) {
    return Refuse(reference,
                  "SB_LATE_PAYLOAD_FETCH.UNREDACTED_PROTECTED_PAYLOAD",
                  "storage.page.late_payload_fetch.unredacted_protected",
                  "protected payload lacks unredacted authorization");
  }
  if (!request.allow_full_payload_bytes) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.FULL_PAYLOAD_NOT_ALLOWED",
                  "storage.page.late_payload_fetch.full_payload_not_allowed",
                  "full payload bytes were not authorized by caller");
  }

  LargePayloadReadRequest read;
  read.descriptor = reference.descriptor;
  read.observer_snapshot_visible_through_local_transaction_id =
      reference.observer_snapshot_visible_through_local_transaction_id;
  read.use_cache = request.use_cache;
  read.prefetch_on_miss = request.prefetch_on_miss;
  read.reason = request.reason.empty()
                    ? "late_materialization;diagnostic_only=true;finality_authority=false;visibility_authority=false"
                    : request.reason;
  auto loaded = ReadLargePayloadGeneration(request.large_payload_store, read);
  if (!loaded.ok()) {
    return Refuse(reference, "SB_LATE_PAYLOAD_FETCH.READ_REFUSED",
                  "storage.page.late_payload_fetch.read_refused",
                  loaded.diagnostic.diagnostic_code);
  }

  LatePayloadFetchResult result;
  result.status = OkStatus();
  result.row_uuid = reference.row_uuid;
  result.descriptor = reference.descriptor;
  result.fetched = true;
  result.payload_bytes = std::move(loaded.payload_bytes);
  AppendBaseEvidence(&result.evidence, reference);
  result.evidence.push_back("late_payload_fetch.full_payload=true");
  result.evidence.push_back("late_payload_fetch.payload_bytes=" +
                            std::to_string(result.payload_bytes.size()));
  result.evidence.push_back(std::string("late_payload_fetch.cache_hit=") +
                            (loaded.cache_hit ? "true" : "false"));
  result.diagnostic = MakeLatePayloadFetchDiagnostic(
      result.status, "ok", "storage.page.late_payload_fetch.fetched",
      "payload fetched after final row authorization and pruning");
  (void)matched_record;
  return result;
}

DiagnosticRecord MakeLatePayloadFetchDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.late_payload_fetch",
                        status.ok() ? "" : "fail closed before exposing payload bytes");
}

}  // namespace scratchbird::storage::page
