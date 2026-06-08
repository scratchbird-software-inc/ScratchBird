// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_validation_repair_tooling.hpp"

// DPC_INDEX_VALIDATION_REPAIR_TOOLING

#include "uuid.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() { return {StatusCode::ok, Severity::warning, Subsystem::engine}; }
Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool GeneratedUuid(const TypedUuid& value, UuidKind expected_kind) {
  return value.kind == expected_kind && value.valid() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(value.value);
}

std::string UuidText(const TypedUuid& value) {
  if (!value.valid()) {
    return "invalid";
  }
  return scratchbird::core::uuid::UuidToString(value.value);
}

void AddEvidence(IndexValidationRepairResult* result,
                 std::string key,
                 std::string value,
                 bool sensitive = false) {
  result->support_evidence.push_back({std::move(key), std::move(value), sensitive});
}

void AddEvidence(IndexFamilyValidationRepairResult* result,
                 std::string key,
                 std::string value,
                 bool sensitive = false) {
  if (sensitive && result->redaction_required) {
    value = "<redacted-detail-present>";
  }
  result->support_evidence.push_back({std::move(key), std::move(value), sensitive});
}

void AddDiagnosticEvidence(IndexValidationRepairResult* result) {
  AddEvidence(result, "diagnostic_code", result->diagnostic.diagnostic_code);
  AddEvidence(result, "message_vector_key", result->diagnostic.message_key);
  AddEvidence(result, "diagnostic_source", result->diagnostic.source_component);
}

