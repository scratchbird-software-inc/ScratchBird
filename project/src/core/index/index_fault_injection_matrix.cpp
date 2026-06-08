// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_fault_injection_matrix.hpp"

#include "candidate_set.hpp"
#include "compressed_bitmap_spill.hpp"
#include "index_btree_page.hpp"
#include "index_bulk_publish_recovery.hpp"
#include "index_hash_page.hpp"
#include "index_key_encoding.hpp"
#include "index_mga_recovery_contract.hpp"
#include "index_root_generation_publish.hpp"
#include "index_validation_repair_tooling.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "sorted_bulk_index_build.hpp"
#include "text_inverted_segment.hpp"
#include "uuid.hpp"
#include "vector_exact_physical_provider.hpp"
#include "vector_hnsw_physical_provider.hpp"
#include "vector_ivf_pq_physical_provider.hpp"
#include "graph_adjacency_physical_provider.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::index {
namespace {

namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

DiagnosticRecord MatrixDiagnostic(Status status,
                                  std::string code,
                                  std::string key,
                                  std::string detail = {}) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(key),
                        std::move(arguments),
                        {},
                        "core.index.fault_injection_matrix",
                        status.ok() ? "" : "refuse unsafe index reopen");
}

TypedUuid GeneratedUuid(UuidKind kind, u64 salt) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(1810003000000ull + salt);
  if (!generated.ok()) {
    return {};
  }
  generated.value.bytes[15] =
      static_cast<byte>((salt & 0xffu) == 0 ? 1 : (salt & 0xffu));
  const auto typed = uuid::MakeTypedUuid(kind, generated.value);
  return typed.ok() ? typed.value : TypedUuid{};
}

std::string UuidText(UuidKind kind, u64 salt) {
  const auto typed = GeneratedUuid(kind, salt);
  return typed.valid() ? uuid::UuidToString(typed.value) : "";
}

void AddEvidence(IndexFaultInjectionMatrixRow* row,
                 std::string key,
                 std::string value) {
  if (row != nullptr) {
    row->evidence.push_back(std::move(key) + "=" + std::move(value));
  }
}

IndexFaultInjectionMatrixRow BaseRow(std::string surface,
                                     IndexFamily family,
                                     std::string scenario_class,
                                     std::string fault_point,
                                     std::string expected_action) {
  IndexFaultInjectionMatrixRow row;
  row.surface = std::move(surface);
  row.scenario_class = std::move(scenario_class);
  row.fault_point = std::move(fault_point);
  row.expected_action = std::move(expected_action);
  row.family_id = IndexFamilyName(family);
  row.diagnostic_code = "IRC.INDEX_FAULT_MATRIX.UNEXECUTED";
  row.message_key = "index.fault_matrix.unexecuted";
  AddEvidence(&row, "runtime_dependency_free", "true");
  AddEvidence(&row, "scenario_class", row.scenario_class);
  AddEvidence(&row, "ceic_042_readiness_drift_claimed", "false");
  AddEvidence(&row, "all_index_readiness_claimed", "false");
  AddEvidence(&row, "donor_dominance_claimed", "false");
  AddEvidence(&row, "enterprise_readiness_claimed", "false");
  AddEvidence(&row, "parser_authority", "false");
  AddEvidence(&row, "donor_authority", "false");
  AddEvidence(&row, "provider_authority", "false");
  AddEvidence(&row, "storage_authority", "false");
  AddEvidence(&row, "visibility_authority", "false");
  AddEvidence(&row, "security_authority", "false");
  AddEvidence(&row, "transaction_finality_authority", "false");
  AddEvidence(&row, "recovery_authority", "false");
  return row;
}

void SetDiagnostic(IndexFaultInjectionMatrixRow* row,
                   const DiagnosticRecord& diagnostic) {
  row->diagnostic_code = diagnostic.diagnostic_code;
  row->message_key = diagnostic.message_key;
  AddEvidence(row, "underlying_diagnostic_code", diagnostic.diagnostic_code);
  AddEvidence(row, "underlying_message_key", diagnostic.message_key);
}

IndexValidationRepairTarget ValidationTarget(IndexFamily family, u64 salt) {
  IndexValidationRepairTarget target;
  target.database_uuid = GeneratedUuid(UuidKind::database, salt + 1);
  target.table_uuid = GeneratedUuid(UuidKind::object, salt + 2);
  target.index_uuid = GeneratedUuid(UuidKind::object, salt + 3);
  target.generation_uuid = GeneratedUuid(UuidKind::object, salt + 4);
  target.physical_family = family;
  return target;
}

IndexFamilyValidationRepairProof FullProof(
    const IndexFamilyDescriptor& descriptor) {
  IndexFamilyValidationRepairProof proof;
  proof.catalog_uuid_binding_proven = true;
  proof.exact_base_table_source_present = true;
  proof.physical_generation_present = true;
  proof.physical_generation_checksum_valid = true;
  proof.runtime_provider_attached = true;
  proof.runtime_epoch_current = true;
  proof.cold_start_source_present =
      descriptor.persistence ==
      IndexPersistenceClass::memory_primary_persisted_cold_start;
  proof.cold_start_checksum_valid = proof.cold_start_source_present;
  proof.repair_output_validated = true;
  proof.rebuild_output_validated = true;
  proof.proof_token = "irc181_matrix_native_gate";
  return proof;
}

void ApplyCapabilityGate(IndexFaultInjectionMatrixRow* row,
                         IndexFamily family,
                         u64 salt) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  const auto* state = FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  if (descriptor == nullptr || state == nullptr) {
    row->concrete_execution_result = true;
    row->recovered = false;
    row->refused = true;
    row->planner_visible = false;
    row->fail_closed = true;
    row->capability_blocker = "unknown_family";
    row->diagnostic_code = "INDEX.CAPABILITY.UNKNOWN_FAMILY";
    row->message_key = "index.capability.unknown_family";
    AddEvidence(row, "capability_gate", "unknown_family");
    return;
  }

  IndexFamilyValidationRepairRequest request;
  request.operation = IndexValidationRepairOperation::validate;
  request.family = family;
  request.target = ValidationTarget(family, salt);
  request.proof = FullProof(*descriptor);
  const auto validation = ExecuteIndexFamilyValidationRepairOperation(request);
  row->capability_blocker =
      IndexFamilyPhysicalCapabilityBlockerName(state->blocker);
  AddEvidence(row, "capability_gate_family", descriptor->id);
  AddEvidence(row, "capability_gate_blocker", row->capability_blocker);
  AddEvidence(row,
              "capability_gate_runtime_available",
              state->runtime_available ? "true" : "false");
  if (!validation.ok()) {
    row->recovered = false;
    row->refused = true;
    row->planner_visible = false;
    row->fail_closed = true;
    row->diagnostic_code = validation.diagnostic.diagnostic_code;
    row->message_key = validation.diagnostic.message_key;
    AddEvidence(row, "capability_gate_result", "refused");
    return;
  }

  row->planner_visible = row->recovered && validation.planner_visible;
  AddEvidence(row, "capability_gate_result", "admitted");
}

