// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction/transaction_inspect_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "local_transaction_store.hpp"
#include "security/security_model.hpp"
#include "transaction_evidence.hpp"
#include "transaction_inventory.hpp"
#include "transaction_recovery.hpp"
#include "uuid.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::storage::database::PersistLocalTransactionInventoryToDatabase;
using scratchbird::transaction::mga::BuildTransactionLineageEvidence;
using scratchbird::transaction::mga::BeginLocalReadOnlyTransaction;
using scratchbird::transaction::mga::ClassifyLocalTransactionForRecovery;
using scratchbird::transaction::mga::ClassifyTransactionInventoryForRestore;
using scratchbird::transaction::mga::LocalTransactionInventory;
using scratchbird::transaction::mga::TransactionInventoryEntry;
using scratchbird::transaction::mga::TransactionLineageEvidenceRecord;
using scratchbird::transaction::mga::TransactionRecoveryActionName;
using scratchbird::transaction::mga::TransactionState;
using scratchbird::transaction::mga::TransactionStateName;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateDurableEngineIdentityV7;
using scratchbird::core::uuid::UuidToString;

EngineApiDiagnostic DiagnosticFromMGA(const DiagnosticRecord& diagnostic,
                                      const std::string& fallback_code,
                                      const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty() ? fallback_code : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key : diagnostic.message_key,
                                 detail,
                                 true);
}

template <typename TResult>
TResult TransactionInspectFailure(const EngineApiRequest& request,
                                  const std::string& operation_id,
                                  EngineApiDiagnostic diagnostic) {
  return MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, std::move(diagnostic));
}

bool HasInspectRight(const EngineRequestContext& context, const std::string& right) {
  return SecurityContextHasTag(context, "security.bootstrap") ||
         SecurityContextHasRight(context, right) ||
         SecurityContextHasRight(context, "MGA_TRANSACTION_INSPECT") ||
         SecurityContextHasRight(context, "OBS_RUNTIME_ALL");
}