void AddDiagnosticEvidence(IndexFamilyValidationRepairResult* result) {
  AddEvidence(result, "diagnostic_code", result->diagnostic.diagnostic_code);
  AddEvidence(result, "message_vector_key", result->diagnostic.message_key);
  AddEvidence(result, "diagnostic_source", result->diagnostic.source_component);
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

IndexValidationRepairResult BaseResult(const IndexValidationRepairRequest& request) {
  IndexValidationRepairResult result;
  result.status = OkStatus();
  result.admitted = true;
  result.mutating = IndexValidationRepairOperationMutates(request.operation);
  result.validation_read_only =
      request.operation == IndexValidationRepairOperation::validate;
  result.redaction_required = !request.allow_sensitive_support_data;
  result.repaired_state = request.state;
  AddEvidence(&result, "dpc_search_key", kIndexValidationRepairToolingSearchKey);
  AddEvidence(&result, "operation", IndexValidationRepairOperationName(request.operation));
  AddEvidence(&result, "validation_family",
              IndexValidationRepairFamilyName(request.validation_family));
  AddEvidence(&result, "table_uuid", UuidText(request.target.table_uuid),
              result.redaction_required);
  AddEvidence(&result, "index_uuid", UuidText(request.target.index_uuid),
              result.redaction_required);
  return result;
}

IndexValidationRepairResult Refuse(const IndexValidationRepairRequest& request,
                                   std::string diagnostic_code,
                                   std::string message_key,
                                   std::string detail,
                                   bool fail_closed = true) {
  auto result = BaseResult(request);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = fail_closed;
  result.classification = fail_closed ? IndexValidationRepairClass::fail_closed
                                      : IndexValidationRepairClass::refused;
  result.diagnostic = MakeIndexValidationRepairDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  AddDiagnosticEvidence(&result);
  result.actions.push_back(fail_closed ? "refuse_and_fail_closed" : "refuse_route");
  return result;
}

void SetDiagnostic(IndexValidationRepairResult* result,
                   Status status,
                   IndexValidationRepairClass classification,
                   std::string diagnostic_code,
                   std::string message_key,
                   std::string detail = {}) {
  result->status = status;
  result->classification = classification;
  result->fail_closed = classification == IndexValidationRepairClass::fail_closed;
  result->diagnostic = MakeIndexValidationRepairDiagnostic(
      result->status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  AddDiagnosticEvidence(result);
}

bool TargetIdentityValid(const IndexValidationRepairTarget& target) {
  return target.names_resolved_to_uuids && target.catalog_resolution_proven &&
         !target.contains_sql_text &&
         GeneratedUuid(target.database_uuid, UuidKind::database) &&
         GeneratedUuid(target.table_uuid, UuidKind::object) &&
         GeneratedUuid(target.index_uuid, UuidKind::object) &&
         target.physical_family != IndexFamily::unknown;
}

bool TargetIdentityValidForFamily(const IndexValidationRepairTarget& target,
                                  IndexFamily family) {
  return TargetIdentityValid(target) && target.physical_family == family;
}

IndexFamilyValidationRepairPath PathForDescriptor(
    const IndexFamilyDescriptor& descriptor) {
  switch (descriptor.persistence) {
    case IndexPersistenceClass::persistent:
      return IndexFamilyValidationRepairPath::persistent_physical;
    case IndexPersistenceClass::memory_only:
      return IndexFamilyValidationRepairPath::memory_only_runtime;
    case IndexPersistenceClass::memory_primary_persisted_cold_start:
      return IndexFamilyValidationRepairPath::memory_primary_cold_start;
    case IndexPersistenceClass::donor_emulated:
      return IndexFamilyValidationRepairPath::donor_semantic_mapping;
    case IndexPersistenceClass::policy_blocked:
      return IndexFamilyValidationRepairPath::policy_refusal;
    case IndexPersistenceClass::virtual_catalog:
      return IndexFamilyValidationRepairPath::unavailable;
  }
  return IndexFamilyValidationRepairPath::unavailable;
}

IndexFamilyValidationRepairResult BaseFamilyResult(
    const IndexFamilyValidationRepairRequest& request,
    const IndexFamilyDescriptor* descriptor,
    const IndexFamilyPhysicalCapabilityState* state) {
  IndexFamilyValidationRepairResult result;
  result.status = OkStatus();
  result.family = request.family;
  result.family_id = descriptor ? descriptor->id : IndexFamilyName(request.family);
  result.blocker =
      state ? IndexFamilyPhysicalCapabilityBlockerName(state->blocker)
            : IndexFamilyPhysicalCapabilityBlockerName(
                  IndexFamilyPhysicalCapabilityBlocker::unknown_family);
  result.path = descriptor ? PathForDescriptor(*descriptor)
                           : IndexFamilyValidationRepairPath::unavailable;
  result.mutating = IndexValidationRepairOperationMutates(request.operation);
  result.validation_read_only =
      request.operation == IndexValidationRepairOperation::validate;
  result.redaction_required = !request.allow_sensitive_support_data;
  result.validation_state = "not_evaluated";
  result.repair_state = result.mutating ? "not_evaluated" : "not_requested";
  AddEvidence(&result, "operation",
              IndexValidationRepairOperationName(request.operation));
  AddEvidence(&result, "family_id", result.family_id);
  AddEvidence(&result, "family_name",
              descriptor ? descriptor->canonical_name : result.family_id);
  AddEvidence(&result, "path", IndexFamilyValidationRepairPathName(result.path));
  AddEvidence(&result, "blocker", result.blocker);
  AddEvidence(&result, "mutation_requested", BoolText(result.mutating));
  AddEvidence(&result, "validation_read_only",
              BoolText(result.validation_read_only));
  AddEvidence(&result, "read_only_database",
              BoolText(request.read_only_database));
  AddEvidence(&result, "policy_allows_mutation",
              BoolText(request.policy_allows_mutation));
  AddEvidence(&result, "catalog_uuid_binding_proven",
              BoolText(request.proof.catalog_uuid_binding_proven));
  AddEvidence(&result, "proof_token_present",
              BoolText(!request.proof.proof_token.empty()));
  if (!request.proof.sensitive_detail.empty()) {
    AddEvidence(&result, "sensitive_detail",
                request.proof.sensitive_detail, true);
  }
  AddEvidence(&result, "observational_only", BoolText(result.observational_only));
  AddEvidence(&result, "catalog_authority", BoolText(result.catalog_authority));
  AddEvidence(&result, "parser_authority", BoolText(result.parser_authority));
  AddEvidence(&result, "provider_authority", BoolText(result.provider_authority));
  AddEvidence(&result, "donor_authority", BoolText(result.donor_authority));
  AddEvidence(&result, "transaction_finality_authority",
              BoolText(result.transaction_finality_authority));
  AddEvidence(&result, "visibility_authority",
              BoolText(result.visibility_authority));
  AddEvidence(&result, "security_authority", BoolText(result.security_authority));
  AddEvidence(&result, "recovery_authority", BoolText(result.recovery_authority));
  AddEvidence(&result, "storage_authority", BoolText(result.storage_authority));
  if (state) {
    AddEvidence(&result, "declared_capability",
                BoolText(state->declared_capability));
    AddEvidence(&result, "planner_contract_capability",
                BoolText(state->planner_contract_capability));
    AddEvidence(&result, "implemented", BoolText(state->implemented));
    AddEvidence(&result, "physical_reader", BoolText(state->physical_reader));
    AddEvidence(&result, "physical_writer", BoolText(state->physical_writer));
    AddEvidence(&result, "maintenance", BoolText(state->maintenance));
    AddEvidence(&result, "validate", BoolText(state->validate));
    AddEvidence(&result, "repair", BoolText(state->repair));
    AddEvidence(&result, "recovery_reopen", BoolText(state->recovery_reopen));
    AddEvidence(&result, "rebuild", BoolText(state->rebuild));
    AddEvidence(&result, "runtime_available",
                BoolText(state->runtime_available));
    AddEvidence(&result, "benchmark_clean", BoolText(state->benchmark_clean));
    AddEvidence(&result, "blocker_diagnostic_code",
                state->blocker_diagnostic_code);
    AddEvidence(&result, "blocker_message_key", state->blocker_message_key);
    AddEvidence(&result, "blocker_detail", state->blocker_detail);
  }
  return result;
}

void SetFamilyDiagnostic(IndexFamilyValidationRepairResult* result,
                         Status status,
                         IndexValidationRepairClass classification,
                         IndexFamilyValidationRepairOpenState open_state,
                         std::string diagnostic_code,
                         std::string message_key,
                         std::string detail = {}) {
  result->status = status;
  result->classification = classification;
  result->open_state = open_state;
  result->open_allowed = open_state == IndexFamilyValidationRepairOpenState::open_allowed;
  result->open_refused = !result->open_allowed;
  result->fail_closed =
      classification == IndexValidationRepairClass::fail_closed ||
      open_state == IndexFamilyValidationRepairOpenState::open_refused ||
      open_state == IndexFamilyValidationRepairOpenState::non_physical_refused;
  result->diagnostic = MakeIndexValidationRepairDiagnostic(
      result->status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  AddEvidence(result, "classification",
              IndexValidationRepairClassName(result->classification));
  AddEvidence(result, "open_state",
              IndexFamilyValidationRepairOpenStateName(result->open_state));
  AddDiagnosticEvidence(result);
}

void SetFamilyCapabilityDiagnostic(
    IndexFamilyValidationRepairResult* result,
    Status status,
    IndexValidationRepairClass classification,
    IndexFamilyValidationRepairOpenState open_state,
    const IndexFamilyPhysicalCapabilityState& state) {
  result->status = status;
  result->classification = classification;
  result->open_state = open_state;
  result->open_allowed = false;
  result->open_refused = true;
  result->fail_closed = true;
  result->diagnostic = MakeIndexFamilyCapabilityBlockerDiagnostic(status, state);
  AddEvidence(result, "classification",
              IndexValidationRepairClassName(result->classification));
  AddEvidence(result, "open_state",
              IndexFamilyValidationRepairOpenStateName(result->open_state));
  AddDiagnosticEvidence(result);
}

void AddFamilyStateEvidence(IndexFamilyValidationRepairResult* result) {
  AddEvidence(result, "validation_state", result->validation_state);
  AddEvidence(result, "repair_state", result->repair_state);
  AddEvidence(result, "planner_visible", BoolText(result->planner_visible));
  AddEvidence(result, "mutation_applied", BoolText(result->mutation_applied));
  AddEvidence(result, "open_allowed", BoolText(result->open_allowed));
  AddEvidence(result, "open_refused", BoolText(result->open_refused));
}

bool PersistentProofPresent(const IndexFamilyValidationRepairProof& proof) {
  return proof.catalog_uuid_binding_proven &&
         proof.exact_base_table_source_present &&
         proof.physical_generation_present &&
         proof.physical_generation_checksum_valid &&
         proof.runtime_provider_attached &&
         proof.runtime_epoch_current;
}

bool MemoryRuntimeProofPresent(const IndexFamilyValidationRepairProof& proof) {
  return proof.catalog_uuid_binding_proven &&
         proof.runtime_provider_attached &&
         proof.runtime_epoch_current;
}

bool MemoryColdStartProofPresent(const IndexFamilyValidationRepairProof& proof) {
  return MemoryRuntimeProofPresent(proof) &&
         proof.cold_start_source_present &&
         proof.cold_start_checksum_valid;
}

bool RepairProofPresent(const IndexFamilyValidationRepairRequest& request) {
  if (request.operation == IndexValidationRepairOperation::repair) {
    return request.proof.repair_output_validated;
  }
  if (request.operation == IndexValidationRepairOperation::rebuild) {
    return request.proof.rebuild_output_validated;
  }
  if (request.operation == IndexValidationRepairOperation::discard_unpublished) {
    return request.proof.repair_output_validated ||
           request.proof.rebuild_output_validated;
  }
  return true;
}

void AdmitFamilyClean(IndexFamilyValidationRepairResult* result,
                      const IndexFamilyValidationRepairRequest& request,
                      std::string diagnostic_code,
                      std::string message_key,
                      std::string action) {
  result->admitted = true;
  result->planner_visible = true;
  result->mutation_applied =
      IndexValidationRepairOperationMutates(request.operation);
  result->validation_state = "validated";
  result->repair_state = result->mutation_applied ? "repaired_and_validated"
                                                  : "not_requested";
  SetFamilyDiagnostic(result, OkStatus(), IndexValidationRepairClass::clean,
                      IndexFamilyValidationRepairOpenState::open_allowed,
                      std::move(diagnostic_code), std::move(message_key));
  result->actions.push_back(std::move(action));
  AddFamilyStateEvidence(result);
}

IndexFamilyValidationRepairResult RefuseFamily(
    const IndexFamilyValidationRepairRequest& request,
    const IndexFamilyDescriptor* descriptor,
    const IndexFamilyPhysicalCapabilityState* state,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    IndexFamilyValidationRepairOpenState open_state =
        IndexFamilyValidationRepairOpenState::open_refused) {
  auto result = BaseFamilyResult(request, descriptor, state);
  SetFamilyDiagnostic(&result, RefuseStatus(),
                      IndexValidationRepairClass::fail_closed, open_state,
                      std::move(diagnostic_code), std::move(message_key),
                      std::move(detail));
  result.validation_state = "refused";
  result.repair_state = result.mutating ? "refused" : "not_requested";
  result.actions.push_back("refuse_and_fail_closed");
  AddFamilyStateEvidence(&result);
  return result;
}

IndexFamilyValidationRepairResult ExecuteFamilyWithCompleteCapability(
    const IndexFamilyValidationRepairRequest& request,
    const IndexFamilyDescriptor& descriptor,
    const IndexFamilyPhysicalCapabilityState& state) {
  auto result = BaseFamilyResult(request, &descriptor, &state);
  bool proof_present = false;
  switch (descriptor.persistence) {
    case IndexPersistenceClass::persistent:
      proof_present = PersistentProofPresent(request.proof);
      break;
    case IndexPersistenceClass::memory_only:
      proof_present = MemoryRuntimeProofPresent(request.proof);
      break;
    case IndexPersistenceClass::memory_primary_persisted_cold_start:
      proof_present = MemoryColdStartProofPresent(request.proof);
      break;
    case IndexPersistenceClass::virtual_catalog:
    case IndexPersistenceClass::donor_emulated:
    case IndexPersistenceClass::policy_blocked:
      proof_present = false;
      break;
  }

  if (!proof_present) {
    result.validation_state = "proof_missing";
    result.repair_state = result.mutating ? "proof_missing" : "not_requested";
    SetFamilyDiagnostic(
        &result, RefuseStatus(), IndexValidationRepairClass::fail_closed,
        IndexFamilyValidationRepairOpenState::open_refused,
        "IRC.INDEX_REPAIR.PROOF_REQUIRED",
        "irc.index_repair.proof_required",
        "family validation requires catalog UUID binding, generation, provider, epoch, and exact-source proof appropriate to the family");
    result.actions.push_back("family_hidden_until_required_proof_is_present");
    AddFamilyStateEvidence(&result);
    return result;
  }

  if (result.mutating && !RepairProofPresent(request)) {
    result.validation_state = "validated";
    result.repair_state = "repair_output_validation_required";
    SetFamilyDiagnostic(
        &result, RefuseStatus(), IndexValidationRepairClass::fail_closed,
        IndexFamilyValidationRepairOpenState::repair_required,
        "IRC.INDEX_REPAIR.REPAIR_OUTPUT_VALIDATION_REQUIRED",
        "irc.index_repair.repair_output_validation_required",
        "repair or rebuild output must validate before planner visibility is restored");
    result.actions.push_back("repair_output_hidden_until_validation_succeeds");
    AddFamilyStateEvidence(&result);
    return result;
  }

  if (descriptor.persistence == IndexPersistenceClass::memory_only) {
    AdmitFamilyClean(&result, request,
                     "IRC.INDEX_REPAIR.MEMORY_RUNTIME_VALID",
                     "irc.index_repair.memory_runtime_valid",
                     result.mutating
                         ? "memory_runtime_repaired_and_validated"
                         : "memory_runtime_validated_read_only");
  } else if (descriptor.persistence ==
             IndexPersistenceClass::memory_primary_persisted_cold_start) {
    AdmitFamilyClean(&result, request,
                     "IRC.INDEX_REPAIR.MEMORY_COLD_START_VALID",
                     "irc.index_repair.memory_cold_start_valid",
                     result.mutating
                         ? "memory_cold_start_repaired_and_validated"
                         : "memory_cold_start_validated_read_only");
  } else {
    AdmitFamilyClean(&result, request,
                     "IRC.INDEX_REPAIR.PERSISTENT_FAMILY_VALID",
                     "irc.index_repair.persistent_family_valid",
                     result.mutating
                         ? "persistent_family_repaired_and_validated"
                         : "persistent_family_validated_read_only");
  }
  return result;
}

IndexValidationRepairResult ValidateOrderedCandidateSet(
    const IndexValidationRepairRequest& request) {
  auto result = BaseResult(request);
  const auto verified =
      VerifyIndexCandidateSet(request.state.ordered_candidate_set);
  AddEvidence(&result, "ordered.expected_count",
              std::to_string(verified.expected_count));
  AddEvidence(&result, "ordered.observed_count",
              std::to_string(verified.observed_count));
  AddEvidence(&result, "ordered.missing_count",
              std::to_string(verified.missing_count));
  AddEvidence(&result, "ordered.extra_count",
              std::to_string(verified.extra_count));
  if (verified.ok()) {
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.ORDERED_VALID",
                  "dpc.index_repair.ordered_valid");
    result.actions.push_back("ordered_candidate_set_validated_read_only");
    result.planner_visible = true;
    return result;
  }

  SetDiagnostic(&result, RefuseStatus(),
                IndexValidationRepairClass::rebuild_required,
                "DPC.INDEX_REPAIR.ORDERED_REBUILD_REQUIRED",
                "dpc.index_repair.ordered_rebuild_required",
                verified.diagnostic.diagnostic_code);
  result.actions.push_back("ordered_index_hidden_until_rebuild_validates");
  if (request.operation == IndexValidationRepairOperation::rebuild) {
    result.repaired_state.ordered_candidate_set.observed_from_index =
        request.state.ordered_candidate_set.expected_from_table;
    result.mutation_applied = true;
    result.status = OkStatus();
    result.classification = IndexValidationRepairClass::clean;
    result.diagnostic = MakeIndexValidationRepairDiagnostic(
        result.status, "DPC.INDEX_REPAIR.ORDERED_REBUILT",
        "dpc.index_repair.ordered_rebuilt");
    AddDiagnosticEvidence(&result);
    result.actions.push_back("rebuilt_ordered_index_from_authoritative_table_candidates");
    result.planner_visible = true;
  }
  return result;
}

IndexValidationRepairResult ValidateDeltaLedger(
    const IndexValidationRepairRequest& request) {
  auto result = BaseResult(request);
  const auto recovery =
      ClassifySecondaryIndexDeltaLedgerForRecovery(request.state.delta_ledger);
  AddEvidence(&result, "delta.recovery_class",
              SecondaryIndexDeltaLedgerRecoveryClassName(recovery.recovery_class));
  AddEvidence(&result, "delta.recovery_action",
              SecondaryIndexDeltaLedgerRecoveryActionName(recovery.action));
  AddEvidence(&result, "delta.uncommitted_count",
              std::to_string(recovery.uncommitted_delta_count));
  AddEvidence(&result, "delta.committed_premerge_count",
              std::to_string(recovery.committed_premerge_delta_count));
  AddEvidence(&result, "delta.stable_reason", recovery.stable_reason);

  if (recovery.ok() &&
      recovery.action == SecondaryIndexDeltaLedgerRecoveryAction::no_action) {
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.DELTA_LEDGER_VALID",
                  "dpc.index_repair.delta_ledger_valid");
    result.actions.push_back("delta_ledger_validated_read_only");
    result.planner_visible = true;
    return result;
  }

  const bool repairable =
      recovery.action ==
          SecondaryIndexDeltaLedgerRecoveryAction::apply_overlay_then_merge ||
      recovery.action ==
          SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base ||
      recovery.action ==
          SecondaryIndexDeltaLedgerRecoveryAction::retain_for_mga_transaction_finality;
  SetDiagnostic(&result, recovery.fail_closed ? RefuseStatus() : WarnStatus(),
                recovery.fail_closed ? IndexValidationRepairClass::fail_closed
                                     : IndexValidationRepairClass::repair_required,
                recovery.fail_closed
                    ? "DPC.INDEX_REPAIR.DELTA_LEDGER_FAIL_CLOSED"
                    : "DPC.INDEX_REPAIR.DELTA_LEDGER_REPAIR_REQUIRED",
                recovery.fail_closed
                    ? "dpc.index_repair.delta_ledger_fail_closed"
                    : "dpc.index_repair.delta_ledger_repair_required",
                recovery.diagnostic.diagnostic_code);
  result.actions.push_back(
      "delta_ledger_index_use_hidden_until_recovery_action_completes");

  if (repairable &&
      request.operation != IndexValidationRepairOperation::validate) {
    if (request.operation == IndexValidationRepairOperation::rebuild) {
      result.repaired_state.delta_ledger.records.clear();
    } else {
      for (auto& record : result.repaired_state.delta_ledger.records) {
        if (record.commit_state ==
            SecondaryIndexDeltaLedgerCommitState::committed_premerge) {
          record.commit_state =
              SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
        }
      }
    }
    result.mutation_applied = true;
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.DELTA_LEDGER_REPAIRED",
                  "dpc.index_repair.delta_ledger_repaired");
    result.actions.push_back(
        request.operation == IndexValidationRepairOperation::rebuild
            ? "rebuilt_delta_ledger_from_authoritative_base_index"
            : "repaired_delta_ledger_to_merged_cleaned_state");
    result.planner_visible = true;
  }
  return result;
}