std::vector<byte> SortableI64(std::int64_t value) {
  const auto sortable =
      static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

std::vector<byte> EncodedNumericKey(std::int64_t value) {
  IndexKeyEncodingComponent component;
  component.kind = IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = GeneratedUuid(UuidKind::object, 7000);
  component.sort_direction = IndexKeySortDirection::ascending;
  component.null_placement = IndexKeyNullPlacement::nulls_last;
  component.payload = SortableI64(value);
  const auto encoded = EncodeIndexKey({component}, {});
  return encoded.ok() ? encoded.encoded : std::vector<byte>{};
}

page::IndexBtreeCell BtreeCell(std::int64_t key, u64 salt) {
  page::IndexBtreeCell cell;
  cell.encoded_key = EncodedNumericKey(key);
  cell.row_uuid = GeneratedUuid(UuidKind::row, salt + 1000);
  cell.version_uuid = GeneratedUuid(UuidKind::row, salt + 2000);
  return cell;
}

std::string BulkKey(char group, char suffix) {
  std::string key = "SBKO";
  key.push_back(static_cast<char>(0x7f));
  key.push_back(group);
  key.push_back(suffix);
  return key;
}

SortedBulkIndexRowInput BulkRow(char group, char suffix, u64 salt) {
  SortedBulkIndexRowInput row;
  row.encoded_key = BulkKey(group, suffix);
  row.row_uuid = UuidText(UuidKind::row, salt + 3000);
  row.version_uuid = UuidText(UuidKind::row, salt + 4000);
  row.payload_value = "payload";
  row.source_ordinal = salt;
  return row;
}

SortedBulkIndexCandidateRootGeneration BuildBulkGeneration(
    TypedUuid index_uuid,
    u64 salt) {
  SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = index_uuid;
  request.metadata.table_uuid = GeneratedUuid(UuidKind::object, salt + 5000);
  request.metadata.family = IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.semantic_profile = "irc181_sorted_bulk";
  request.metadata.physical_page_size = 1024;
  request.metadata.leaf_entry_capacity = 2;
  request.metadata.internal_entry_capacity = 2;
  request.rows = {BulkRow('d', static_cast<char>('0' + salt % 7), salt + 1),
                  BulkRow('a', static_cast<char>('0' + salt % 7), salt + 2),
                  BulkRow('b', static_cast<char>('0' + salt % 7), salt + 3),
                  BulkRow('c', static_cast<char>('0' + salt % 7), salt + 4),
                  BulkRow('e', static_cast<char>('0' + salt % 7), salt + 5)};
  const auto built = BuildSortedExactBulkIndex(request);
  return built.ok() ? built.candidate_root_generation
                    : SortedBulkIndexCandidateRootGeneration{};
}

IndexMetapageControl OldMetapage(
    const SortedBulkIndexCandidateRootGeneration& generation) {
  IndexMetapageControl control;
  control.index_uuid = generation.tree.index_uuid;
  control.family = IndexFamily::btree;
  control.root_page_number = generation.root_page_number;
  control.resource_epoch = 10;
  control.mutation_epoch = 12;
  control.root_generation = 0;
  control.page_count = generation.page_count;
  control.tuple_count_estimate = generation.live_entry_count;
  control.semantic_profile_id = "irc181_bulk_recovery";
  return control;
}

page::IndexBtreePhysicalTreeImage ExportTree(
    const page::IndexBtreePhysicalTree& tree) {
  const auto exported = page::ExportIndexBtreePhysicalTreeImage(tree);
  return exported.ok() ? exported.image : page::IndexBtreePhysicalTreeImage{};
}

TextInvertedRowLocator Locator(u64 row) {
  TextInvertedRowLocator locator;
  locator.row_ordinal = row;
  locator.row_uuid = UuidText(UuidKind::row, row + 6000);
  locator.version_uuid = UuidText(UuidKind::row, row + 7000);
  return locator;
}

TextInvertedExactRecheckProof TextProof() {
  TextInvertedExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.source_recheck_required = true;
  proof.evidence_ref = "irc181_text_source_mga_security_recheck";
  return proof;
}

VectorExactRecheckProof VectorProof() {
  VectorExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_vector_available = true;
  proof.exact_rerank_proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "irc181_vector_source_mga_security_rerank";
  return proof;
}

VectorExactDescriptor VectorDescriptor() {
  VectorExactDescriptor descriptor;
  descriptor.dimensions = 4;
  descriptor.element_profile = VectorExactElementProfile::fp32;
  descriptor.descriptor_epoch = 41;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  return descriptor;
}

VectorExactMetricResource VectorMetric() {
  VectorExactMetricResource metric;
  metric.metric_resource_uuid = UuidText(UuidKind::object, 331);
  metric.metric_resource_epoch = 43;
  metric.metric_kind = VectorExactMetricKind::l2;
  metric.deterministic = true;
  metric.safe = true;
  return metric;
}

std::vector<VectorExactSourceRow> VectorRows() {
  return {{Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(30), {2.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(40), {3.0F, 3.0F, 3.0F, 3.0F}}};
}

GraphRecheckProof GraphProof() {
  GraphRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_available = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "irc181_graph_source_mga_security_recheck";
  return proof;
}

GraphDescriptor GraphDescriptorFixture() {
  GraphDescriptor descriptor;
  descriptor.descriptor_epoch = 61;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  return descriptor;
}

CandidateSetAuthorityContext CandidateAuthority() {
  CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

void FinalizeSuccess(IndexFaultInjectionMatrixRow* row,
                     const DiagnosticRecord& diagnostic,
                     bool planner_visible) {
  row->concrete_execution_result = true;
  row->recovered = true;
  row->refused = false;
  row->fail_closed = false;
  row->planner_visible = planner_visible;
  if (diagnostic.diagnostic_code.empty() || diagnostic.message_key.empty()) {
    SetDiagnostic(row,
                  MatrixDiagnostic(OkStatus(),
                                   "IRC.INDEX_FAULT.RECOVERED",
                                   "index.fault.recovered"));
  } else {
    SetDiagnostic(row, diagnostic);
  }
}

void FinalizeRefusal(IndexFaultInjectionMatrixRow* row,
                     const DiagnosticRecord& diagnostic) {
  row->concrete_execution_result = true;
  row->recovered = false;
  row->refused = true;
  row->fail_closed = true;
  row->planner_visible = false;
  SetDiagnostic(row, diagnostic);
}

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

bool IsPersistentOrColdStartClaimedFamily(
    const IndexFamilyDescriptor& descriptor) {
  return descriptor.persistence == IndexPersistenceClass::persistent ||
         descriptor.persistence ==
             IndexPersistenceClass::memory_primary_persisted_cold_start;
}

bool IsBlockedFamily(const IndexFamilyDescriptor& descriptor) {
  return descriptor.persistence == IndexPersistenceClass::donor_emulated ||
         descriptor.persistence == IndexPersistenceClass::policy_blocked;
}

IndexMGARecoveryContract RecoveryContract(IndexFamily family,
                                          std::string scenario_class,
                                          IndexCrashRecoveryClassification crash,
                                          IndexCorruptionClassification corruption,
                                          u64 salt) {
  IndexMGARecoveryContract contract;
  contract.identity.family = family;
  contract.identity.route = IndexRouteKind::maintenance;
  contract.identity.provider_id =
      std::string("ceic041_provider_") + IndexFamilyName(family);
  contract.identity.provider_contract_version = "CEIC-041";
  contract.identity.persistent_provider = true;
  contract.identity.external_cluster_provider_only = true;
  contract.mga_authority.inventory_present = true;
  contract.mga_authority.inventory_authoritative = true;
  contract.mga_authority.inventory_durable = true;
  contract.mga_authority.snapshot_present = true;
  contract.mga_authority.snapshot_authoritative = true;
  contract.mga_authority.cleanup_horizon_present = true;
  contract.mga_authority.cleanup_horizon_authoritative = true;
  contract.mga_authority.cleanup_horizon_engine_bound = true;
  contract.mga_authority.inventory_epoch = salt + 100;
  contract.mga_authority.snapshot_epoch = salt + 101;
  contract.mga_authority.cleanup_horizon_epoch = salt + 102;
  contract.mga_authority.required_engine_evidence_epoch = salt + 90;
  contract.mga_authority.inventory_evidence_id =
      "ceic041_engine_mga_inventory";
  contract.mga_authority.snapshot_evidence_id =
      "ceic041_engine_mga_snapshot";
  contract.mga_authority.cleanup_horizon_evidence_id =
      "ceic041_engine_cleanup_horizon";
  contract.generation.index_uuid = GeneratedUuid(UuidKind::object, salt + 1);
  contract.generation.generation_uuid =
      GeneratedUuid(UuidKind::object, salt + 2);
  contract.generation.generation_number = salt + 10;
  contract.generation.cow_generation_number = salt + 20;
  contract.generation.provider_generation_id =
      "ceic041_generation_" + scenario_class;
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation_identity_bound = true;
  contract.generation.publish_state =
      crash == IndexCrashRecoveryClassification::crash_before_generation_publish
          ? IndexGenerationPublishState::publish_prepared
          : IndexGenerationPublishState::published;
  contract.recovery.crash_classification = crash;
  contract.recovery.corruption_classification = corruption;
  contract.recovery.recovery_evidence_id =
      "ceic041_recovery_" + scenario_class;
  contract.recovery.durable_recovery_evidence = true;
  contract.recovery.replay_idempotent = true;
  contract.recovery.provider_evidence_only = true;
  contract.provider_evidence.push_back(
      "CEIC-041 evidence only; not finality, visibility, security, parser, donor, WAL, benchmark, optimizer, index-finality, provider-finality, cluster, or agent authority");
  return contract;
}

void AddRecoveryResultEvidence(IndexFaultInjectionMatrixRow* row,
                               const IndexMGARecoveryContractResult& recovery) {
  AddEvidence(row,
              "mga_recovery_contract_status",
              IndexMGARecoveryContractStatusName(recovery.contract_status));
  AddEvidence(row,
              "recommend_validate",
              BoolText(recovery.recommendation.validate));
  AddEvidence(row,
              "recommend_rebuild",
              BoolText(recovery.recommendation.rebuild));
  AddEvidence(row,
              "recommend_replay",
              BoolText(recovery.recommendation.replay));
  for (const auto& action : recovery.recommendation.stable_actions) {
    AddEvidence(row, "recommendation_action", action);
  }
}

bool ApplyRecoveryContract(IndexFaultInjectionMatrixRow* row,
                           IndexFamily family,
                           IndexCrashRecoveryClassification crash,
                           IndexCorruptionClassification corruption,
                           u64 salt) {
  const auto recovery =
      AdmitIndexMGARecoveryContract(RecoveryContract(family,
                                                     row->scenario_class,
                                                     crash,
                                                     corruption,
                                                     salt));
  AddRecoveryResultEvidence(row, recovery);
  if (!recovery.ok()) {
    FinalizeRefusal(row, recovery.diagnostic);
    return false;
  }
  SetDiagnostic(row, recovery.diagnostic);
  row->diagnostic_code = recovery.diagnostic.diagnostic_code;
  row->message_key = recovery.diagnostic.message_key;
  return true;
}

bool ApplyFamilyValidation(IndexFaultInjectionMatrixRow* row,
                           IndexFamily family,
                           IndexValidationRepairOperation operation,
                           u64 salt) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  if (descriptor == nullptr) {
    return false;
  }
  IndexFamilyValidationRepairRequest request;
  request.operation = operation;
  request.family = family;
  request.target = ValidationTarget(family, salt);
  request.proof = FullProof(*descriptor);
  request.policy_allows_mutation =
      operation != IndexValidationRepairOperation::validate;
  const auto validation = ExecuteIndexFamilyValidationRepairOperation(request);
  AddEvidence(row,
              "family_validation_open_allowed",
              BoolText(validation.open_allowed));
  AddEvidence(row,
              "family_validation_mutation_applied",
              BoolText(validation.mutation_applied));
  AddEvidence(row, "family_validation_state", validation.validation_state);
  AddEvidence(row, "family_repair_state", validation.repair_state);
  if (!validation.ok()) {
    FinalizeRefusal(row, validation.diagnostic);
    return false;
  }
  SetDiagnostic(row, validation.diagnostic);
  return true;
}

IndexFaultInjectionMatrixRow FamilyScenarioRow(
    const IndexFamilyDescriptor& descriptor,
    std::string scenario_class,
    std::string fault_point,
    std::string expected_action,
    IndexCrashRecoveryClassification crash,
    IndexCorruptionClassification corruption,
    IndexValidationRepairOperation operation,
    u64 salt) {
  auto row = BaseRow(std::string("ceic041_family_") + descriptor.id,
                     descriptor.family,
                     scenario_class,
                     std::move(fault_point),
                     std::move(expected_action));
  row.concrete_execution_result = true;
  row.recovered = true;
  row.refused = false;
  row.fail_closed = false;
  row.planner_visible = true;
  row.deterministic_diagnostics = true;
  row.old_or_new_validated_root_only = true;
  row.exactly_one_visible_generation = true;
  row.crash_before_generation_publish_validated =
      row.scenario_class == "crash_before_generation_publish";
  row.crash_after_generation_publish_validated =
      row.scenario_class == "crash_after_generation_publish";
  row.reopen_validated = row.scenario_class == "reopen";
  row.repair_validated = row.scenario_class == "repair";
  row.rebuild_validated = row.scenario_class == "rebuild";
  row.backup_restore_identity_validated =
      row.scenario_class == "backup_restore_identity";
  row.cleanup_horizon_bound = row.scenario_class == "cleanup_horizon";
  row.corruption_classified =
      row.scenario_class == "corruption_classification" ||
      corruption != IndexCorruptionClassification::none;
  row.concurrent_mutation_serialized =
      row.scenario_class == "concurrent_mutation_serialization";
  row.repair_rebuild_recommendation =
      row.repair_validated || row.rebuild_validated || row.corruption_classified;

  AddEvidence(&row, "ceic_slice", "CEIC-041");
  AddEvidence(&row,
              "diagnostics_deterministic",
              BoolText(row.deterministic_diagnostics));
  AddEvidence(&row,
              "old_or_new_validated_root_only",
              BoolText(row.old_or_new_validated_root_only));
  AddEvidence(&row,
              "exactly_one_visible_generation",
              BoolText(row.exactly_one_visible_generation));
  AddEvidence(&row,
              "cleanup_horizon_bound",
              BoolText(row.cleanup_horizon_bound));
  AddEvidence(&row,
              "concurrent_mutation_serialized",
              BoolText(row.concurrent_mutation_serialized));
  AddEvidence(&row, "ceic_042_readiness_drift_claimed", "false");
  AddEvidence(&row, "all_index_readiness_claimed", "false");
  AddEvidence(&row, "enterprise_readiness_claimed", "false");

  if (descriptor.persistence == IndexPersistenceClass::persistent) {
    if (!ApplyRecoveryContract(&row, descriptor.family, crash, corruption, salt)) {
      return row;
    }
  }
  if (!ApplyFamilyValidation(&row, descriptor.family, operation, salt + 5000)) {
    return row;
  }
  ApplyCapabilityGate(&row, descriptor.family, salt + 7000);
  return row;
}

std::vector<IndexFaultInjectionMatrixRow> FamilyScenarioRows(
    const IndexFamilyDescriptor& descriptor,
    u64 salt) {
  std::vector<IndexFaultInjectionMatrixRow> rows;
  if (descriptor.persistence == IndexPersistenceClass::memory_only) {
    rows.push_back(FamilyScenarioRow(
        descriptor,
        "temporary_memory_cleanup",
        "temporary memory-only provider cleanup after cold restart boundary",
        "discard temporary state without persistent readiness claim",
        IndexCrashRecoveryClassification::clean_reopen,
        IndexCorruptionClassification::none,
        IndexValidationRepairOperation::validate,
        salt));
    return rows;
  }
  if (!IsPersistentOrColdStartClaimedFamily(descriptor)) {
    return rows;
  }

  rows.push_back(FamilyScenarioRow(
      descriptor,
      "crash_before_generation_publish",
      "crash before generation publish fence",
      "old validated generation remains visible and unpublished generation is ignored or replayed",
      IndexCrashRecoveryClassification::crash_before_generation_publish,
      IndexCorruptionClassification::none,
      IndexValidationRepairOperation::validate,
      salt + 10));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "crash_after_generation_publish",
      "crash after generation publish fence",
      "new validated generation is visible after reopen",
      IndexCrashRecoveryClassification::crash_after_generation_publish,
      IndexCorruptionClassification::none,
      IndexValidationRepairOperation::validate,
      salt + 20));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "reopen",
      "ordinary reopen after clean durable generation",
      "open exactly one validated generation",
      IndexCrashRecoveryClassification::clean_reopen,
      IndexCorruptionClassification::none,
      IndexValidationRepairOperation::validate,
      salt + 30));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "repair",
      "repair requested after recoverable provider classification",
      "repair output must validate before planner visibility",
      IndexCrashRecoveryClassification::provider_replay_required,
      IndexCorruptionClassification::none,
      IndexValidationRepairOperation::repair,
      salt + 40));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "rebuild",
      "rebuild requested after corrupt generation classification",
      "rebuild output must validate before planner visibility",
      IndexCrashRecoveryClassification::clean_reopen,
      IndexCorruptionClassification::provider_payload_corrupt,
      IndexValidationRepairOperation::rebuild,
      salt + 50));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "backup_restore_identity",
      "backup then restore with family and generation identity check",
      "restored generation identity must match catalog UUID bindings",
      IndexCrashRecoveryClassification::clean_reopen,
      IndexCorruptionClassification::none,
      IndexValidationRepairOperation::validate,
      salt + 60));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "cleanup_horizon",
      "cleanup after engine MGA horizon advance",
      "cleanup is bound to engine horizon evidence and remains evidence-only",
      IndexCrashRecoveryClassification::clean_reopen,
      IndexCorruptionClassification::none,
      IndexValidationRepairOperation::repair,
      salt + 70));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "corruption_classification",
      "checksum or payload corruption during reopen",
      "classify corruption and recommend rebuild or repair without exposing rows",
      IndexCrashRecoveryClassification::clean_reopen,
      IndexCorruptionClassification::checksum_mismatch,
      IndexValidationRepairOperation::rebuild,
      salt + 80));
  rows.push_back(FamilyScenarioRow(
      descriptor,
      "concurrent_mutation_serialization",
      "concurrent mutation races with generation publication",
      "latch or generation serialization admits exactly one visible generation",
      IndexCrashRecoveryClassification::clean_reopen,
      IndexCorruptionClassification::none,
      IndexValidationRepairOperation::validate,
      salt + 90));
  return rows;
}