template <typename TResult>
std::optional<TResult> EnforceInspectRight(const EngineApiRequest& request,
                                          const std::string& operation_id,
                                          const std::string& right) {
  if (!HasInspectRight(request.context, right)) {
    return TransactionInspectFailure<TResult>(
        request,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", right));
  }
  return std::nullopt;
}

std::string SchemaEpochFor(const EngineApiRequest& request) {
  const auto explicit_epoch = SecurityOptionValue(request, "schema_epoch:");
  if (!explicit_epoch.empty()) { return explicit_epoch; }
  if (request.context.catalog_generation_id != 0) { return std::to_string(request.context.catalog_generation_id); }
  return "local_schema_epoch_unspecified";
}

std::string SnapshotCapsuleFor(const EngineApiRequest& request) {
  const auto explicit_capsule = SecurityOptionValue(request, "snapshot_capsule:");
  if (!explicit_capsule.empty()) { return explicit_capsule; }
  if (!request.context.transaction_uuid.canonical.empty()) {
    return "transaction:" + request.context.transaction_uuid.canonical;
  }
  if (request.context.local_transaction_id != 0) {
    return "local_transaction:" + std::to_string(request.context.local_transaction_id);
  }
  return "local_latest";
}

void AddLineageRow(EngineApiResult* result, const TransactionLineageEvidenceRecord& record) {
  AddApiBehaviorRow(result,
                    {{"local_transaction_id", std::to_string(record.local_id.value)},
                     {"transaction_uuid", record.transaction_uuid},
                     {"event_class", record.event_class},
                     {"observed_state", record.observed_state},
                     {"terminal_state", record.terminal_state},
                     {"schema_epoch", record.schema_epoch},
                     {"snapshot_capsule", record.snapshot_capsule},
                     {"restore_classification", record.restore_classification},
                     {"refusal_condition", record.refusal_condition},
                     {"terminal", record.terminal ? "true" : "false"},
                     {"evidence_written", record.evidence_written ? "true" : "false"},
                     {"wal_required", record.wal_required ? "true" : "false"}});
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t ParseU64OrZero(const std::string& value) {
  std::uint64_t parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') { return 0; }
    parsed = (parsed * 10U) + static_cast<std::uint64_t>(ch - '0');
  }
  return parsed;
}

std::uint64_t EffectiveTargetLocalTransactionId(const EngineLocateTransactionRequest& request) {
  if (request.target_local_transaction_id != 0) {
    return request.target_local_transaction_id;
  }
  return ParseU64OrZero(SecurityOptionValue(request, "target_local_transaction_id:"));
}

std::string EffectiveTargetTransactionUuid(const EngineLocateTransactionRequest& request) {
  if (!request.target_transaction_uuid.canonical.empty()) {
    return request.target_transaction_uuid.canonical;
  }
  return SecurityOptionValue(request, "target_transaction_uuid:");
}

std::string EffectiveRequestedLocationClass(const EngineLocateTransactionRequest& request) {
  if (!request.requested_location_class.empty()) {
    return SecurityLower(request.requested_location_class);
  }
  return SecurityLower(SecurityOptionValue(request, "transaction_location_class:"));
}

bool RemoteLocationRequested(const EngineLocateTransactionRequest& request) {
  return request.remote_location_requested ||
         EffectiveRequestedLocationClass(request) == "remote" ||
         SecurityOptionBool(request, "remote_transaction_location:", false);
}

bool RetiredHistoryEvidencePresent(const EngineLocateTransactionRequest& request) {
  return request.retired_history_evidence_present ||
         EffectiveRequestedLocationClass(request) == "retired" ||
         SecurityOptionBool(request, "retired_history_evidence:", false);
}

EngineApiDiagnostic AuditLocationDiagnostic(std::string code,
                                            std::string detail,
                                            bool error = true) {
  return MakeEngineApiDiagnostic(std::move(code),
                                 "mga.audit_transaction_location",
                                 std::move(detail),
                                 error);
}

struct TransactionLocationResolution {
  std::uint64_t target_local_transaction_id = 0;
  std::string target_transaction_uuid;
  std::string location_class = "unknown";
  std::string transaction_state = "unknown";
  bool queryable = false;
  bool writes_refused = true;
  bool fail_closed = true;
  bool local_inventory_authoritative = false;
  bool archive_authoritative = false;
  bool external_cluster_provider_required = false;
  EngineApiDiagnostic diagnostic =
      AuditLocationDiagnostic("ENGINE.MGA_AUDIT_LOCATION_UNKNOWN",
                              "target_transaction_unknown",
                              true);
};

const TransactionInventoryEntry* FindTransactionEntry(
    const LocalTransactionInventory& inventory,
    std::uint64_t local_transaction_id,
    const std::string& transaction_uuid) {
  for (const auto& entry : inventory.entries) {
    const bool id_matches =
        local_transaction_id != 0 &&
        entry.identity.local_id.value == local_transaction_id;
    const bool uuid_matches =
        !transaction_uuid.empty() &&
        entry.identity.transaction_uuid.valid() &&
        UuidToString(entry.identity.transaction_uuid.value) == transaction_uuid;
    if (id_matches || uuid_matches) {
      return &entry;
    }
  }
  return nullptr;
}

bool EntryQueryableForAudit(const TransactionInventoryEntry& entry) {
  return entry.state == TransactionState::committed ||
         entry.state == TransactionState::archived;
}

TransactionLocationResolution ResolveTransactionLocation(
    const EngineLocateTransactionRequest& request,
    const LocalTransactionInventory& inventory) {
  TransactionLocationResolution resolved;
  resolved.target_local_transaction_id = EffectiveTargetLocalTransactionId(request);
  resolved.target_transaction_uuid = EffectiveTargetTransactionUuid(request);

  if (RemoteLocationRequested(request)) {
    resolved.location_class = "remote";
    resolved.fail_closed = true;
    resolved.queryable = false;
    resolved.external_cluster_provider_required = true;
    resolved.diagnostic = AuditLocationDiagnostic(
        request.context.cluster_authority_available
            ? "ENGINE.MGA_AUDIT_REMOTE_EXTERNAL_PROVIDER_REQUIRED"
            : "ENGINE.MGA_AUDIT_REMOTE_CLUSTER_PROVIDER_UNAVAILABLE",
        request.context.cluster_authority_available
            ? "remote_transaction_requires_external_cluster_provider"
            : "remote_transaction_no_cluster_provider_available",
        true);
    return resolved;
  }

  if (RetiredHistoryEvidencePresent(request)) {
    resolved.location_class = "retired";
    resolved.fail_closed = true;
    resolved.queryable = false;
    resolved.diagnostic = AuditLocationDiagnostic(
        "ENGINE.MGA_AUDIT_RETIRED_HISTORY_NOT_QUERYABLE",
        "retired_history_requires_restore_or_external_archive_evidence",
        true);
    return resolved;
  }

  if (resolved.target_local_transaction_id == 0 &&
      resolved.target_transaction_uuid.empty()) {
    resolved.diagnostic = AuditLocationDiagnostic(
        "ENGINE.MGA_AUDIT_LOCATION_TARGET_REQUIRED",
        "target_local_transaction_id_or_transaction_uuid_required",
        true);
    return resolved;
  }

  const TransactionInventoryEntry* entry =
      FindTransactionEntry(inventory,
                           resolved.target_local_transaction_id,
                           resolved.target_transaction_uuid);
  if (entry == nullptr) {
    resolved.diagnostic = AuditLocationDiagnostic(
        "ENGINE.MGA_AUDIT_LOCATION_UNKNOWN",
        "target_transaction_not_found_in_durable_inventory",
        true);
    return resolved;
  }

  const std::string entry_uuid = entry->identity.transaction_uuid.valid()
      ? UuidToString(entry->identity.transaction_uuid.value)
      : "";
  if (resolved.target_local_transaction_id != 0 &&
      resolved.target_transaction_uuid.empty()) {
    resolved.target_transaction_uuid = entry_uuid;
  }
  if (resolved.target_local_transaction_id == 0) {
    resolved.target_local_transaction_id = entry->identity.local_id.value;
  }
  if (!resolved.target_transaction_uuid.empty() &&
      !entry_uuid.empty() &&
      resolved.target_transaction_uuid != entry_uuid) {
    resolved.location_class = "unknown";
    resolved.transaction_state = TransactionStateName(entry->state);
    resolved.diagnostic = AuditLocationDiagnostic(
        "ENGINE.MGA_AUDIT_LOCATION_IDENTITY_MISMATCH",
        "target_local_transaction_id_and_transaction_uuid_do_not_match",
        true);
    return resolved;
  }

  resolved.local_inventory_authoritative = true;
  resolved.transaction_state = TransactionStateName(entry->state);
  resolved.location_class =
      entry->state == TransactionState::archived ? "local_archive" : "local_hot";
  resolved.archive_authoritative = entry->state == TransactionState::archived;
  resolved.queryable = EntryQueryableForAudit(*entry);
  resolved.fail_closed = !resolved.queryable;
  resolved.diagnostic = resolved.queryable
      ? AuditLocationDiagnostic(
            entry->state == TransactionState::archived
                ? "ENGINE.MGA_AUDIT_LOCAL_ARCHIVE_QUERYABLE"
                : "ENGINE.MGA_AUDIT_LOCAL_HOT_QUERYABLE",
            "mga_inventory_location_queryable",
            false)
      : AuditLocationDiagnostic(
            "ENGINE.MGA_AUDIT_TRANSACTION_NOT_QUERYABLE",
            "target_transaction_state_not_queryable_as_audit_snapshot",
            true);
  return resolved;
}

void PopulateLocationResult(EngineLocateTransactionResult* result,
                            const TransactionLocationResolution& resolved) {
  result->target_local_transaction_id = resolved.target_local_transaction_id;
  result->target_transaction_uuid.canonical = resolved.target_transaction_uuid;
  result->location_class = resolved.location_class;
  result->transaction_state = resolved.transaction_state;
  result->queryable = resolved.queryable;
  result->writes_refused = resolved.writes_refused;
  result->fail_closed = resolved.fail_closed;
  result->local_inventory_authoritative = resolved.local_inventory_authoritative;
  result->archive_authoritative = resolved.archive_authoritative;
  result->external_cluster_provider_required =
      resolved.external_cluster_provider_required;
  result->location_diagnostic_code = resolved.diagnostic.code;
  result->location_diagnostic_detail = resolved.diagnostic.detail;
  AddApiBehaviorRow(result,
                    {{"target_local_transaction_id",
                      std::to_string(resolved.target_local_transaction_id)},
                     {"target_transaction_uuid", resolved.target_transaction_uuid},
                     {"location_class", resolved.location_class},
                     {"transaction_state", resolved.transaction_state},
                     {"queryable", resolved.queryable ? "true" : "false"},
                     {"writes_refused", resolved.writes_refused ? "true" : "false"},
                     {"fail_closed", resolved.fail_closed ? "true" : "false"},
                     {"local_inventory_authoritative",
                      resolved.local_inventory_authoritative ? "true" : "false"},
                     {"archive_authoritative",
                      resolved.archive_authoritative ? "true" : "false"},
                     {"external_cluster_provider_required",
                      resolved.external_cluster_provider_required ? "true" : "false"},
                     {"location_diagnostic_code", resolved.diagnostic.code},
                     {"mga_inventory_authority",
                      resolved.local_inventory_authoritative ? "true" : "false"},
                     {"parser_finality_authority", "false"},
                     {"reference_finality_authority", "false"}});
  AddApiBehaviorEvidence(result,
                         "mga_transaction_location",
                         resolved.location_class);
  AddApiBehaviorEvidence(result, "writes_refused", "true");
  AddApiBehaviorEvidence(result, "parser_finality", "false");
}

bool ContainsTransactionUuid(const LocalTransactionInventory& inventory,
                             const TypedUuid& transaction_uuid) {
  for (const auto& entry : inventory.entries) {
    if (entry.identity.transaction_uuid.valid() &&
        entry.identity.transaction_uuid.value == transaction_uuid.value) {
      return true;
    }
  }
  return false;
}

std::optional<TypedUuid> GenerateAuditTransactionUuid(
    const LocalTransactionInventory& inventory,
    std::uint64_t unix_epoch_millis,
    DiagnosticRecord* diagnostic) {
  const std::uint64_t attempts =
      static_cast<std::uint64_t>(inventory.entries.size()) + 8U;
  for (std::uint64_t attempt = 0; attempt < attempts; ++attempt) {
    const auto generated = GenerateDurableEngineIdentityV7(
        UuidKind::transaction,
        unix_epoch_millis + inventory.next_local_transaction_id + attempt);
    if (!generated.ok()) {
      *diagnostic = generated.diagnostic;
      return std::nullopt;
    }
    if (!ContainsTransactionUuid(inventory, generated.value)) {
      return generated.value;
    }
  }
  return std::nullopt;
}

EngineApiDiagnostic ValidateAuditAdmissionContext(
    const EngineBeginAuditReadTransactionRequest& request) {
  if (request.context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("transaction.begin_audit_read",
                                        "database_path_required");
  }
  if (request.context.local_transaction_id != 0 ||
      !request.context.transaction_uuid.canonical.empty()) {
    return AuditLocationDiagnostic(
        "ENGINE.MGA_AUDIT_ACTIVE_TRANSACTION_ALREADY_BOUND",
        "audit_read_requires_unbound_request_context",
        true);
  }
  if (request.context.catalog_generation_id == 0 ||
      request.context.security_epoch == 0 ||
      request.context.resource_epoch == 0 ||
      request.context.name_resolution_epoch == 0) {
    return AuditLocationDiagnostic(
        "ENGINE.MGA_AUDIT_AUTHORITY_GENERATION_REQUIRED",
        "catalog_security_resource_name_resolution_epochs_required",
        true);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

}  // namespace

EngineInspectTransactionLineageResult EngineInspectTransactionLineage(
    const EngineInspectTransactionLineageRequest& request) {
  const std::string operation_id = "transaction.inspect_lineage";
  if (auto denied = EnforceInspectRight<EngineInspectTransactionLineageResult>(
          request,
          operation_id,
          "MGA_LINEAGE_INSPECT")) {
    return *denied;
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return TransactionInspectFailure<EngineInspectTransactionLineageResult>(
        request,
        operation_id,
        DiagnosticFromMGA(loaded.diagnostic,
                          "SB-MGA-TXN-INV-LOAD-FAILED",
                          "mga.transaction_inventory.load_failed"));
  }
  auto result = MakeApiBehaviorSuccess<EngineInspectTransactionLineageResult>(request.context, operation_id);
  for (const auto& record : BuildTransactionLineageEvidence(loaded.inventory,
                                                            SchemaEpochFor(request),
                                                            SnapshotCapsuleFor(request))) {
    AddLineageRow(&result, record);
  }
  AddApiBehaviorEvidence(&result, "mga_lineage", "durable_transaction_inventory");
  AddApiBehaviorEvidence(&result, "wal_required", "false");
  return result;
}

EngineClassifyTransactionRestoreResult EngineClassifyTransactionRestore(
    const EngineClassifyTransactionRestoreRequest& request) {
  const std::string operation_id = "transaction.classify_restore";
  if (auto denied = EnforceInspectRight<EngineClassifyTransactionRestoreResult>(
          request,
          operation_id,
          "MGA_RECOVERY_INSPECT")) {
    return *denied;
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return TransactionInspectFailure<EngineClassifyTransactionRestoreResult>(
        request,
        operation_id,
        DiagnosticFromMGA(loaded.diagnostic,
                          "SB-MGA-TXN-INV-LOAD-FAILED",
                          "mga.transaction_inventory.load_failed"));
  }
  const auto classified = ClassifyTransactionInventoryForRestore(
      loaded.inventory,
      SchemaEpochFor(request),
      SnapshotCapsuleFor(request),
      SecurityOptionBool(request, "wal_required:", false));
  auto result = MakeApiBehaviorSuccess<EngineClassifyTransactionRestoreResult>(request.context, operation_id);
  result.restore_allowed = classified.restore_allowed;
  result.wal_required = classified.wal_required;
  if (!classified.ok()) {
    result.ok = false;
    result.diagnostics.push_back(DiagnosticFromMGA(classified.diagnostic,
                                                   "SB-MGA-RESTORE-CLASSIFICATION-REFUSED",
                                                   "transaction.evidence.restore_classification_refused"));
  }
  for (const auto& record : classified.records) {
    AddLineageRow(&result, record);
  }
  AddApiBehaviorEvidence(&result, "restore_classification", classified.restore_allowed ? "allowed" : "refused");
  AddApiBehaviorEvidence(&result, "wal_required", classified.wal_required ? "true" : "false");
  return result;
}

EngineInspectTransactionForensicsResult EngineInspectTransactionForensics(
    const EngineInspectTransactionForensicsRequest& request) {
  const std::string operation_id = "transaction.inspect_forensics";
  if (auto denied = EnforceInspectRight<EngineInspectTransactionForensicsResult>(
          request,
          operation_id,
          "MGA_FORENSIC_INSPECT")) {
    return *denied;
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return TransactionInspectFailure<EngineInspectTransactionForensicsResult>(
        request,
        operation_id,
        DiagnosticFromMGA(loaded.diagnostic,
                          "SB-MGA-TXN-INV-LOAD-FAILED",
                          "mga.transaction_inventory.load_failed"));
  }
  auto result = MakeApiBehaviorSuccess<EngineInspectTransactionForensicsResult>(request.context, operation_id);
  for (const auto& entry : loaded.inventory.entries) {
    const auto classification = ClassifyLocalTransactionForRecovery(entry);
    AddApiBehaviorRow(&result,
                      {{"local_transaction_id", std::to_string(entry.identity.local_id.value)},
                       {"transaction_uuid", entry.identity.transaction_uuid.valid()
                           ? UuidToString(entry.identity.transaction_uuid.value)
                           : ""},
                       {"observed_state", TransactionStateName(classification.observed_state)},
                       {"recovery_action", TransactionRecoveryActionName(classification.action)},
                       {"fail_closed", classification.fail_closed ? "true" : "false"},
                       {"stable_reason", classification.stable_reason},
                       {"rollback_only", entry.rollback_only ? "true" : "false"},
                       {"evidence_written", entry.evidence_record_written ? "true" : "false"}});
  }
  AddApiBehaviorEvidence(&result, "mga_forensics", "recovery_classification");
  AddApiBehaviorEvidence(&result, "wal_required", "false");
  return result;
}

EngineLocateTransactionResult EngineLocateTransaction(
    const EngineLocateTransactionRequest& request) {
  const std::string operation_id = "transaction.locate";
  if (auto denied = EnforceInspectRight<EngineLocateTransactionResult>(
          request,
          operation_id,
          "MGA_TRANSACTION_INSPECT")) {
    return *denied;
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return TransactionInspectFailure<EngineLocateTransactionResult>(
        request,
        operation_id,
        DiagnosticFromMGA(loaded.diagnostic,
                          "SB-MGA-TXN-INV-LOAD-FAILED",
                          "mga.transaction_inventory.load_failed"));
  }
  const auto location = ResolveTransactionLocation(request, loaded.inventory);
  auto result = MakeApiBehaviorSuccess<EngineLocateTransactionResult>(
      request.context,
      operation_id);
  PopulateLocationResult(&result, location);
  AddApiBehaviorEvidence(&result, "mga_authority", "durable_transaction_inventory");
  return result;
}

EngineBeginAuditReadTransactionResult EngineBeginAuditReadTransaction(
    const EngineBeginAuditReadTransactionRequest& request) {
  const std::string operation_id = "transaction.begin_audit_read";
  if (auto denied = EnforceInspectRight<EngineBeginAuditReadTransactionResult>(
          request,
          operation_id,
          "MGA_TRANSACTION_INSPECT")) {
    return *denied;
  }
  const auto admission = ValidateAuditAdmissionContext(request);
  if (admission.error) {
    return TransactionInspectFailure<EngineBeginAuditReadTransactionResult>(
        request,
        operation_id,
        admission);
  }

  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return TransactionInspectFailure<EngineBeginAuditReadTransactionResult>(
        request,
        operation_id,
        DiagnosticFromMGA(loaded.diagnostic,
                          "SB-MGA-TXN-INV-LOAD-FAILED",
                          "mga.transaction_inventory.load_failed"));
  }

  const auto location = ResolveTransactionLocation(request, loaded.inventory);
  if (!location.queryable) {
    auto result = TransactionInspectFailure<EngineBeginAuditReadTransactionResult>(
        request,
        operation_id,
        location.diagnostic);
    PopulateLocationResult(&result, location);
    AddApiBehaviorEvidence(&result,
                           "audit_read_admission",
                           "fail_closed_without_inventory_mutation");
    return result;
  }

  const std::uint64_t begin_unix_epoch_millis = CurrentUnixMillis();
  DiagnosticRecord generation_diagnostic;
  const auto generated_transaction_uuid =
      GenerateAuditTransactionUuid(loaded.inventory,
                                   begin_unix_epoch_millis,
                                   &generation_diagnostic);
  if (!generated_transaction_uuid.has_value()) {
    return TransactionInspectFailure<EngineBeginAuditReadTransactionResult>(
        request,
        operation_id,
        DiagnosticFromMGA(generation_diagnostic,
                          "SB-MGA-AUDIT-TXN-UUID-FAILED",
                          "mga.audit_transaction.uuid_generation_failed"));
  }

  auto begun = BeginLocalReadOnlyTransaction(loaded.inventory,
                                             *generated_transaction_uuid,
                                             begin_unix_epoch_millis);
  if (!begun.ok()) {
    return TransactionInspectFailure<EngineBeginAuditReadTransactionResult>(
        request,
        operation_id,
        DiagnosticFromMGA(begun.diagnostic,
                          "SB-MGA-AUDIT-TXN-BEGIN-FAILED",
                          "mga.audit_transaction.begin_failed"));
  }

  for (auto& entry : begun.inventory.entries) {
    if (entry.identity.local_id.value == begun.entry.identity.local_id.value) {
      entry.begin_visible_through_local_transaction_id =
          location.target_local_transaction_id;
      begun.entry = entry;
      break;
    }
  }

  const auto persisted = PersistLocalTransactionInventoryToDatabase(
      request.context.database_path,
      begun.inventory);
  if (!persisted.ok()) {
    return TransactionInspectFailure<EngineBeginAuditReadTransactionResult>(
        request,
        operation_id,
        DiagnosticFromMGA(persisted.diagnostic,
                          "SB-MGA-AUDIT-TXN-PERSIST-FAILED",
                          "mga.audit_transaction.persist_failed"));
  }

  auto result = MakeApiBehaviorSuccess<EngineBeginAuditReadTransactionResult>(
      request.context,
      operation_id);
  PopulateLocationResult(&result, location);
  result.audit_transaction_uuid.canonical =
      UuidToString(begun.entry.identity.transaction_uuid.value);
  result.audit_local_transaction_id = begun.entry.identity.local_id.value;
  result.snapshot_visible_through_local_transaction_id =
      begun.entry.begin_visible_through_local_transaction_id;
  result.audit_transaction_distinct =
      result.audit_local_transaction_id != location.target_local_transaction_id &&
      result.audit_transaction_uuid.canonical != location.target_transaction_uuid;
  result.read_only = true;
  result.writes_refused = true;
  result.fail_closed = false;
  result.transaction_uuid = result.audit_transaction_uuid;
  result.local_transaction_id = result.audit_local_transaction_id;
  AddApiBehaviorRow(&result,
                    {{"audit_local_transaction_id",
                      std::to_string(result.audit_local_transaction_id)},
                     {"audit_transaction_uuid",
                      result.audit_transaction_uuid.canonical},
                     {"audit_transaction_state",
                      TransactionStateName(begun.entry.state)},
                     {"audit_transaction_distinct",
                      result.audit_transaction_distinct ? "true" : "false"},
                     {"read_only", "true"},
                     {"writes_refused", "true"},
                     {"snapshot_visible_through_local_transaction_id",
                      std::to_string(
                          result.snapshot_visible_through_local_transaction_id)},
                     {"isolation_level", request.isolation_level},
                     {"mga_inventory_authority", "true"},
                     {"parser_finality_authority", "false"},
                     {"reference_finality_authority", "false"}});
  AddApiBehaviorEvidence(&result,
                         "audit_read_transaction",
                         "durable_mga_read_only_transaction");
  AddApiBehaviorEvidence(&result, "audit_read_write_guard", "read_only_active");
  AddApiBehaviorEvidence(&result, "mga_authority", "durable_transaction_inventory");
  return result;
}

}  // namespace scratchbird::engine::internal_api