IndexValidationRepairResult ValidatePageExtentSummary(
    const IndexValidationRepairRequest& request) {
  auto result = BaseResult(request);
  const auto decision = ClassifyPageExtentSummaryRepairOrCrashReopen(
      request.state.page_extent_summary,
      request.state.page_extent_summary_format,
      true,
      request.expected_generation_present,
      request.expected_generation);
  AddEvidence(&result, "page_summary.use_class",
              PageExtentSummaryUseClassName(decision.use_class));
  AddEvidence(&result, "page_summary.fallback_reason",
              PageExtentSummaryFallbackReasonName(decision.fallback_reason));
  AddEvidence(&result, "page_summary.rebuild_classification",
              PageExtentSummaryRebuildClassificationName(
                  decision.rebuild_classification));
  if (decision.summary_usable) {
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.PAGE_SUMMARY_VALID",
                  "dpc.index_repair.page_summary_valid");
    result.actions.push_back("page_extent_summary_validated_read_only");
    result.planner_visible = true;
    return result;
  }

  const bool restricted =
      decision.restricted_repair_required ||
      decision.rebuild_classification ==
          PageExtentSummaryRebuildClassification::restricted_repair_required ||
      decision.rebuild_classification ==
          PageExtentSummaryRebuildClassification::persisted_repair_required;
  SetDiagnostic(&result, restricted ? RefuseStatus() : WarnStatus(),
                restricted ? IndexValidationRepairClass::repair_required
                           : IndexValidationRepairClass::safe_fallback,
                restricted
                    ? "DPC.INDEX_REPAIR.PAGE_SUMMARY_REPAIR_REQUIRED"
                    : "DPC.INDEX_REPAIR.PAGE_SUMMARY_SAFE_FALLBACK",
                restricted
                    ? "dpc.index_repair.page_summary_repair_required"
                    : "dpc.index_repair.page_summary_safe_fallback",
                decision.diagnostic.diagnostic_code);
  result.actions.push_back("page_extent_summary_hidden_full_scan_fallback_selected");

  if (request.operation == IndexValidationRepairOperation::repair ||
      request.operation == IndexValidationRepairOperation::rebuild) {
    const auto rebuilt = RebuildPageExtentSummaryFromBasePageEvidence(
        request.state.page_extent_summary,
        request.state.page_extent_summary_format,
        request.state.page_extent_rebuild_event);
    result.repaired_state.page_extent_summary = rebuilt.metadata;
    result.mutation_applied = rebuilt.rebuild_performed;
    if (rebuilt.rebuild_performed && rebuilt.decision.summary_usable) {
      SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                    "DPC.INDEX_REPAIR.PAGE_SUMMARY_REBUILT",
                    "dpc.index_repair.page_summary_rebuilt");
      result.actions.push_back(
          "rebuilt_page_extent_summary_from_authoritative_base_page_evidence");
      result.planner_visible = true;
    } else {
      SetDiagnostic(&result, RefuseStatus(),
                    IndexValidationRepairClass::fail_closed,
                    "DPC.INDEX_REPAIR.PAGE_SUMMARY_REBUILD_REFUSED",
                    "dpc.index_repair.page_summary_rebuild_refused",
                    rebuilt.decision.diagnostic.diagnostic_code);
    }
  }
  return result;
}