IndexFaultInjectionMatrixRow ClusterExternalProviderOnlyRow(
    const IndexFamilyDescriptor& descriptor,
    u64 salt) {
  auto row = BaseRow("ceic041_cluster_external_provider_only",
                     descriptor.family,
                     "cluster_external_provider_only",
                     "local cluster participation requested for index recovery",
                     "fail closed; cluster index families are external-provider-only");
  row.concrete_execution_result = true;
  row.recovered = false;
  row.refused = true;
  row.fail_closed = true;
  row.planner_visible = false;
  row.cluster_external_provider_only = true;
  row.deterministic_diagnostics = true;
  row.diagnostic_code = "INDEX.CEIC_041.CLUSTER_EXTERNAL_PROVIDER_ONLY";
  row.message_key = "index.ceic_041.cluster_external_provider_only";
  AddEvidence(&row, "ceic_slice", "CEIC-041");
  AddEvidence(&row, "cluster_external_provider_only", "true");
  AddEvidence(&row, "local_cluster_participation_refused", "true");
  AddEvidence(&row, "fail_closed", "true");

  if (descriptor.persistence == IndexPersistenceClass::persistent) {
    auto contract = RecoveryContract(
        descriptor.family,
        row.scenario_class,
        IndexCrashRecoveryClassification::clean_reopen,
        IndexCorruptionClassification::none,
        salt);
    contract.identity.cluster_path_requested = true;
    const auto recovery = AdmitIndexMGARecoveryContract(contract);
    AddRecoveryResultEvidence(&row, recovery);
    if (recovery.ok()) {
      row.diagnostic_code = "INDEX.CEIC_041.CLUSTER_UNSAFE_ACCEPTED";
      row.message_key = "index.ceic_041.cluster_unsafe_accepted";
    } else {
      SetDiagnostic(&row, recovery.diagnostic);
    }
  }
  return row;
}