IndexValidationRepairResult ValidateTimeRangeSummary(
    const IndexValidationRepairRequest& request) {
  auto result = BaseResult(request);
  const auto plan = PlanTimeRangeSummaryPrune(request.state.time_range_summary);
  AddEvidence(&result, "time_range.selected_access", plan.selected_access);
  AddEvidence(&result, "time_range.fallback_reason", plan.fallback_reason);
  AddEvidence(&result, "time_range.summary_generation",
              std::to_string(plan.summary_generation));
  if (plan.ok()) {
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.TIME_RANGE_VALID",
                  "dpc.index_repair.time_range_valid");
    result.actions.push_back("time_range_summary_validated_read_only");
    result.planner_visible = true;
    return result;
  }
  SetDiagnostic(&result, plan.status, IndexValidationRepairClass::safe_fallback,
                "DPC.INDEX_REPAIR.TIME_RANGE_SAFE_FALLBACK",
                "dpc.index_repair.time_range_safe_fallback",
                plan.diagnostic.diagnostic_code);
  result.actions.push_back("time_range_summary_hidden_exact_fallback_selected");
  if (request.operation == IndexValidationRepairOperation::repair ||
      request.operation == IndexValidationRepairOperation::rebuild ||
      request.operation == IndexValidationRepairOperation::discard_unpublished) {
    result.mutation_applied = true;
    result.repaired_state.time_range_summary.summaries.erase(
        std::remove_if(result.repaired_state.time_range_summary.summaries.begin(),
                       result.repaired_state.time_range_summary.summaries.end(),
                       [](const TimeRangeSummaryDescriptor& descriptor) {
                         return descriptor.status != PageExtentSummaryStatus::current ||
                                !descriptor.persisted_record_present ||
                                !descriptor.checksum_valid ||
                                !TimeRangeSummaryDescriptorAuthorityClean(descriptor) ||
                                !TimeRangeSummaryDescriptorIdentityValid(descriptor);
                       }),
        result.repaired_state.time_range_summary.summaries.end());
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.TIME_RANGE_DISCARDED_UNSAFE",
                  "dpc.index_repair.time_range_discarded_unsafe");
    result.actions.push_back("discarded_unusable_time_range_summary_state");
  }
  return result;
}

IndexValidationRepairResult ValidateShadowBuild(
    const IndexValidationRepairRequest& request) {
  auto result = BaseResult(request);
  const auto visible =
      EvaluateShadowIndexPlannerVisibility(request.state.shadow_build);
  AddEvidence(&result, "shadow.state",
              ShadowIndexBuildStateName(request.state.shadow_build.state));
  AddEvidence(&result, "shadow.planner_visible",
              visible.planner_visible ? "true" : "false");
  if (visible.ok()) {
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.SHADOW_VALID",
                  "dpc.index_repair.shadow_valid");
    result.actions.push_back("shadow_index_publish_state_validated_read_only");
    result.planner_visible = true;
    return result;
  }

  SetDiagnostic(&result, WarnStatus(), IndexValidationRepairClass::discard_required,
                "DPC.INDEX_REPAIR.SHADOW_UNPUBLISHED_STATE",
                "dpc.index_repair.shadow_unpublished_state",
                visible.diagnostic.diagnostic_code);
  result.actions.push_back("shadow_index_state_invisible_until_publish_rules_allow_use");
  if (request.operation == IndexValidationRepairOperation::discard_unpublished) {
    result.repaired_state.shadow_build.state = ShadowIndexBuildState::cancelled;
    result.repaired_state.shadow_build.planner_visible = false;
    result.repaired_state.shadow_build.read_visible = false;
    result.mutation_applied = true;
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.SHADOW_DISCARDED",
                  "dpc.index_repair.shadow_discarded");
    result.actions.push_back("discarded_unpublished_shadow_index_state");
  }
  return result;
}