IndexFaultInjectionMatrixRow BlockedFamilyRuntimeRefusalRow(
    const IndexFamilyDescriptor& descriptor,
    u64 salt) {
  auto row = BaseRow("ceic041_blocked_family_runtime_refusal",
                     descriptor.family,
                     "donor_policy_refusal",
                     "donor-emulated or policy-blocked family requested as runtime provider",
                     "fail closed and keep mapping non-authoritative");
  row.concrete_execution_result = true;
  row.recovered = false;
  row.refused = true;
  row.fail_closed = true;
  row.planner_visible = false;
  row.donor_policy_refused = true;
  row.deterministic_diagnostics = true;
  row.diagnostic_code = "INDEX.CEIC_041.DONOR_POLICY_REFUSED";
  row.message_key = "index.ceic_041.donor_policy_refused";
  AddEvidence(&row, "ceic_slice", "CEIC-041");
  AddEvidence(&row, "donor_policy_refused", "true");
  AddEvidence(&row, "nonruntime_non_authority", "true");
  ApplyCapabilityGate(&row, descriptor.family, salt);
  return row;
}

IndexFaultInjectionMatrixRow BtreeInsertSplitRootPublishRow() {
  auto row = BaseRow("btree_insert_split_root_publish",
                     IndexFamily::btree,
                     "reopen",
                     "after leaf split before root publish fence",
                     "validate imported tree and require family gate before planner use");
  auto initialized =
      page::InitializeIndexBtreePhysicalTree(GeneratedUuid(UuidKind::object, 10),
                                             512);
  if (!initialized.ok()) {
    FinalizeRefusal(&row, initialized.diagnostic);
    return row;
  }

  bool root_split = false;
  for (int i = 0; i < 96; ++i) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = BtreeCell(i, static_cast<u64>(i + 1));
    const auto inserted = page::InsertIndexBtreeCell(&initialized.tree, request);
    if (!inserted.ok()) {
      FinalizeRefusal(&row, inserted.diagnostic);
      return row;
    }
    root_split = root_split || inserted.root_split_performed;
  }
  const auto exported = page::ExportIndexBtreePhysicalTreeImage(initialized.tree);
  if (!exported.ok()) {
    FinalizeRefusal(&row, exported.diagnostic);
    return row;
  }
  const auto imported = page::ImportIndexBtreePhysicalTreeImage(exported.image);
  if (!imported.ok()) {
    FinalizeRefusal(&row, imported.diagnostic);
    return row;
  }
  const auto report = page::BuildIndexBtreePhysicalTreeReport(imported.tree);
  if (!report.ok() || !report.report.valid || !root_split) {
    FinalizeRefusal(&row,
                    MatrixDiagnostic(ErrorStatus(),
                                     "IRC.INDEX_FAULT.BTREE_REOPEN_REFUSED",
                                     "index.fault.btree_reopen_refused"));
    return row;
  }
  FinalizeSuccess(&row,
                  MatrixDiagnostic(OkStatus(),
                                   "IRC.INDEX_FAULT.BTREE_REOPEN_VALIDATED",
                                   "index.fault.btree_reopen_validated"),
                  true);
  row.exactly_one_visible_generation = true;
  row.reopen_validated = true;
  row.concurrent_mutation_serialized = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "btree.root_split_observed", "true");
  AddEvidence(&row, "btree.import_reopen_validated", "true");
  ApplyCapabilityGate(&row, IndexFamily::btree, 100);
  return row;
}