IndexValidationRepairResult ValidateInvertedSegments(
    const IndexValidationRepairRequest& request) {
  auto result = BaseResult(request);
  const auto recovery = ClassifyInvertedSearchSegmentReopen(
      request.state.inverted_segments,
      request.state.exact_base_table_fallback_available);
  AddEvidence(&result, "search_segment.recovery_class",
              InvertedSearchSegmentRecoveryClassName(recovery.recovery_class));
  AddEvidence(&result, "search_segment.recovery_action",
              InvertedSearchSegmentRecoveryActionName(recovery.action));
  AddEvidence(&result, "search_segment.stable_reason", recovery.stable_reason);
  if (recovery.ok() &&
      recovery.action == InvertedSearchSegmentRecoveryAction::keep_visible_segments) {
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.SEARCH_SEGMENT_VALID",
                  "dpc.index_repair.search_segment_valid");
    result.actions.push_back("inverted_search_segments_validated_read_only");
    result.planner_visible = true;
    return result;
  }

  SetDiagnostic(&result, recovery.fail_closed ? RefuseStatus() : WarnStatus(),
                recovery.fail_closed ? IndexValidationRepairClass::fail_closed
                                     : IndexValidationRepairClass::safe_fallback,
                recovery.fail_closed
                    ? "DPC.INDEX_REPAIR.SEARCH_SEGMENT_FAIL_CLOSED"
                    : "DPC.INDEX_REPAIR.SEARCH_SEGMENT_SAFE_FALLBACK",
                recovery.fail_closed
                    ? "dpc.index_repair.search_segment_fail_closed"
                    : "dpc.index_repair.search_segment_safe_fallback",
                recovery.diagnostic.diagnostic_code);
  result.actions.push_back("search_segment_state_hidden_until_publish_rules_allow_use");
  if (request.operation == IndexValidationRepairOperation::discard_unpublished ||
      request.operation == IndexValidationRepairOperation::repair ||
      request.operation == IndexValidationRepairOperation::rebuild) {
    for (auto& segment : result.repaired_state.inverted_segments.segments) {
      if (segment.state != InvertedSearchSegmentState::visible) {
        segment.state = InvertedSearchSegmentState::retired;
        segment.visible = false;
      }
    }
    result.mutation_applied = true;
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.SEARCH_SEGMENT_DISCARDED_UNSAFE",
                  "dpc.index_repair.search_segment_discarded_unsafe");
    result.actions.push_back("discarded_unpublished_or_unsafe_search_segments");
  }
  return result;
}