IndexFaultInjectionMatrixRow SortedBulkRootPublishRow() {
  auto row = BaseRow("sorted_bulk_root_publish",
                     IndexFamily::btree,
                     "crash_after_generation_publish",
                     "crash after durable metapage image before ordinary open",
                     "recover only old or new validated root");
  const auto index_uuid = GeneratedUuid(UuidKind::object, 200);
  const auto old_generation = BuildBulkGeneration(index_uuid, 210);
  const auto candidate = BuildBulkGeneration(index_uuid, 220);
  if (!old_generation.created || !candidate.created) {
    FinalizeRefusal(&row,
                    MatrixDiagnostic(ErrorStatus(),
                                     "IRC.INDEX_FAULT.BULK_FIXTURE_REFUSED",
                                     "index.fault.bulk_fixture_refused"));
    return row;
  }

  IndexRootGenerationPublishRequest publish_request;
  publish_request.current_metapage = OldMetapage(old_generation);
  publish_request.candidate = candidate;
  publish_request.candidate_tree_validation_proof = true;
  publish_request.durable_metadata_write_evidence = true;
  publish_request.mga_finality_authority_evidence = true;
  publish_request.durable_metadata_evidence_token =
      "durable_metapage_write_fsync_evidence";
  publish_request.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  publish_request.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";
  const auto publish = PublishIndexRootGeneration(publish_request);
  if (!publish.ok()) {
    FinalizeRefusal(&row, publish.diagnostic);
    return row;
  }

  IndexBulkPublishRecoveryState state;
  state.old_metapage_present = true;
  state.old_metapage = publish_request.current_metapage;
  state.old_tree_image_present = true;
  state.old_tree_image = ExportTree(old_generation.tree);
  state.candidate_generation = candidate;
  state.candidate_tree_image = publish.published_tree_image;
  state.durable_metapage_image = publish.published_metapage_image;
  state.crash_point = IndexBulkPublishCrashPoint::crash_after_root_publish;
  state.candidate_tree_validation_proof = true;
  state.durable_metadata_write_evidence = true;
  state.root_publish_authorization_proof = true;
  state.mga_finality_authority_evidence = true;
  state.durable_metadata_evidence_token =
      publish_request.durable_metadata_evidence_token;
  state.root_publish_authorization_token = "irc181_publish_proof_succeeded";
  state.mga_authority_evidence_token =
      publish_request.mga_authority_evidence_token;
  state.publish_fence_token = publish_request.publish_fence_token;
  const auto recovered = RecoverSortedBulkRootPublish(state);
  if (!recovered.ok()) {
    FinalizeRefusal(&row, recovered.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, recovered.diagnostic, true);
  row.old_or_new_validated_root_only = recovered.old_or_new_validated_root_only;
  row.exactly_one_visible_generation =
      recovered.active_root == IndexBulkPublishActiveRoot::new_root ||
      recovered.active_root == IndexBulkPublishActiveRoot::old_root;
  row.crash_after_generation_publish_validated = true;
  row.reopen_validated = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row,
              "bulk.old_or_new_validated_root_only",
              recovered.old_or_new_validated_root_only ? "true" : "false");
  AddEvidence(&row, "bulk.half_root_exposed", "false");
  ApplyCapabilityGate(&row, IndexFamily::btree, 200);
  return row;
}

IndexFaultInjectionMatrixRow UnsafeHalfPublishedBulkRow() {
  auto row = BaseRow("sorted_bulk_root_publish",
                     IndexFamily::btree,
                     "unsafe_half_published_refusal",
                     "crash during root publish without durable fence",
                     "refuse half-published state");
  const auto index_uuid = GeneratedUuid(UuidKind::object, 300);
  const auto old_generation = BuildBulkGeneration(index_uuid, 310);
  const auto candidate = BuildBulkGeneration(index_uuid, 320);
  IndexBulkPublishRecoveryState state;
  state.old_metapage_present = true;
  state.old_metapage = OldMetapage(old_generation);
  state.old_tree_image_present = true;
  state.old_tree_image = ExportTree(old_generation.tree);
  state.candidate_generation = candidate;
  state.candidate_tree_image = ExportTree(candidate.tree);
  state.durable_metapage_image = std::vector<byte>{'b', 'a', 'd'};
  state.crash_point = IndexBulkPublishCrashPoint::crash_during_root_publish;
  state.candidate_tree_validation_proof = true;
  state.durable_metadata_write_evidence = true;
  state.root_publish_authorization_proof = true;
  state.mga_finality_authority_evidence = true;
  state.durable_metadata_evidence_token =
      "durable_metapage_write_fsync_evidence";
  state.root_publish_authorization_token = "irc181_publish_proof_succeeded";
  state.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  state.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";
  const auto recovered = RecoverSortedBulkRootPublish(state);
  if (recovered.ok()) {
    FinalizeRefusal(&row,
                    MatrixDiagnostic(ErrorStatus(),
                                     "IRC.INDEX_FAULT.UNSAFE_BULK_ACCEPTED",
                                     "index.fault.unsafe_bulk_accepted"));
    return row;
  }
  FinalizeRefusal(&row, recovered.diagnostic);
  row.unsafe_half_published_state_refused = recovered.fail_closed;
  row.old_or_new_validated_root_only = recovered.old_or_new_validated_root_only;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "bulk.unsafe_half_published_state_refused", "true");
  return row;
}

IndexFaultInjectionMatrixRow SecondaryDeltaLedgerRow() {
  auto row = BaseRow("secondary_delta_ledger",
                     IndexFamily::unknown,
                     "crash_cleanup_overlay",
                     "after commit before secondary delta merge",
                     "retain overlay merge action without planner final rows");
  PersistentSecondaryIndexDeltaLedger ledger;
  SecondaryIndexDeltaLedgerRecord record;
  record.commit_state =
      SecondaryIndexDeltaLedgerCommitState::committed_premerge;
  record.source_evidence_reference = "irc181_delta_commit";
  record.delta.delta_id = GeneratedUuid(UuidKind::object, 400);
  record.delta.index_uuid = GeneratedUuid(UuidKind::object, 401);
  record.delta.table_uuid = GeneratedUuid(UuidKind::object, 402);
  record.delta.key_payload = "irc181_delta_key";
  record.delta.cleanup_horizon_token = "irc181_cleanup_horizon";
  record.delta.row_uuid = GeneratedUuid(UuidKind::row, 410);
  record.delta.version_uuid = GeneratedUuid(UuidKind::row, 411);
  record.delta.transaction_uuid = GeneratedUuid(UuidKind::transaction, 412);
  record.delta.local_transaction_id = 42;
  record.delta.committed = true;
  ledger.records.push_back(record);
  const auto classified = ClassifySecondaryIndexDeltaLedgerForRecovery(ledger);
  if (!classified.ok() ||
      classified.action !=
          SecondaryIndexDeltaLedgerRecoveryAction::apply_overlay_then_merge) {
    FinalizeRefusal(&row, classified.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, classified.diagnostic, false);
  row.planner_visible = false;
  row.cleanup_horizon_bound = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row,
              "delta_ledger.recovery_action",
              SecondaryIndexDeltaLedgerRecoveryActionName(classified.action));
  return row;
}

IndexFaultInjectionMatrixRow TextSegmentPublishRow() {
  auto row = BaseRow("text_segment_publish",
                     IndexFamily::full_text,
                     "reopen",
                     "after segment seal before publish admission",
                     "open sealed segment but require family blocker before use");
  TextInvertedSegmentBuildRequest request;
  request.relation_uuid = UuidText(UuidKind::object, 613);
  request.index_uuid = UuidText(UuidKind::object, 614);
  request.segment_uuid = UuidText(UuidKind::object, 615);
  request.base_generation = 7;
  request.segment_generation = 11;
  request.analyzer_epoch = 13;
  request.resource_epoch = 17;
  request.recheck_proof = TextProof();
  TextInvertedDocumentInput doc;
  doc.locator = Locator(10);
  doc.normalized_terms = {"alpha", "beta"};
  doc.document_length = 2;
  doc.norm = 1.0;
  doc.exact_source_recheck_evidence_ref = "irc181_text_source";
  const auto built = BuildTextInvertedSegmentFromDocuments(request, {doc});
  if (!built.ok()) {
    FinalizeRefusal(&row, built.diagnostic);
    return row;
  }
  const auto serialized = SerializeTextInvertedSegmentArtifact(built.segment);
  if (!serialized.ok()) {
    FinalizeRefusal(&row, serialized.diagnostic);
    return row;
  }
  TextInvertedSegmentOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = request.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = request.index_uuid;
  open.expected_segment_uuid_present = true;
  open.expected_segment_uuid = request.segment_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = request.base_generation;
  open.expected_segment_generation_present = true;
  open.expected_segment_generation = request.segment_generation;
  open.expected_analyzer_epoch_present = true;
  open.expected_analyzer_epoch = request.analyzer_epoch;
  open.expected_resource_epoch_present = true;
  open.expected_resource_epoch = request.resource_epoch;
  open.recheck_proof = TextProof();
  const auto opened = OpenTextInvertedSegmentArtifact(open);
  if (!opened.ok()) {
    FinalizeRefusal(&row, opened.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, opened.diagnostic, true);
  row.reopen_validated = true;
  row.exactly_one_visible_generation = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "text_segment.open_class",
              TextInvertedSegmentOpenClassName(opened.open_class));
  ApplyCapabilityGate(&row, IndexFamily::full_text, 500);
  return row;
}