IndexValidationRepairResult ValidateVectorGenerations(
    const IndexValidationRepairRequest& request) {
  auto result = BaseResult(request);
  const auto recovery = ClassifyVectorGenerationReopen(
      request.state.vector_generations,
      request.state.exact_vector_scan_fallback_available);
  AddEvidence(&result, "vector_generation.recovery_class",
              VectorGenerationRecoveryClassName(recovery.recovery_class));
  AddEvidence(&result, "vector_generation.recovery_action",
              VectorGenerationRecoveryActionName(recovery.action));
  AddEvidence(&result, "vector_generation.stable_reason",
              recovery.stable_reason);
  if (recovery.ok() &&
      recovery.action == VectorGenerationRecoveryAction::keep_published_generation) {
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.VECTOR_GENERATION_VALID",
                  "dpc.index_repair.vector_generation_valid");
    result.actions.push_back("vector_generations_validated_read_only");
    result.planner_visible = true;
    return result;
  }

  SetDiagnostic(&result, recovery.fail_closed ? RefuseStatus() : WarnStatus(),
                recovery.fail_closed ? IndexValidationRepairClass::fail_closed
                                     : IndexValidationRepairClass::safe_fallback,
                recovery.fail_closed
                    ? "DPC.INDEX_REPAIR.VECTOR_GENERATION_FAIL_CLOSED"
                    : "DPC.INDEX_REPAIR.VECTOR_GENERATION_SAFE_FALLBACK",
                recovery.fail_closed
                    ? "dpc.index_repair.vector_generation_fail_closed"
                    : "dpc.index_repair.vector_generation_safe_fallback",
                recovery.diagnostic.diagnostic_code);
  result.actions.push_back("vector_generation_hidden_until_publish_rules_allow_use");
  if (request.operation == IndexValidationRepairOperation::discard_unpublished ||
      request.operation == IndexValidationRepairOperation::repair ||
      request.operation == IndexValidationRepairOperation::rebuild) {
    for (auto& generation : result.repaired_state.vector_generations.generations) {
      if (generation.state != VectorGenerationState::published) {
        generation.state = VectorGenerationState::retired;
        generation.visible = false;
      }
    }
    result.mutation_applied = true;
    SetDiagnostic(&result, OkStatus(), IndexValidationRepairClass::clean,
                  "DPC.INDEX_REPAIR.VECTOR_GENERATION_DISCARDED_UNSAFE",
                  "dpc.index_repair.vector_generation_discarded_unsafe");
    result.actions.push_back("discarded_unpublished_or_unsafe_vector_generations");
  }
  return result;
}

}  // namespace

const char* IndexValidationRepairFamilyName(
    IndexValidationRepairFamily family) {
  switch (family) {
    case IndexValidationRepairFamily::ordered_table_candidate_set:
      return "ordered_table_candidate_set";
    case IndexValidationRepairFamily::secondary_delta_ledger:
      return "secondary_delta_ledger";
    case IndexValidationRepairFamily::page_extent_summary:
      return "page_extent_summary";
    case IndexValidationRepairFamily::time_range_summary:
      return "time_range_summary";
    case IndexValidationRepairFamily::shadow_index_build_state:
      return "shadow_index_build_state";
    case IndexValidationRepairFamily::inverted_search_segment_state:
      return "inverted_search_segment_state";
    case IndexValidationRepairFamily::vector_generation_state:
      return "vector_generation_state";
  }
  return "unknown";
}

const char* IndexValidationRepairOperationName(
    IndexValidationRepairOperation operation) {
  switch (operation) {
    case IndexValidationRepairOperation::validate: return "validate";
    case IndexValidationRepairOperation::repair: return "repair";
    case IndexValidationRepairOperation::rebuild: return "rebuild";
    case IndexValidationRepairOperation::discard_unpublished:
      return "discard_unpublished";
  }
  return "unknown";
}

const char* IndexValidationRepairClassName(
    IndexValidationRepairClass classification) {
  switch (classification) {
    case IndexValidationRepairClass::clean: return "clean";
    case IndexValidationRepairClass::safe_fallback: return "safe_fallback";
    case IndexValidationRepairClass::repair_required: return "repair_required";
    case IndexValidationRepairClass::rebuild_required: return "rebuild_required";
    case IndexValidationRepairClass::discard_required: return "discard_required";
    case IndexValidationRepairClass::refused: return "refused";
    case IndexValidationRepairClass::fail_closed: return "fail_closed";
  }
  return "unknown";
}

const char* IndexFamilyValidationRepairPathName(
    IndexFamilyValidationRepairPath path) {
  switch (path) {
    case IndexFamilyValidationRepairPath::persistent_physical:
      return "persistent_physical";
    case IndexFamilyValidationRepairPath::memory_only_runtime:
      return "memory_only_runtime";
    case IndexFamilyValidationRepairPath::memory_primary_cold_start:
      return "memory_primary_cold_start";
    case IndexFamilyValidationRepairPath::donor_semantic_mapping:
      return "donor_semantic_mapping";
    case IndexFamilyValidationRepairPath::policy_refusal:
      return "policy_refusal";
    case IndexFamilyValidationRepairPath::unavailable:
      return "unavailable";
  }
  return "unavailable";
}

const char* IndexFamilyValidationRepairOpenStateName(
    IndexFamilyValidationRepairOpenState state) {
  switch (state) {
    case IndexFamilyValidationRepairOpenState::open_allowed:
      return "open_allowed";
    case IndexFamilyValidationRepairOpenState::open_refused:
      return "open_refused";
    case IndexFamilyValidationRepairOpenState::repair_required:
      return "repair_required";
    case IndexFamilyValidationRepairOpenState::rebuild_required:
      return "rebuild_required";
    case IndexFamilyValidationRepairOpenState::non_physical_refused:
      return "non_physical_refused";
  }
  return "open_refused";
}

bool IndexValidationRepairOperationMutates(
    IndexValidationRepairOperation operation) {
  return operation != IndexValidationRepairOperation::validate;
}