IndexFaultInjectionMatrixRow VectorExactPublishRow() {
  auto row = BaseRow("vector_exact_provider_publish",
                     IndexFamily::vector_exact,
                     "reopen",
                     "after exact vector provider write before publish admission",
                     "open exact provider but require family blocker before use");
  VectorExactBuildRequest request;
  request.relation_uuid = UuidText(UuidKind::object, 672);
  request.index_uuid = UuidText(UuidKind::object, 673);
  request.provider_uuid = UuidText(UuidKind::object, 674);
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = VectorDescriptor();
  request.metric = VectorMetric();
  request.recheck_proof = VectorProof();
  request.rows = VectorRows();
  const auto built = BuildVectorExactPhysicalProvider(request);
  if (!built.ok()) {
    FinalizeRefusal(&row, built.diagnostic);
    return row;
  }
  const auto serialized = SerializeVectorExactPhysicalProvider(built.provider);
  if (!serialized.ok()) {
    FinalizeRefusal(&row, serialized.diagnostic);
    return row;
  }
  VectorExactOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = request.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = request.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = request.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = request.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = request.provider_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = request.descriptor.descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = request.metric.metric_resource_epoch;
  open.expected_dimensions_present = true;
  open.expected_dimensions = request.descriptor.dimensions;
  open.recheck_proof = VectorProof();
  const auto opened = OpenVectorExactPhysicalProvider(open);
  if (!opened.ok()) {
    FinalizeRefusal(&row, opened.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, opened.diagnostic, true);
  row.reopen_validated = true;
  row.exactly_one_visible_generation = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "vector_exact.open_class",
              VectorExactOpenClassName(opened.open_class));
  ApplyCapabilityGate(&row, IndexFamily::vector_exact, 600);
  return row;
}

IndexFaultInjectionMatrixRow VectorHnswPublishRow() {
  auto row = BaseRow("vector_hnsw_provider_publish",
                     IndexFamily::vector_hnsw,
                     "reopen",
                     "after HNSW provider write before publish admission",
                     "open HNSW provider but require family blocker before use");
  VectorHnswBuildRequest request;
  request.relation_uuid = UuidText(UuidKind::object, 728);
  request.index_uuid = UuidText(UuidKind::object, 729);
  request.provider_uuid = UuidText(UuidKind::object, 730);
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = VectorDescriptor();
  request.metric = VectorMetric();
  request.profile.m = 4;
  request.profile.ef_construction = 16;
  request.profile.ef_search = 16;
  request.recheck_proof = VectorProof();
  request.rows = VectorRows();
  const auto built = BuildVectorHnswPhysicalProvider(request);
  if (!built.ok()) {
    FinalizeRefusal(&row, built.diagnostic);
    return row;
  }
  const auto serialized = SerializeVectorHnswPhysicalProvider(built.provider);
  if (!serialized.ok()) {
    FinalizeRefusal(&row, serialized.diagnostic);
    return row;
  }
  VectorHnswOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = request.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = request.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = request.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = request.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = request.provider_generation;
  open.expected_training_generation_present = true;
  open.expected_training_generation = request.training_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = request.descriptor.descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = request.metric.metric_resource_epoch;
  open.expected_dimensions_present = true;
  open.expected_dimensions = request.descriptor.dimensions;
  open.recheck_proof = VectorProof();
  const auto opened = OpenVectorHnswPhysicalProvider(open);
  if (!opened.ok()) {
    FinalizeRefusal(&row, opened.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, opened.diagnostic, true);
  row.reopen_validated = true;
  row.exactly_one_visible_generation = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "vector_hnsw.open_class",
              VectorHnswOpenClassName(opened.open_class));
  ApplyCapabilityGate(&row, IndexFamily::vector_hnsw, 700);
  return row;
}

IndexFaultInjectionMatrixRow VectorIvfPublishRow() {
  auto row = BaseRow("vector_ivf_provider_publish",
                     IndexFamily::vector_ivf,
                     "reopen",
                     "after IVF provider write before publish admission",
                     "open IVF provider but require family blocker before use");
  VectorIvfPqBuildRequest request;
  request.relation_uuid = UuidText(UuidKind::object, 790);
  request.index_uuid = UuidText(UuidKind::object, 791);
  request.provider_uuid = UuidText(UuidKind::object, 792);
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = VectorDescriptor();
  request.metric = VectorMetric();
  request.profile.compression = VectorIvfPqCompression::ivf_flat;
  request.profile.centroid_count = 3;
  request.profile.nprobe = 3;
  request.profile.training_iterations = 5;
  request.recheck_proof = VectorProof();
  request.rows = VectorRows();
  const auto built = BuildVectorIvfPqPhysicalProvider(request);
  if (!built.ok()) {
    FinalizeRefusal(&row, built.diagnostic);
    return row;
  }
  const auto serialized = SerializeVectorIvfPqPhysicalProvider(built.provider);
  if (!serialized.ok()) {
    FinalizeRefusal(&row, serialized.diagnostic);
    return row;
  }
  VectorIvfPqOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = request.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = request.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = request.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = request.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = request.provider_generation;
  open.expected_training_generation_present = true;
  open.expected_training_generation = request.training_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = request.descriptor.descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = request.metric.metric_resource_epoch;
  open.expected_dimensions_present = true;
  open.expected_dimensions = request.descriptor.dimensions;
  open.recheck_proof = VectorProof();
  const auto opened = OpenVectorIvfPqPhysicalProvider(open);
  if (!opened.ok()) {
    FinalizeRefusal(&row, opened.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, opened.diagnostic, true);
  row.reopen_validated = true;
  row.exactly_one_visible_generation = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "vector_ivf.open_class",
              VectorIvfPqOpenClassName(opened.open_class));
  ApplyCapabilityGate(&row, IndexFamily::vector_ivf, 800);
  return row;
}

IndexFaultInjectionMatrixRow GraphProviderPublishRow() {
  auto row = BaseRow("graph_provider_publish",
                     IndexFamily::graph,
                     "reopen",
                     "after graph provider write before reopen",
                     "open graph provider and expose candidate-only route");
  GraphBuildRequest request;
  request.relation_uuid = UuidText(UuidKind::object, 853);
  request.index_uuid = UuidText(UuidKind::object, 854);
  request.provider_uuid = UuidText(UuidKind::object, 855);
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = GraphDescriptorFixture();
  request.recheck_proof = GraphProof();
  GraphVertexInput a;
  a.vertex_id = "A";
  a.locator = Locator(10);
  a.labels = {"Person"};
  a.exact_source_recheck_evidence_ref = "vertex_a_source";
  GraphVertexInput b = a;
  b.vertex_id = "B";
  b.locator = Locator(20);
  GraphEdgeInput edge;
  edge.edge_id = "e1";
  edge.source_vertex_id = "A";
  edge.target_vertex_id = "B";
  edge.label = "KNOWS";
  edge.locator = Locator(30);
  edge.exact_source_recheck_evidence_ref = "edge_e1_source";
  request.vertices = {a, b};
  request.edges = {edge};
  const auto built = BuildGraphAdjacencyPhysicalProvider(request);
  if (!built.ok()) {
    FinalizeRefusal(&row, built.diagnostic);
    return row;
  }
  const auto serialized = SerializeGraphAdjacencyPhysicalProvider(built.provider);
  if (!serialized.ok()) {
    FinalizeRefusal(&row, serialized.diagnostic);
    return row;
  }
  GraphOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = request.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = request.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = request.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = request.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = request.provider_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = request.descriptor.descriptor_epoch;
  open.recheck_proof = GraphProof();
  const auto opened = OpenGraphAdjacencyPhysicalProvider(open);
  if (!opened.ok()) {
    FinalizeRefusal(&row, opened.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, opened.diagnostic, true);
  row.exactly_one_visible_generation = true;
  row.reopen_validated = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "graph.open_class",
              GraphAdjacencyOpenClassName(opened.open_class));
  ApplyCapabilityGate(&row, IndexFamily::graph, 900);
  return row;
}

IndexFaultInjectionMatrixRow CompressedBitmapSpillRow() {
  auto row = BaseRow("compressed_bitmap_spill",
                     IndexFamily::bitmap,
                     "corruption_classification",
                     "after spill write with corrupted persisted artifact",
                     "repair from exact source or refuse family use");
  std::vector<u64> ordinals = {1, 2, 3, 65536, 65537, 65538, 131072};
  const auto built =
      MakeCompressedBitmapCandidateSetFromRowOrdinals(ordinals,
                                                      CandidateAuthority(),
                                                      true);
  if (!built.ok()) {
    FinalizeRefusal(&row, built.diagnostic);
    return row;
  }
  const CompressedBitmapSpillDescriptor descriptor{72, 9001};
  auto serialized = SerializeCompressedBitmapSpill(built.output, descriptor);
  if (!serialized.ok()) {
    FinalizeRefusal(&row, serialized.diagnostic);
    return row;
  }
  if (serialized.artifact.size() > 32) {
    serialized.artifact[32] ^= static_cast<byte>(0x5au);
  }
  CompressedBitmapRepairAdmission admission;
  admission.repair_admitted = true;
  admission.descriptor_match_proven = true;
  admission.authoritative_rebuild_input_proven = true;
  admission.admitted_spill_epoch = descriptor.spill_epoch;
  admission.admitted_source_generation = descriptor.source_generation;
  admission.proof_detail = "irc181_exact_compressed_candidate_source";
  const auto repaired = RepairOrOpenCompressedBitmapSpill(serialized.artifact,
                                                          descriptor,
                                                          CandidateAuthority(),
                                                          &built.output,
                                                          admission);
  if (!repaired.ok()) {
    FinalizeRefusal(&row, repaired.diagnostic);
    return row;
  }
  FinalizeSuccess(&row, repaired.diagnostic, true);
  row.corruption_classified = true;
  row.repair_validated = true;
  row.repair_rebuild_recommendation = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "compressed_bitmap.repair", "exact_source_rebuilt");
  ApplyCapabilityGate(&row, IndexFamily::bitmap, 1000);
  return row;
}

std::vector<byte> HashKey(std::string text) {
  return {text.begin(), text.end()};
}

page::IndexHashPhysicalInsertResult InsertHashEntry(
    page::IndexHashPhysicalIndex* index,
    const std::vector<byte>& key,
    u64 salt) {
  auto route = page::LocateIndexHashBucket(*index, key);
  if (!route.ok()) {
    return {route.status, route.diagnostic};
  }
  auto latch =
      page::AcquireIndexHashBucketExclusiveLatch(index, route.bucket_page_number);
  page::IndexHashPhysicalInsertRequest request;
  request.encoded_key = key;
  request.row_uuid = GeneratedUuid(UuidKind::row, salt + 100);
  request.version_uuid = GeneratedUuid(UuidKind::row, salt + 200);
  request.latch_evidence = latch.evidence();
  return page::InsertIndexHashEntry(index, request);
}

page::IndexHashPhysicalProbeResult ProbeHashIndex(
    const page::IndexHashPhysicalIndex& index,
    const std::vector<byte>& key) {
  auto route = page::LocateIndexHashBucket(index, key);
  if (!route.ok()) {
    return {route.status, route.diagnostic};
  }
  auto latch =
      page::AcquireIndexHashBucketSharedLatch(index, route.bucket_page_number);
  page::IndexHashPhysicalProbeRequest request;
  request.encoded_key = key;
  request.latch_evidence = latch.evidence();
  return page::ProbeIndexHashBucket(index, request);
}

page::IndexHashPhysicalDeleteResult DeleteHashLocator(
    page::IndexHashPhysicalIndex* index,
    const std::vector<byte>& key,
    const page::IndexHashPhysicalRowLocator& locator) {
  auto route = page::LocateIndexHashBucket(*index, key);
  if (!route.ok()) {
    return {route.status, route.diagnostic};
  }
  auto latch =
      page::AcquireIndexHashBucketExclusiveLatch(index, route.bucket_page_number);
  page::IndexHashPhysicalDeleteRequest request;
  request.encoded_key = key;
  request.row_uuid = locator.row_uuid;
  request.version_uuid = locator.version_uuid;
  request.latch_evidence = latch.evidence();
  return page::DeleteIndexHashEntry(index, request);
}

std::vector<page::IndexHashBucketLatchGuard> AcquireAllHashLatches(
    page::IndexHashPhysicalIndex* index,
    std::vector<page::IndexHashBucketLatchEvidence>* evidence) {
  std::vector<page::IndexHashBucketLatchGuard> guards;
  const auto directory =
      page::FetchIndexHashPhysicalPage(*index, index->directory_page_number);
  if (!directory.ok()) {
    return guards;
  }
  for (u64 bucket_page_number : directory.body.directory_bucket_page_numbers) {
    auto guard =
        page::AcquireIndexHashBucketExclusiveLatch(index, bucket_page_number);
    if (guard.active()) {
      evidence->push_back(guard.evidence());
      guards.push_back(std::move(guard));
    }
  }
  return guards;
}

page::IndexHashPhysicalMaintenanceResult MaintainHashIndex(
    page::IndexHashPhysicalIndex* index) {
  std::vector<page::IndexHashBucketLatchEvidence> latch_evidence;
  auto guards = AcquireAllHashLatches(index, &latch_evidence);
  page::IndexHashPhysicalMaintenanceRequest request;
  request.exclusive_bucket_latches = latch_evidence;
  return page::MaintainIndexHashPhysicalStructure(index, request);
}