IndexValidationRepairResult ExecuteIndexValidationRepairOperation(
    const IndexValidationRepairRequest& request) {
  if (!TargetIdentityValid(request.target)) {
    return Refuse(request,
                  "DPC.INDEX_REPAIR.IDENTITY_REFUSED",
                  "dpc.index_repair.identity_refused",
                  "names must resolve to generated UUID identities before validation or repair");
  }
  if (IndexValidationRepairOperationMutates(request.operation)) {
    if (request.read_only_database) {
      return Refuse(request,
                    "DPC.INDEX_REPAIR.READ_ONLY_REFUSED",
                    "dpc.index_repair.read_only_refused",
                    "read-only database cannot execute index repair, rebuild, or discard");
    }
    if (!request.policy_allows_mutation) {
      return Refuse(request,
                    "DPC.INDEX_REPAIR.MUTATION_POLICY_REQUIRED",
                    "dpc.index_repair.mutation_policy_required",
                    "index repair requires an engine-owned mutation policy grant");
    }
  }

  switch (request.validation_family) {
    case IndexValidationRepairFamily::ordered_table_candidate_set:
      return ValidateOrderedCandidateSet(request);
    case IndexValidationRepairFamily::secondary_delta_ledger:
      return ValidateDeltaLedger(request);
    case IndexValidationRepairFamily::page_extent_summary:
      return ValidatePageExtentSummary(request);
    case IndexValidationRepairFamily::time_range_summary:
      return ValidateTimeRangeSummary(request);
    case IndexValidationRepairFamily::shadow_index_build_state:
      return ValidateShadowBuild(request);
    case IndexValidationRepairFamily::inverted_search_segment_state:
      return ValidateInvertedSegments(request);
    case IndexValidationRepairFamily::vector_generation_state:
      return ValidateVectorGenerations(request);
  }
  return Refuse(request,
                "DPC.INDEX_REPAIR.FAMILY_REFUSED",
                "dpc.index_repair.family_refused",
                "unknown validation family");
}

IndexFamilyValidationRepairResult ExecuteIndexFamilyValidationRepairOperation(
    const IndexFamilyValidationRepairRequest& request) {
  const auto* descriptor = FindBuiltinIndexFamily(request.family);
  const auto* state = FindBuiltinIndexFamilyPhysicalCapabilityState(request.family);
  if (descriptor == nullptr || state == nullptr) {
    return RefuseFamily(request, descriptor, state,
                        "IRC.INDEX_REPAIR.UNKNOWN_FAMILY",
                        "irc.index_repair.unknown_family",
                        "family must be declared in the built-in registry");
  }

  if (!TargetIdentityValidForFamily(request.target, request.family)) {
    return RefuseFamily(
        request, descriptor, state,
        "IRC.INDEX_REPAIR.IDENTITY_REFUSED",
        "irc.index_repair.identity_refused",
        "database, table, index, catalog binding, and physical family must resolve to generated UUID identities before family validation or repair");
  }

  if (!request.proof.catalog_uuid_binding_proven) {
    return RefuseFamily(
        request, descriptor, state,
        "IRC.INDEX_REPAIR.CATALOG_UUID_PROOF_REQUIRED",
        "irc.index_repair.catalog_uuid_proof_required",
        "catalog UUID binding proof is required before any index family state is validated");
  }

  if (IndexValidationRepairOperationMutates(request.operation)) {
    if (request.read_only_database) {
      return RefuseFamily(request, descriptor, state,
                          "IRC.INDEX_REPAIR.READ_ONLY_REFUSED",
                          "irc.index_repair.read_only_refused",
                          "read-only database cannot execute family repair, rebuild, or discard");
    }
    if (!request.policy_allows_mutation) {
      return RefuseFamily(
          request, descriptor, state,
          "IRC.INDEX_REPAIR.MUTATION_POLICY_REQUIRED",
          "irc.index_repair.mutation_policy_required",
          "family repair requires an engine-owned mutation policy grant");
    }
  }

  if (descriptor->persistence == IndexPersistenceClass::donor_emulated) {
    return RefuseFamily(
        request, descriptor, state,
        "IRC.INDEX_REPAIR.DONOR_EMULATED.NON_AUTHORITY_MAPPING",
        "irc.index_repair.donor_emulated.non_authority_mapping",
        "donor-emulated index metadata is semantic mapping only and cannot validate, repair, recover, or expose physical authority",
        IndexFamilyValidationRepairOpenState::non_physical_refused);
  }

  if (descriptor->persistence == IndexPersistenceClass::policy_blocked ||
      state->blocker == IndexFamilyPhysicalCapabilityBlocker::policy_blocked) {
    return RefuseFamily(
        request, descriptor, state,
        state->blocker_diagnostic_code.empty()
            ? "IRC.INDEX_REPAIR.POLICY_BLOCKED"
            : state->blocker_diagnostic_code,
        state->blocker_message_key.empty()
            ? "irc.index_repair.policy_blocked"
            : state->blocker_message_key,
        state->blocker_detail.empty()
            ? "policy blocks this index family from physical use"
            : state->blocker_detail,
        IndexFamilyValidationRepairOpenState::non_physical_refused);
  }

  if (!state->physically_complete() || !state->runtime_available) {
    auto result = BaseFamilyResult(request, descriptor, state);
    result.validation_state = std::string("blocked_by_") +
                              IndexFamilyPhysicalCapabilityBlockerName(
                                  state->blocker);
    result.repair_state = result.mutating ? result.validation_state
                                          : "not_requested";
    SetFamilyCapabilityDiagnostic(
        &result, RefuseStatus(), IndexValidationRepairClass::fail_closed,
        IndexFamilyValidationRepairOpenState::open_refused, *state);
    result.actions.push_back(
        "family_hidden_until_physical_capability_state_is_complete");
    AddFamilyStateEvidence(&result);
    return result;
  }

  return ExecuteFamilyWithCompleteCapability(request, *descriptor, *state);
}

DiagnosticRecord MakeIndexValidationRepairDiagnostic(
    Status status,
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
                        "core.index.validation_repair");
}

}  // namespace scratchbird::core::index