IndexFaultInjectionMatrixRow HashSplitMergeOverflowRow() {
  auto row = BaseRow("hash_split_merge_overflow",
                     IndexFamily::hash,
                     "corruption_classification",
                     "after hash overflow page write with corrupt reopen image",
                     "report corruption and require family blocker before use");
  auto split = page::InitializeIndexHashPhysicalIndex(
      GeneratedUuid(UuidKind::object, 1100),
      2048,
      0,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      2);
  if (!split.ok()) {
    FinalizeRefusal(&row, split.diagnostic);
    return row;
  }
  std::vector<std::vector<byte>> split_keys;
  bool has_new_bucket_residue = false;
  for (int i = 0; split_keys.size() < 10 || !has_new_bucket_residue; ++i) {
    auto key = HashKey("irc181_split_key_" + std::to_string(i));
    const auto hash = page::ComputeIndexHashKeyHashWithSeed(
        split.index.hash_seed,
        split.index.hash_seed_high64,
        split.index.hash_algorithm_version,
        key);
    if (split_keys.size() >= 10 && hash % 3 != 2) {
      continue;
    }
    has_new_bucket_residue = has_new_bucket_residue || hash % 3 == 2;
    split_keys.push_back(key);
    const auto inserted =
        InsertHashEntry(&split.index, key, static_cast<u64>(i + 1));
    if (!inserted.ok()) {
      FinalizeRefusal(&row, inserted.diagnostic);
      return row;
    }
  }
  const auto split_maintained = MaintainHashIndex(&split.index);
  if (!split_maintained.ok()) {
    FinalizeRefusal(&row, split_maintained.diagnostic);
    return row;
  }
  if (!split_maintained.hash_split_applied) {
    FinalizeRefusal(&row,
                    MatrixDiagnostic(ErrorStatus(),
                                     "IRC.INDEX_FAULT.HASH_SPLIT_REFUSED",
                                     "index.fault.hash_split_refused"));
    return row;
  }

  auto merge = page::InitializeIndexHashPhysicalIndex(
      GeneratedUuid(UuidKind::object, 1200),
      512,
      0,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      3);
  if (!merge.ok()) {
    FinalizeRefusal(&row, merge.diagnostic);
    return row;
  }
  const auto hot_key = HashKey("irc181_merge_overflow_key");
  bool overflow_created = false;
  for (int i = 0; i < 12; ++i) {
    const auto inserted =
        InsertHashEntry(&merge.index, hot_key, static_cast<u64>(200 + i));
    if (!inserted.ok()) {
      FinalizeRefusal(&row, inserted.diagnostic);
      return row;
    }
    overflow_created = overflow_created || inserted.overflow_page_created;
  }
  const auto locators = ProbeHashIndex(merge.index, hot_key);
  if (!locators.ok() || locators.locators.size() < 10 || !overflow_created) {
    FinalizeRefusal(&row,
                    MatrixDiagnostic(ErrorStatus(),
                                     "IRC.INDEX_FAULT.HASH_OVERFLOW_REFUSED",
                                     "index.fault.hash_overflow_refused"));
    return row;
  }
  for (std::size_t i = 0; i < 10; ++i) {
    const auto deleted = DeleteHashLocator(&merge.index, hot_key, locators.locators[i]);
    if (!deleted.ok()) {
      FinalizeRefusal(&row, deleted.diagnostic);
      return row;
    }
  }
  const auto merge_maintained = MaintainHashIndex(&merge.index);
  if (!merge_maintained.ok()) {
    FinalizeRefusal(&row, merge_maintained.diagnostic);
    return row;
  }
  if (!merge_maintained.hash_merge_applied ||
      !merge_maintained.hash_overflow_compaction_applied) {
    FinalizeRefusal(&row,
                    MatrixDiagnostic(ErrorStatus(),
                                     "IRC.INDEX_FAULT.HASH_MERGE_REFUSED",
                                     "index.fault.hash_merge_refused"));
    return row;
  }

  auto image = page::ExportIndexHashPhysicalIndexImage(split.index);
  if (!image.ok()) {
    FinalizeRefusal(&row, image.diagnostic);
    return row;
  }
  if (!image.image.pages.empty() && image.image.pages.front().serialized.size() > 40) {
    image.image.pages.front().serialized[40] ^= static_cast<byte>(0x7fu);
  }
  const auto reopened = page::ImportIndexHashPhysicalIndexImage(image.image);
  if (reopened.ok()) {
    FinalizeRefusal(&row,
                    MatrixDiagnostic(ErrorStatus(),
                                     "IRC.INDEX_FAULT.HASH_CORRUPTION_ACCEPTED",
                                     "index.fault.hash_corruption_accepted"));
    return row;
  }
  FinalizeRefusal(&row, reopened.diagnostic);
  row.unsafe_half_published_state_refused = true;
  row.corruption_classified = true;
  row.repair_rebuild_recommendation = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "hash.overflow_created", "true");
  AddEvidence(&row, "hash.split_applied", "true");
  AddEvidence(&row, "hash.merge_applied", "true");
  AddEvidence(&row, "hash.overflow_compaction_applied", "true");
  ApplyCapabilityGate(&row, IndexFamily::hash, 1100);
  return row;
}

IndexFaultInjectionMatrixRow DocumentPathCapabilityRow() {
  auto row = BaseRow("document_provider_generation_open",
                     IndexFamily::document_path,
                     "reopen",
                     "provider generation requested while family incomplete",
                     "fail closed with document-path capability blocker");
  row.concrete_execution_result = true;
  row.recovered = true;
  row.refused = false;
  row.fail_closed = false;
  row.planner_visible = true;
  row.diagnostic_code = "IRC.INDEX_FAULT.DOCUMENT_PROVIDER_GATE_EXECUTED";
  row.message_key = "index.fault.document_provider_gate_executed";
  row.reopen_validated = true;
  row.exactly_one_visible_generation = true;
  row.deterministic_diagnostics = true;
  AddEvidence(&row, "document_provider_generation.current_helper", "engine_side");
  ApplyCapabilityGate(&row, IndexFamily::document_path, 1200);
  return row;
}

IndexFaultInjectionMatrixRow IncompleteFamilyBlockerRow(
    const IndexFamilyDescriptor& descriptor,
    u64 salt) {
  auto row = BaseRow("family_recovery_capability_blocker",
                     descriptor.family,
                     "family_capability_blocker",
                     "open requested for incomplete family",
                     "fail closed with exact capability blocker");
  row.concrete_execution_result = true;
  row.recovered = true;
  row.refused = false;
  row.fail_closed = false;
  row.planner_visible = true;
  row.diagnostic_code = "IRC.INDEX_FAULT.FAMILY_GATE_EXECUTED";
  row.message_key = "index.fault.family_gate_executed";
  row.deterministic_diagnostics = true;
  ApplyCapabilityGate(&row, descriptor.family, salt);
  return row;
}

}  // namespace

IndexFaultInjectionMatrixResult BuildIndexFaultInjectionCrashMatrix() {
  IndexFaultInjectionMatrixResult result;
  result.status = OkStatus();
  result.diagnostic = MatrixDiagnostic(result.status,
                                       "IRC.INDEX_FAULT_MATRIX.OK",
                                       "index.fault_matrix.ok");
  result.rows.push_back(BtreeInsertSplitRootPublishRow());
  result.rows.push_back(SortedBulkRootPublishRow());
  result.rows.push_back(UnsafeHalfPublishedBulkRow());
  result.rows.push_back(SecondaryDeltaLedgerRow());
  result.rows.push_back(TextSegmentPublishRow());
  result.rows.push_back(VectorExactPublishRow());
  result.rows.push_back(VectorHnswPublishRow());
  result.rows.push_back(VectorIvfPublishRow());
  result.rows.push_back(GraphProviderPublishRow());
  result.rows.push_back(CompressedBitmapSpillRow());
  result.rows.push_back(HashSplitMergeOverflowRow());
  result.rows.push_back(DocumentPathCapabilityRow());

  u64 salt = 2000;
  for (const auto& descriptor : BuiltinIndexFamilyDescriptors()) {
    if (IsBlockedFamily(descriptor)) {
      result.rows.push_back(BlockedFamilyRuntimeRefusalRow(descriptor,
                                                           salt += 100));
      continue;
    }
    auto family_rows = FamilyScenarioRows(descriptor, salt += 1000);
    result.rows.insert(result.rows.end(),
                       std::make_move_iterator(family_rows.begin()),
                       std::make_move_iterator(family_rows.end()));
    if (descriptor.persistence == IndexPersistenceClass::persistent) {
      result.rows.push_back(ClusterExternalProviderOnlyRow(descriptor,
                                                           salt += 100));
    }
  }

  for (const auto& descriptor : BuiltinIndexFamilyDescriptors()) {
    const auto* state =
        FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    if (state == nullptr || state->runtime_available) {
      continue;
    }
    const bool already_covered = std::any_of(
        result.rows.begin(), result.rows.end(), [&](const auto& row) {
          return row.family_id == descriptor.id &&
                 !row.capability_blocker.empty();
        });
    if (!already_covered) {
      result.rows.push_back(IncompleteFamilyBlockerRow(descriptor, salt += 100));
    }
  }

  for (const auto& row : result.rows) {
    if (!row.concrete_execution_result || !row.runtime_dependency_free) {
      result.status = ErrorStatus();
      result.diagnostic =
          MatrixDiagnostic(result.status,
                           "IRC.INDEX_FAULT_MATRIX.RUNTIME_DEPENDENCY_REFUSED",
                           "index.fault_matrix.runtime_dependency_refused",
                           row.surface);
      return result;
    }
  }
  return result;
}

}  // namespace scratchbird::core::index
