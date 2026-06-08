// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_index_write_path.hpp"

#include "api_diagnostics.hpp"
#include "index_family_registry.hpp"
#include "index_key_encoding.hpp"
#include "index_maintenance.hpp"
#include "index_route_capability.hpp"
#include "secondary_index_delta_overlay.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK",
                                 "engine.api.ok",
                                 {},
                                 false);
}

EngineApiDiagnostic Invalid(std::string_view detail) {
  return MakeInvalidRequestDiagnostic("dml.index_write_path",
                                      std::string(detail));
}

EngineApiDiagnostic FromPageDiagnostic(
    const platform::DiagnosticRecord& diagnostic,
    std::string_view fallback_detail) {
  return MakeEngineApiDiagnostic(
      diagnostic.diagnostic_code.empty() ? "SB-DML-INDEX-WRITE-PHYSICAL-ERROR"
                                         : diagnostic.diagnostic_code,
      diagnostic.message_key.empty() ? "dml.index_write.physical_error"
                                     : diagnostic.message_key,
      diagnostic.remediation_hint.empty() ? std::string(fallback_detail)
                                          : diagnostic.remediation_hint,
      true);
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

std::string ResolvedFamily(const CrudIndexRecord& index) {
  if (!index.family.empty()) {
    if (index.family == kCrudIndexFamilyBtree && index.unique) {
      return "unique_btree";
    }
    if (index.family == kCrudIndexFamilyGraphAdjacency) {
      return "graph";
    }
    return index.family;
  }
  if (index.unique) {
    return "unique_btree";
  }
  return CrudIndexFamilyForProfile(index.profile);
}

idx::IndexFamily FamilyFromId(const std::string& family) {
  if (family == kCrudIndexFamilyGraphAdjacency) {
    return idx::IndexFamily::graph;
  }
  const auto lookup = idx::FindBuiltinIndexFamilyById(family);
  return lookup.ok() ? lookup.descriptor->family : idx::IndexFamily::unknown;
}

idx::IndexRouteKind RouteForOperation(DmlIndexWriteOperation operation) {
  switch (operation) {
    case DmlIndexWriteOperation::insert:
    case DmlIndexWriteOperation::merge_insert:
      return idx::IndexRouteKind::dml_insert;
    case DmlIndexWriteOperation::update:
    case DmlIndexWriteOperation::merge_update:
      return idx::IndexRouteKind::dml_update;
    case DmlIndexWriteOperation::delete_row:
    case DmlIndexWriteOperation::merge_delete:
      return idx::IndexRouteKind::dml_delete;
  }
  return idx::IndexRouteKind::unknown;
}

bool RouteSupportsDmlWriteFamily(const std::string& family,
                                 idx::IndexRouteKind route) {
  const idx::IndexFamily enum_family = FamilyFromId(family);
  const auto* state =
      idx::FindBuiltinIndexRouteCapabilityState(route, enum_family);
  return state != nullptr && state->route_complete() &&
         state->supports_write && state->supports_mutation;
}

bool UniqueIndex(const CrudIndexRecord& index, const std::string& family) {
  return family == "unique_btree" || index.unique ||
         std::find(index.key_envelopes.begin(),
                   index.key_envelopes.end(),
                   "unique") != index.key_envelopes.end();
}

bool OrderedPhysicalTreeWriteFamily(idx::IndexFamily family) {
  return family == idx::IndexFamily::btree ||
         family == idx::IndexFamily::unique_btree ||
         family == idx::IndexFamily::expression ||
         family == idx::IndexFamily::partial ||
         family == idx::IndexFamily::covering;
}

bool DeferredLedgerDmlFamily(idx::IndexFamily family) {
  const auto* capability =
      idx::FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  return capability != nullptr && capability->physically_complete() &&
         capability->runtime_available && capability->benchmark_clean &&
         capability->blocker == idx::IndexFamilyPhysicalCapabilityBlocker::none &&
         !OrderedPhysicalTreeWriteFamily(family);
}

EngineApiDiagnostic CapabilityDiagnostic(const std::string& family,
                                         idx::IndexRouteKind route) {
  const idx::IndexFamily enum_family = FamilyFromId(family);
  const auto* route_state =
      idx::FindBuiltinIndexRouteCapabilityState(route, enum_family);
  if (route_state != nullptr) {
    if (route_state->family_physical_complete &&
        !route_state->route_complete()) {
      return MakeEngineApiDiagnostic(
          "SB-DML-INDEX-WRITE-FAMILY-ROUTE-UNSUPPORTED",
          "dml.index_write.family_route_unsupported",
          "family=" + family +
              ";route=" + idx::IndexRouteKindName(route) +
              ";physical index family is benchmark-clean but this DML write path only admits ordered B-tree-backed families",
          true);
    }
    return MakeEngineApiDiagnostic(route_state->route_diagnostic_code,
                                   route_state->route_message_key,
                                   "family=" + family + ";route=" +
                                       idx::IndexRouteKindName(route) +
                                       ";" + route_state->route_detail,
                                   true);
  }
  return MakeEngineApiDiagnostic(
      "INDEX.CAPABILITY.UNKNOWN_FAMILY",
      "index.capability.unknown_family",
      "family=" + family +
          ";family is not declared in the built-in index family registry",
          true);
}

void AppendRouteCapabilityEvidence(
    std::vector<EngineEvidenceReference>* evidence,
    const std::string& family,
    idx::IndexRouteKind route) {
  const auto enum_family = FamilyFromId(family);
  const auto* route_state =
      idx::FindBuiltinIndexRouteCapabilityState(route, enum_family);
  evidence->push_back({"dml_index_route_kind",
                       idx::IndexRouteKindName(route)});
  if (route_state == nullptr) {
    evidence->push_back({"dml_index_route_capability", "missing"});
    return;
  }
  evidence->push_back({"dml_index_route_capability",
                       route_state->route_complete() ? "complete" : "refused"});
  evidence->push_back({"dml_index_route_benchmark_clean",
                       family + "=" +
                           (route_state->benchmark_clean ? "true" : "false")});
  evidence->push_back({"dml_index_route_supports_write",
                       family + "=" +
                           (route_state->supports_write ? "true" : "false")});
  evidence->push_back({"dml_index_route_supports_mutation",
                       family + "=" +
                           (route_state->supports_mutation ? "true" : "false")});
}

void AppendDmlMaintenanceStrategyEvidence(
    std::vector<EngineEvidenceReference>* evidence,
    const idx::IndexDmlMaintenanceStrategy& strategy) {
  evidence->push_back({"dml_index_maintenance_strategy",
                       strategy.strategy_id});
  evidence->push_back(
      {"dml_index_maintenance_strategy_kind",
       idx::IndexDmlMaintenanceStrategyKindName(strategy.strategy)});
  evidence->push_back({"dml_index_maintenance_strategy_admitted",
                       strategy.admitted ? "true" : "false"});
  evidence->push_back({"dml_index_maintenance_strategy_exact_recheck",
                       strategy.exact_recheck_required ? "true" : "false"});
  evidence->push_back(
      {"dml_index_maintenance_strategy_exact_recheck_strategy_bound",
       strategy.exact_recheck_strategy_bound ? "true" : "false"});
  evidence->push_back({"dml_index_maintenance_strategy_exact_recheck_gate",
                       strategy.exact_recheck_gate_passed ? "true" : "false"});
  evidence->push_back({"dml_index_maintenance_strategy_mga_recheck",
                       strategy.mga_recheck_required ? "true" : "false"});
  evidence->push_back({"dml_index_maintenance_strategy_security_recheck",
                       strategy.security_recheck_required ? "true" : "false"});
  evidence->push_back({"dml_index_maintenance_strategy_dml_route_supported",
                       strategy.dml_route_supported ? "true" : "false"});
}

EngineApiDiagnostic CoreDiagnosticToEngine(
    const platform::DiagnosticRecord& diagnostic,
    std::string_view fallback_code,
    std::string_view fallback_key,
    std::string_view fallback_detail) {
  std::string detail = diagnostic.arguments.empty()
                           ? std::string(fallback_detail)
                           : diagnostic.arguments.front().value;
  if (detail.empty()) {
    detail = diagnostic.remediation_hint;
  }
  return MakeEngineApiDiagnostic(
      diagnostic.diagnostic_code.empty() ? std::string(fallback_code)
                                         : diagnostic.diagnostic_code,
      diagnostic.message_key.empty() ? std::string(fallback_key)
                                     : diagnostic.message_key,
      detail,
      true);
}

EngineApiDiagnostic HotDeltaDiagnostic(std::string code,
                                       std::string message_key,
                                       std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code),
                                 std::move(message_key),
                                 std::move(detail),
                                 true);
}

EngineApiU64 CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<EngineApiU64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

struct StagedPhysicalTree {
  std::string index_uuid;
  page::IndexBtreePhysicalTree* original = nullptr;
  page::IndexBtreePhysicalTree staged;
};

struct StagedDeltaLedger {
  std::string index_uuid;
  idx::PersistentSecondaryIndexDeltaLedger* original = nullptr;
  idx::PersistentSecondaryIndexDeltaLedger staged;
  idx::SecondaryIndexDeltaLedgerLimits limits;
};

StagedPhysicalTree* FindStagedTree(std::vector<StagedPhysicalTree>* trees,
                                   const std::string& index_uuid) {
  for (auto& tree : *trees) {
    if (tree.index_uuid == index_uuid) {
      return &tree;
    }
  }
  return nullptr;
}

StagedDeltaLedger* FindStagedLedger(std::vector<StagedDeltaLedger>* ledgers,
                                    const std::string& index_uuid) {
  for (auto& ledger : *ledgers) {
    if (ledger.index_uuid == index_uuid) {
      return &ledger;
    }
  }
  return nullptr;
}

std::vector<platform::byte> PayloadBytes(const std::string& key) {
  return std::vector<platform::byte>(key.begin(), key.end());
}

struct EncodedKeyResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<platform::byte> encoded_key;
};

EncodedKeyResult EncodePhysicalKey(const CrudIndexRecord& index,
                                   const std::string& key) {
  const auto descriptor_uuid =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index.index_uuid);
  if (!descriptor_uuid.ok()) {
    EncodedKeyResult result;
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-INDEX-UUID-PROOF-REQUIRED",
        "dml.index_write.index_uuid_proof_required",
        "index_uuid=" + index.index_uuid,
        true);
    return result;
  }

  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = descriptor_uuid.value;
  component.payload = PayloadBytes(key);
  const auto encoded = idx::EncodeIndexKey({component}, {});
  if (!encoded.ok()) {
    EncodedKeyResult result;
    result.diagnostic = FromPageDiagnostic(encoded.diagnostic,
                                           "key_encoding_refused");
    return result;
  }
  EncodedKeyResult result;
  result.ok = true;
  result.encoded_key = encoded.encoded;
  return result;
}

struct TypedRowUuidResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  platform::TypedUuid row_uuid;
  platform::TypedUuid version_uuid;
};

TypedRowUuidResult ParseRowImageUuids(const DmlIndexWriteRowImage& row,
                                      std::string_view label) {
  TypedRowUuidResult result;
  const auto parsed_row =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::row,
                                           row.row_uuid);
  if (!parsed_row.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-ROW-UUID-PROOF-REQUIRED",
        "dml.index_write.row_uuid_proof_required",
        std::string(label) + ":row_uuid=" + row.row_uuid,
        true);
    return result;
  }
  const auto parsed_version =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::row,
                                           row.version_uuid);
  if (!parsed_version.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-VERSION-UUID-PROOF-REQUIRED",
        "dml.index_write.version_uuid_proof_required",
        std::string(label) + ":version_uuid=" + row.version_uuid,
        true);
    return result;
  }
  result.ok = true;
  result.row_uuid = parsed_row.value;
  result.version_uuid = parsed_version.value;
  return result;
}

EngineApiDiagnostic ValidateUpdateRowVersionContinuity(
    const DmlIndexWriteEvent& event) {
  const auto old_ids = ParseRowImageUuids(event.old_row, "old");
  if (!old_ids.ok) {
    return old_ids.diagnostic;
  }
  const auto new_ids = ParseRowImageUuids(event.new_row, "new");
  if (!new_ids.ok) {
    return new_ids.diagnostic;
  }
  if (uuid::CompareUuid128(old_ids.row_uuid.value,
                           new_ids.row_uuid.value) != 0) {
    return MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-UPDATE-ROW-UUID-MISMATCH",
        "dml.index_write.update_row_uuid_mismatch",
        "update old/new row images must describe the same MGA row identity",
        true);
  }
  if (uuid::CompareUuid128(old_ids.version_uuid.value,
                           new_ids.version_uuid.value) == 0) {
    return MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-UPDATE-VERSION-UUID-UNCHANGED",
        "dml.index_write.update_version_uuid_unchanged",
        "update old/new row images must describe distinct MGA row versions",
        true);
  }
  return OkDiagnostic();
}

struct ParsedDeltaIdentities {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  platform::TypedUuid index_uuid;
  platform::TypedUuid table_uuid;
  platform::TypedUuid transaction_uuid;
};

ParsedDeltaIdentities ParseDeltaIdentities(const DmlIndexWriteEvent& event) {
  ParsedDeltaIdentities result;
  const auto parsed_index =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           event.index.index_uuid);
  if (!parsed_index.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB-DML-HOT-DELTA-INDEX-UUID-PROOF-REQUIRED",
        "dml.hot_delta.index_uuid_proof_required",
        "index_uuid=" + event.index.index_uuid,
        true);
    return result;
  }
  const auto parsed_table =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           event.table_uuid);
  if (!parsed_table.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB-DML-HOT-DELTA-TABLE-UUID-PROOF-REQUIRED",
        "dml.hot_delta.table_uuid_proof_required",
        "table_uuid=" + event.table_uuid,
        true);
    return result;
  }
  const auto parsed_tx =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::transaction,
                                           event.transaction_uuid);
  if (!parsed_tx.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB-DML-HOT-DELTA-TRANSACTION-UUID-PROOF-REQUIRED",
        "dml.hot_delta.transaction_uuid_proof_required",
        "transaction_uuid=" + event.transaction_uuid,
        true);
    return result;
  }
  result.ok = true;
  result.index_uuid = parsed_index.value;
  result.table_uuid = parsed_table.value;
  result.transaction_uuid = parsed_tx.value;
  return result;
}

struct PlannedCell {
  page::IndexBtreeCell cell;
  std::string logical_key;
};

struct PlannedEvent {
  const DmlIndexWriteEvent* event = nullptr;
  page::IndexBtreePhysicalTree* tree = nullptr;
  StagedDeltaLedger* delta_ledger = nullptr;
  std::string family;
  bool unique = false;
  bool deferred_ledger_mutation = false;
  bool hot_like_update = false;
  std::vector<PlannedCell> old_cells;
  std::vector<PlannedCell> new_cells;
};

bool InsertLike(DmlIndexWriteOperation operation) {
  return operation == DmlIndexWriteOperation::insert ||
         operation == DmlIndexWriteOperation::merge_insert;
}

bool UpdateLike(DmlIndexWriteOperation operation) {
  return operation == DmlIndexWriteOperation::update ||
         operation == DmlIndexWriteOperation::merge_update;
}

bool DeleteLike(DmlIndexWriteOperation operation) {
  return operation == DmlIndexWriteOperation::delete_row ||
         operation == DmlIndexWriteOperation::merge_delete;
}

bool MergeLike(DmlIndexWriteOperation operation) {
  return operation == DmlIndexWriteOperation::merge_insert ||
         operation == DmlIndexWriteOperation::merge_update ||
         operation == DmlIndexWriteOperation::merge_delete;
}

bool KeyListEqual(const std::vector<PlannedCell>& left,
                  const std::vector<PlannedCell>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].logical_key != right[i].logical_key) {
      return false;
    }
  }
  return true;
}

EngineApiDiagnostic ValidateProofs(const DmlIndexWriteEvent& event,
                                   const std::string& family,
                                   bool unique) {
  if (event.table_uuid.empty() || event.index.index_uuid.empty()) {
    return Invalid("table_and_index_uuid_required");
  }
  if (event.local_transaction_id == 0 || event.transaction_uuid.empty() ||
      !event.mga_transaction_identity_proof ||
      !event.mga_transaction_finality_authority_proof) {
    return MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-MGA-TXN-PROOF-REQUIRED",
        "dml.index_write.mga_transaction_proof_required",
        "local transaction id, engine transaction identity token, identity proof, and MGA completion proof are required",
        true);
  }
  if (event.rollback_evidence_token.empty() &&
      !event.rollback_safe_structural_evidence) {
    return MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-ROLLBACK-EVIDENCE-REQUIRED",
        "dml.index_write.rollback_evidence_required",
        "rollback evidence token or rollback-safe structural evidence is required",
        true);
  }
  if (!event.index_descriptor_capability_proof) {
    return MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-DESCRIPTOR-CAPABILITY-PROOF-REQUIRED",
        "dml.index_write.descriptor_capability_proof_required",
        "index_uuid=" + event.index.index_uuid,
        true);
  }
  if ((family == "expression" && !event.key_extraction_proof) ||
      (family == "partial" && !event.partial_predicate_proof) ||
      (family == "covering" && !event.covering_payload_proof)) {
    return MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-KEY-PAYLOAD-PROOF-REQUIRED",
        "dml.index_write.key_payload_proof_required",
        "family=" + family + ";index_uuid=" + event.index.index_uuid,
        true);
  }
  if (unique &&
      (!event.unique_preflight_proof ||
       !event.unique_reservation_preflight_proof)) {
    return MakeEngineApiDiagnostic(
        "SB-DML-INDEX-WRITE-UNIQUE-PREFLIGHT-PROOF-REQUIRED",
        "dml.index_write.unique_preflight_proof_required",
        "index_uuid=" + event.index.index_uuid,
        true);
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateDeferredLedgerProofs(
    const DmlIndexWritePathRequest& request,
    const DmlIndexWriteEvent& event,
    bool unique) {
  if (request.cleanup_horizon_token.empty()) {
    return HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-CLEANUP-HORIZON",
        "dml.hot_delta.missing_cleanup_horizon",
        "cleanup horizon token is required before selecting deferred secondary delta maintenance");
  }
  if (!request.durable_mga_inventory_proof) {
    return HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-DURABLE-MGA-PROOF",
        "dml.hot_delta.missing_durable_mga_proof",
        "durable MGA transaction inventory proof is required and remains finality authority");
  }
  if (!request.delta_overlay_read_proof) {
    return HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-OVERLAY-PROOF",
        "dml.hot_delta.missing_overlay_proof",
        "reader-safe secondary delta overlay proof is required before deferred maintenance");
  }
  if (!request.recovery_classification_proof) {
    return HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-RECOVERY-CLASSIFICATION-PROOF",
        "dml.hot_delta.missing_recovery_classification_proof",
        "persistent secondary delta ledger recovery classification proof is required");
  }
  if (unique && (!request.unique_reservation_protocol_proof ||
                 !request.unique_deferred_route_closure_proof)) {
    return HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-UNIQUE-PROOF-REQUIRED",
        "dml.hot_delta.unique_proof_required",
        "unique index " + event.index.index_uuid +
            " requires reservation protocol and deferred route closure proof");
  }
  return OkDiagnostic();
}

struct PlannedCellsResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<PlannedCell> cells;
};

PlannedCellsResult BuildCells(const CrudIndexRecord& index,
                              const DmlIndexWriteRowImage& row,
                              std::string_view label) {
  const auto parsed = ParseRowImageUuids(row, label);
  if (!parsed.ok) {
    return PlannedCellsResult{false, parsed.diagnostic, {}};
  }
  PlannedCellsResult result;
  for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
    const auto encoded = EncodePhysicalKey(index, key);
    if (!encoded.ok) {
      return PlannedCellsResult{false, encoded.diagnostic, {}};
    }
    page::IndexBtreeCell cell;
    cell.key_ordinal = 0;
    cell.encoded_key = encoded.encoded_key;
    cell.row_uuid = parsed.row_uuid;
    cell.version_uuid = parsed.version_uuid;
    result.cells.push_back({std::move(cell), key});
  }
  result.ok = true;
  return result;
}

DmlIndexWritePathResult FailWithEvidence(EngineApiDiagnostic diagnostic,
                                         std::vector<EngineEvidenceReference> evidence) {
  DmlIndexWritePathResult result;
  result.ok = false;
  result.diagnostic = std::move(diagnostic);
  result.evidence = std::move(evidence);
  return result;
}

bool SameRow(const page::IndexBtreePhysicalRowLocator& locator,
             const page::IndexBtreeCell& cell) {
  return uuid::CompareUuid128(locator.row_uuid.value, cell.row_uuid.value) == 0;
}

EngineApiDiagnostic PreflightUniqueInsert(const PlannedEvent& planned,
                                          const page::IndexBtreeCell& cell) {
  const auto scan =
      page::PointLookupIndexBtreePhysicalTree(*planned.tree, cell.encoded_key);
  if (!scan.ok()) {
    return FromPageDiagnostic(scan.diagnostic, "unique_preflight_scan_refused");
  }
  for (const auto& locator : scan.locators) {
    if (!SameRow(locator, cell)) {
      return MakeEngineApiDiagnostic(
          "SB-DML-INDEX-WRITE-UNIQUE-DUPLICATE",
          "dml.index_write.unique_duplicate_refused",
          "index_uuid=" + planned.event->index.index_uuid,
          true);
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ApplyDelete(page::IndexBtreePhysicalTree* tree,
                                const page::IndexBtreeCell& cell,
                                DmlIndexWritePathResult* result) {
  page::IndexBtreePhysicalDeleteRequest request;
  request.cell = cell;
  const auto deleted = page::DeleteIndexBtreeCell(tree, request);
  if (!deleted.ok()) {
    return FromPageDiagnostic(deleted.diagnostic, "physical_delete_refused");
  }
  ++result->physical_deletes;
  for (const auto& evidence : deleted.evidence) {
    result->evidence.push_back({"dml_index_physical_delete_evidence",
                                evidence});
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ApplyInsert(page::IndexBtreePhysicalTree* tree,
                                const page::IndexBtreeCell& cell,
                                bool unique,
                                DmlIndexWritePathResult* result) {
  if (unique) {
    page::IndexBtreePhysicalUniqueInsertRequest request;
    request.cell = cell;
    request.active_duplicate_policy =
        page::IndexBtreePhysicalUniqueActiveDuplicatePolicy::refuse_candidate;
    request.allow_same_row_update = true;
    request.same_row_proof_uuid = cell.row_uuid;
    const auto inserted = page::InsertUniqueIndexBtreeCell(tree, request);
    if (!inserted.ok() || inserted.conflict) {
      if (inserted.conflict) {
        return MakeEngineApiDiagnostic(
            "SB-DML-INDEX-WRITE-UNIQUE-DUPLICATE",
            "dml.index_write.unique_duplicate_refused",
            "physical unique insert refused duplicate visible key",
            true);
      }
      return FromPageDiagnostic(inserted.diagnostic, "physical_unique_insert_refused");
    }
    ++result->physical_inserts;
    for (const auto& evidence : inserted.evidence) {
      result->evidence.push_back({"dml_index_physical_unique_insert_evidence",
                                  evidence});
    }
    return OkDiagnostic();
  }

  page::IndexBtreePhysicalInsertRequest request;
  request.cell = cell;
  const auto inserted = page::InsertIndexBtreeCell(tree, request);
  if (!inserted.ok()) {
    return FromPageDiagnostic(inserted.diagnostic, "physical_insert_refused");
  }
  ++result->physical_inserts;
  for (const auto& evidence : inserted.evidence) {
    result->evidence.push_back({"dml_index_physical_insert_evidence",
                                evidence});
  }
  return OkDiagnostic();
}

idx::SecondaryIndexDeltaLedgerRecord BuildDeltaRecord(
    const DmlIndexWriteEvent& event,
    const ParsedDeltaIdentities& identities,
    const PlannedCell& cell,
    idx::SecondaryIndexDeltaKind kind,
    std::string_view cleanup_horizon_token,
    EngineApiU64 sequence) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  const auto delta_id = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::object,
      CurrentUnixMillis() + sequence);
  record.delta.delta_id = delta_id.ok() ? delta_id.value : platform::TypedUuid{};
  record.delta.index_uuid = identities.index_uuid;
  record.delta.table_uuid = identities.table_uuid;
  record.delta.row_uuid = cell.cell.row_uuid;
  record.delta.version_uuid = cell.cell.version_uuid;
  record.delta.transaction_uuid = identities.transaction_uuid;
  record.delta.local_transaction_id = event.local_transaction_id;
  record.delta.delta_kind = kind;
  record.delta.key_payload = cell.logical_key;
  record.delta.cleanup_horizon_token = std::string(cleanup_horizon_token);
  record.delta.committed = false;
  record.commit_state =
      idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  record.source_evidence_reference =
      "dml_hot_delta_ledger:" + event.index.index_uuid + ":" +
      idx::SecondaryIndexDeltaKindName(kind);
  return record;
}

EngineApiDiagnostic AppendDeferredDeltaCells(
    const PlannedEvent& planned,
    const ParsedDeltaIdentities& identities,
    const std::vector<PlannedCell>& cells,
    idx::SecondaryIndexDeltaKind kind,
    std::string_view cleanup_horizon_token,
    EngineApiU64* sequence,
    DmlIndexWritePathResult* result) {
  for (const auto& cell : cells) {
    const auto appended = idx::AppendPersistentSecondaryIndexDelta(
        &planned.delta_ledger->staged,
        BuildDeltaRecord(*planned.event,
                         identities,
                         cell,
                         kind,
                         cleanup_horizon_token,
                         (*sequence)++),
        planned.delta_ledger->limits);
    if (!appended.ok()) {
      return CoreDiagnosticToEngine(appended.diagnostic,
                                    "SB-DML-HOT-DELTA-APPEND-REFUSED",
                                    "dml.hot_delta.append_refused",
                                    "secondary delta ledger append refused");
    }
    ++result->secondary_delta_ledger_appends;
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ApplyDeferredDeltaMutation(const PlannedEvent& planned,
                                               std::string_view cleanup_horizon_token,
                                               DmlIndexWritePathResult* result) {
  if (planned.delta_ledger == nullptr) {
    return HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-LEDGER-REQUIRED",
        "dml.hot_delta.ledger_required",
        "persistent secondary delta ledger ref is required for deferred mutation");
  }
  const auto identities = ParseDeltaIdentities(*planned.event);
  if (!identities.ok) {
    return identities.diagnostic;
  }

  EngineApiU64 sequence = result->secondary_delta_ledger_appends + 1;
  if (InsertLike(planned.event->operation)) {
    const auto appended = AppendDeferredDeltaCells(planned,
                                                   identities,
                                                   planned.new_cells,
                                                   idx::SecondaryIndexDeltaKind::insert,
                                                   cleanup_horizon_token,
                                                   &sequence,
                                                   result);
    if (appended.error) { return appended; }
  } else if (UpdateLike(planned.event->operation)) {
    auto appended = AppendDeferredDeltaCells(planned,
                                             identities,
                                             planned.old_cells,
                                             idx::SecondaryIndexDeltaKind::update_before,
                                             cleanup_horizon_token,
                                             &sequence,
                                             result);
    if (appended.error) { return appended; }
    appended = AppendDeferredDeltaCells(planned,
                                        identities,
                                        planned.new_cells,
                                        idx::SecondaryIndexDeltaKind::update_after,
                                        cleanup_horizon_token,
                                        &sequence,
                                        result);
    if (appended.error) { return appended; }
  } else if (DeleteLike(planned.event->operation)) {
    const auto appended = AppendDeferredDeltaCells(planned,
                                                   identities,
                                                   planned.old_cells,
                                                   idx::SecondaryIndexDeltaKind::delete_row,
                                                   cleanup_horizon_token,
                                                   &sequence,
                                                   result);
    if (appended.error) { return appended; }
  }

  idx::SecondaryIndexDeltaLedger overlay_delta_ledger;
  overlay_delta_ledger.deltas.reserve(planned.delta_ledger->staged.records.size());
  for (const auto& record : planned.delta_ledger->staged.records) {
    overlay_delta_ledger.deltas.push_back(record.delta);
  }

  std::vector<idx::SecondaryIndexBaseEntry> base_entries;
  base_entries.reserve(planned.old_cells.size());
  for (const auto& cell : planned.old_cells) {
    idx::SecondaryIndexBaseEntry base;
    base.index_uuid = identities.index_uuid;
    base.table_uuid = identities.table_uuid;
    base.row_uuid = cell.cell.row_uuid;
    base.version_uuid = cell.cell.version_uuid;
    base.key_payload = cell.logical_key;
    base.committed_local_transaction_id =
        planned.event->local_transaction_id == 0
            ? 0
            : planned.event->local_transaction_id - 1;
    base_entries.push_back(std::move(base));
  }

  idx::SecondaryIndexOverlayLedger overlay_ledger;
  idx::SecondaryIndexOverlayRequest overlay_request;
  overlay_request.index_uuid = identities.index_uuid;
  overlay_request.table_uuid = identities.table_uuid;
  overlay_request.transaction_uuid = identities.transaction_uuid;
  overlay_request.local_transaction_id = planned.event->local_transaction_id;
  overlay_request.snapshot_high_water_local_transaction_id =
      planned.event->local_transaction_id;
  overlay_request.index_kind = idx::SecondaryIndexKind::non_unique;
  overlay_request.include_own_transaction = true;
  const auto overlay = idx::BuildSecondaryIndexDeltaOverlay(&overlay_ledger,
                                                           base_entries,
                                                           overlay_delta_ledger,
                                                           overlay_request);
  if (!overlay.ok()) {
    return CoreDiagnosticToEngine(overlay.diagnostic,
                                  "SB-DML-HOT-DELTA-OVERLAY-REFUSED",
                                  "dml.hot_delta.overlay_refused",
                                  "secondary delta overlay refused");
  }
  ++result->secondary_delta_overlay_reads;
  result->evidence.push_back({"secondary_delta_overlay_result_entries",
                              std::to_string(overlay.entries.size())});
  result->evidence.push_back({"secondary_delta_overlay_visible_deltas",
                              std::to_string(
                                  overlay.evidence.visible_delta_entries)});
  return OkDiagnostic();
}

}  // namespace

const char* DmlIndexWriteOperationName(DmlIndexWriteOperation operation) {
  switch (operation) {
    case DmlIndexWriteOperation::insert: return "insert";
    case DmlIndexWriteOperation::update: return "update";
    case DmlIndexWriteOperation::delete_row: return "delete";
    case DmlIndexWriteOperation::merge_insert: return "merge_insert";
    case DmlIndexWriteOperation::merge_update: return "merge_update";
    case DmlIndexWriteOperation::merge_delete: return "merge_delete";
  }
  return "unknown";
}

DmlUpdateIndexMaintenanceDecision DecideDmlUpdateIndexMaintenance(
    const DmlUpdateIndexMaintenanceRequest& request) {
  DmlUpdateIndexMaintenanceDecision decision;
  decision.diagnostic = OkDiagnostic();
  decision.evidence.push_back({"hot_like_version_append_selected", "false"});
  decision.evidence.push_back({"unchanged_index_churn_avoided", "false"});
  decision.evidence.push_back({"secondary_delta_ledger_appended", "false"});
  decision.evidence.push_back({"delta_overlay_read_safe", "false"});
  decision.evidence.push_back({"cleanup_horizon_bound", "false"});
  decision.evidence.push_back({"durable_mga_inventory_remains_authority", "true"});
  decision.evidence.push_back({"transaction_finality_authority", "false"});
  decision.evidence.push_back({"parser_or_donor_authority", "false"});
  decision.evidence.push_back({"runtime_route_capability", "false"});
  decision.evidence.push_back({"benchmark_clean", "false"});

  const auto strategy =
      idx::ClassifyIndexDmlMaintenanceStrategy(FamilyFromId(request.family));
  AppendDmlMaintenanceStrategyEvidence(&decision.evidence, strategy);
  if (!strategy.ok()) {
    decision.diagnostic = CoreDiagnosticToEngine(
        strategy.diagnostic,
        "SB-DML-INDEX-MAINTENANCE-STRATEGY-REFUSED",
        "dml.index_maintenance.strategy_refused",
        "DML index maintenance strategy was not admitted");
    decision.reason = "dml_maintenance_strategy_refused";
    return decision;
  }

  if (!RouteSupportsDmlWriteFamily(request.family,
                                   idx::IndexRouteKind::dml_update)) {
    decision.diagnostic =
        CapabilityDiagnostic(request.family, idx::IndexRouteKind::dml_update);
    decision.reason = "unsupported_family_or_capability";
    decision.evidence.push_back({"runtime_route_capability", "false"});
    return decision;
  }
  decision.evidence.push_back({"runtime_route_capability", "true"});
  decision.evidence.push_back({"benchmark_clean", "true"});
  const idx::IndexFamily enum_family = FamilyFromId(request.family);
  const bool deferred_ledger_family = DeferredLedgerDmlFamily(enum_family);

  if (!request.indexed_keys_changed) {
    decision.ok = true;
    decision.mode = DmlUpdateIndexMaintenanceMode::hot_like_version_append;
    decision.synchronous_fallback_required = false;
    decision.reason = "hot_like_unchanged_index_key_update";
    decision.evidence.push_back({"hot_like_version_append_selected", "true"});
    decision.evidence.push_back({"unchanged_index_churn_avoided", "true"});
    decision.evidence.push_back({"old_index_locator_remains_valid_through_mga_chain",
                                 "true"});
    decision.evidence.push_back({"mga_version_chain_finality_engine_owned",
                                 "true"});
    return decision;
  }

  const auto runtime = idx::ResolveDeferredSecondaryIndexRuntimePolicy(
      request.option_envelopes);
  if (!runtime.enabled) {
    if (deferred_ledger_family) {
      decision.diagnostic = HotDeltaDiagnostic(
          "SB-DML-HOT-DELTA-REQUIRED-FOR-FAMILY",
          "dml.hot_delta.required_for_family",
          "non-ordered index families require persistent delta-ledger DML mutation proof");
      decision.reason = runtime.fallback_reason.empty()
                            ? "deferred_secondary_index_required"
                            : runtime.fallback_reason;
      return decision;
    }
    decision.ok = true;
    decision.mode = DmlUpdateIndexMaintenanceMode::synchronous_physical_rewrite;
    decision.synchronous_fallback_required = true;
    decision.reason = runtime.fallback_reason.empty()
                          ? "runtime_deferred_secondary_index_disabled"
                          : runtime.fallback_reason;
    decision.evidence.push_back({"dml_update_synchronous_fallback_reason",
                                 decision.reason});
    return decision;
  }

  if (!deferred_ledger_family) {
    decision.ok = true;
    decision.mode = DmlUpdateIndexMaintenanceMode::synchronous_physical_rewrite;
    decision.synchronous_fallback_required = true;
    decision.reason = request.unique ? "unique_ordered_index_synchronous_rewrite"
                                     : "ordered_index_synchronous_rewrite";
    decision.evidence.push_back({"dml_update_synchronous_fallback_reason",
                                 decision.reason});
    return decision;
  }

  if (request.unique) {
    if (!request.unique_reservation_protocol_proof ||
        !request.unique_deferred_route_closure_proof) {
      decision.diagnostic = HotDeltaDiagnostic(
          "SB-DML-HOT-DELTA-UNIQUE-PROOF-REQUIRED",
          "dml.hot_delta.unique_proof_required",
          "unique indexes require reservation protocol and deferred route closure proof before ledger mutation");
      decision.reason = "unique_deferred_proof_missing";
      decision.evidence.push_back({"unique_deferred_refusal", "true"});
      return decision;
    }
    decision.evidence.push_back({"unique_deferred_reservation_protocol",
                                 "true"});
  }

  if (!request.cleanup_horizon_present) {
    decision.diagnostic = HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-CLEANUP-HORIZON",
        "dml.hot_delta.missing_cleanup_horizon",
        "cleanup horizon token is required before selecting deferred secondary delta maintenance");
    decision.reason = "missing_cleanup_horizon";
    return decision;
  }
  if (!request.durable_mga_inventory_proof) {
    decision.diagnostic = HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-DURABLE-MGA-PROOF",
        "dml.hot_delta.missing_durable_mga_proof",
        "durable MGA transaction inventory proof is required and remains finality authority");
    decision.reason = "missing_durable_mga_proof";
    return decision;
  }
  if (!request.delta_overlay_read_proof) {
    decision.diagnostic = HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-OVERLAY-PROOF",
        "dml.hot_delta.missing_overlay_proof",
        "reader-safe secondary delta overlay proof is required before deferred maintenance");
    decision.reason = "missing_overlay_proof";
    return decision;
  }
  if (!request.recovery_classification_proof) {
    decision.diagnostic = HotDeltaDiagnostic(
        "SB-DML-HOT-DELTA-MISSING-RECOVERY-CLASSIFICATION-PROOF",
        "dml.hot_delta.missing_recovery_classification_proof",
        "persistent secondary delta ledger recovery classification proof is required");
    decision.reason = "missing_recovery_classification_proof";
    return decision;
  }

  decision.ok = true;
  decision.mode = DmlUpdateIndexMaintenanceMode::deferred_secondary_delta_ledger;
  decision.synchronous_fallback_required = false;
  decision.reason = "deferred_secondary_delta_ledger_selected";
  decision.evidence.push_back({"delta_overlay_read_safe", "true"});
  decision.evidence.push_back({"cleanup_horizon_bound", "true"});
  decision.evidence.push_back({"deferred_secondary_delta_ledger_selected",
                               "true"});
  return decision;
}

DmlIndexWritePathResult ApplyDmlIndexWritePath(
    const DmlIndexWritePathRequest& request) {
  DmlIndexWritePathResult result;
  result.diagnostic = OkDiagnostic();
  result.evidence.push_back({"dml_index_write_path", "IRC-050"});
  result.evidence.push_back({"dml_index_write_batch_events",
                             std::to_string(request.events.size())});
  result.evidence.push_back({"mga_finality_authority",
                             "engine_transaction_inventory"});
  result.evidence.push_back({"parser_or_donor_authority", "false"});
  result.evidence.push_back({"hot_like_version_append_selected", "false"});
  result.evidence.push_back({"unchanged_index_churn_avoided", "false"});
  result.evidence.push_back({"secondary_delta_ledger_appended", "false"});
  result.evidence.push_back({"delta_overlay_read_safe", "false"});
  result.evidence.push_back({"cleanup_horizon_bound", "false"});
  result.evidence.push_back({"durable_mga_inventory_remains_authority", "true"});
  result.evidence.push_back({"transaction_finality_authority", "false"});
  result.evidence.push_back({"runtime_route_capability", "false"});
  result.evidence.push_back({"benchmark_clean", "false"});
  result.evidence.push_back({"tree_mutation_visibility_authority", "false"});
  result.evidence.push_back({"tree_mutation_authorization_authority", "false"});
  result.evidence.push_back({"tree_mutation_finality_authority", "false"});
  result.evidence.push_back({"index_runtime_route_available", "false"});
  result.evidence.push_back({"index_benchmark_clean", "false"});
  result.evidence.push_back({"dml_index_write_copy_on_success", "true"});

  std::vector<StagedPhysicalTree> staged_trees;
  staged_trees.reserve(request.physical_trees.size());
  for (const auto& ref : request.physical_trees) {
    if (ref.tree == nullptr) {
      continue;
    }
    if (FindStagedTree(&staged_trees, ref.index_uuid) != nullptr) {
      continue;
    }
    staged_trees.push_back({ref.index_uuid, ref.tree, *ref.tree});
  }

  std::vector<StagedDeltaLedger> staged_ledgers;
  staged_ledgers.reserve(request.secondary_delta_ledgers.size());
  for (const auto& ref : request.secondary_delta_ledgers) {
    if (ref.ledger == nullptr) {
      continue;
    }
    if (FindStagedLedger(&staged_ledgers, ref.index_uuid) != nullptr) {
      continue;
    }
    staged_ledgers.push_back({ref.index_uuid,
                              ref.ledger,
                              *ref.ledger,
                              ref.limits});
  }

  std::vector<PlannedEvent> planned_events;
  planned_events.reserve(request.events.size());
  for (const auto& event : request.events) {
    const std::string family = ResolvedFamily(event.index);
    result.evidence.push_back({"dml_index_write_event_operation",
                               DmlIndexWriteOperationName(event.operation)});
    result.evidence.push_back({"dml_index_write_source_ordinal",
                               std::to_string(event.source_ordinal)});
    result.evidence.push_back({"dml_index_write_action_ordinal",
                               std::to_string(event.action_ordinal)});
    result.evidence.push_back({"dml_index_write_family", family});
    result.evidence.push_back({"dml_index_family_runtime_available",
                               family + "=false"});
    result.evidence.push_back({"dml_index_family_benchmark_clean",
                               family + "=false"});
    const auto route = RouteForOperation(event.operation);
    AppendRouteCapabilityEvidence(&result.evidence, family, route);
    if (MergeLike(event.operation)) {
      ++result.merge_events;
      result.evidence.push_back({"merge_index_write_source_action_order",
                                 std::to_string(event.source_ordinal) + ":" +
                                     DmlIndexWriteOperationName(event.operation) +
                                     ":" + std::to_string(event.action_ordinal)});
    }

    if (!RouteSupportsDmlWriteFamily(family, route)) {
      result.evidence.push_back({"dml_index_family_fail_closed", family});
      return FailWithEvidence(CapabilityDiagnostic(family, route),
                              std::move(result.evidence));
    }
    result.evidence.push_back({"index_runtime_route_available", "true"});
    result.evidence.push_back({"index_benchmark_clean", "true"});

    const bool unique = UniqueIndex(event.index, family);
    const auto proof_status = ValidateProofs(event, family, unique);
    if (proof_status.error) {
      return FailWithEvidence(proof_status, std::move(result.evidence));
    }
    const idx::IndexFamily enum_family = FamilyFromId(family);
    const bool deferred_ledger_family = DeferredLedgerDmlFamily(enum_family);

    PlannedEvent planned;
    planned.event = &event;
    planned.family = family;
    planned.unique = unique;
    if ((UpdateLike(event.operation) || DeleteLike(event.operation)) &&
        !event.has_old_row) {
      return FailWithEvidence(Invalid("old_row_image_required"),
                              std::move(result.evidence));
    }
    if ((InsertLike(event.operation) || UpdateLike(event.operation)) &&
        !event.has_new_row) {
      return FailWithEvidence(Invalid("new_row_image_required"),
                              std::move(result.evidence));
    }
    if (event.has_old_row) {
      auto old_cells = BuildCells(event.index, event.old_row, "old");
      if (!old_cells.ok) {
        return FailWithEvidence(old_cells.diagnostic,
                                std::move(result.evidence));
      }
      planned.old_cells = std::move(old_cells.cells);
    }
    if (event.has_new_row) {
      auto new_cells = BuildCells(event.index, event.new_row, "new");
      if (!new_cells.ok) {
        return FailWithEvidence(new_cells.diagnostic,
                                std::move(result.evidence));
      }
      planned.new_cells = std::move(new_cells.cells);
    }
    if (UpdateLike(event.operation)) {
      const auto continuity = ValidateUpdateRowVersionContinuity(event);
      if (continuity.error) {
        result.evidence.push_back({"dml_update_mga_version_chain_refused",
                                   event.index.index_uuid});
        return FailWithEvidence(continuity, std::move(result.evidence));
      }
      const bool keys_changed = !KeyListEqual(planned.old_cells,
                                              planned.new_cells);
      DmlUpdateIndexMaintenanceRequest decision_request;
      decision_request.indexed_keys_changed = keys_changed;
      decision_request.unique = unique;
      decision_request.family = family;
      decision_request.option_envelopes =
          request.deferred_secondary_index_options;
      decision_request.cleanup_horizon_present =
          !request.cleanup_horizon_token.empty();
      decision_request.durable_mga_inventory_proof =
          request.durable_mga_inventory_proof;
      decision_request.delta_overlay_read_proof =
          request.delta_overlay_read_proof;
      decision_request.recovery_classification_proof =
          request.recovery_classification_proof;
      decision_request.unique_reservation_protocol_proof =
          request.unique_reservation_protocol_proof;
      decision_request.unique_deferred_route_closure_proof =
          request.unique_deferred_route_closure_proof;
      const auto decision =
          DecideDmlUpdateIndexMaintenance(decision_request);
      result.evidence.insert(result.evidence.end(),
                             decision.evidence.begin(),
                             decision.evidence.end());
      if (!decision.ok) {
        result.evidence.push_back({"dml_update_index_maintenance_refused",
                                   decision.reason});
        return FailWithEvidence(decision.diagnostic,
                                std::move(result.evidence));
      }
      planned.hot_like_update =
          decision.mode == DmlUpdateIndexMaintenanceMode::hot_like_version_append;
      planned.deferred_ledger_mutation =
          decision.mode ==
          DmlUpdateIndexMaintenanceMode::deferred_secondary_delta_ledger;
      if (planned.deferred_ledger_mutation) {
        planned.delta_ledger =
            FindStagedLedger(&staged_ledgers, event.index.index_uuid);
        if (planned.delta_ledger == nullptr) {
          return FailWithEvidence(
              MakeEngineApiDiagnostic(
                  "SB-DML-HOT-DELTA-LEDGER-REQUIRED",
                  "dml.hot_delta.ledger_required",
                  "index_uuid=" + event.index.index_uuid,
                  true),
              std::move(result.evidence));
        }
      }
    }
    if (!UpdateLike(event.operation) && deferred_ledger_family) {
      const auto ledger_proof = ValidateDeferredLedgerProofs(request,
                                                             event,
                                                             unique);
      if (ledger_proof.error) {
        return FailWithEvidence(ledger_proof, std::move(result.evidence));
      }
      planned.deferred_ledger_mutation = true;
      planned.delta_ledger =
          FindStagedLedger(&staged_ledgers, event.index.index_uuid);
      if (planned.delta_ledger == nullptr) {
        return FailWithEvidence(
            MakeEngineApiDiagnostic(
                "SB-DML-HOT-DELTA-LEDGER-REQUIRED",
                "dml.hot_delta.ledger_required",
                "index_uuid=" + event.index.index_uuid,
                true),
            std::move(result.evidence));
      }
    }
    if (!planned.deferred_ledger_mutation && !planned.hot_like_update) {
      auto* staged_tree = FindStagedTree(&staged_trees, event.index.index_uuid);
      if (staged_tree == nullptr) {
        return FailWithEvidence(
            MakeEngineApiDiagnostic(
                "SB-DML-INDEX-WRITE-PHYSICAL-TREE-REQUIRED",
                "dml.index_write.physical_tree_required",
                "index_uuid=" + event.index.index_uuid,
                true),
            std::move(result.evidence));
      }
      planned.tree = &staged_tree->staged;
    }
    planned_events.push_back(std::move(planned));
  }

  for (const auto& planned : planned_events) {
    if (!planned.unique || DeleteLike(planned.event->operation)) {
      continue;
    }
    if (UpdateLike(planned.event->operation) &&
        KeyListEqual(planned.old_cells, planned.new_cells)) {
      continue;
    }
    if (planned.deferred_ledger_mutation) {
      result.evidence.push_back({"dml_index_unique_deferred_preflight",
                                 "reservation_protocol_proven"});
      continue;
    }
    for (const auto& cell : planned.new_cells) {
      const auto preflight = PreflightUniqueInsert(planned, cell.cell);
      if (preflight.error) {
        result.evidence.push_back({"dml_index_unique_preflight", "refused"});
        return FailWithEvidence(preflight, std::move(result.evidence));
      }
    }
    result.evidence.push_back({"dml_index_unique_preflight", "accepted"});
  }

  for (const auto& planned : planned_events) {
    const auto* event = planned.event;
    result.evidence.push_back({"dml_index_write_apply_operation",
                               DmlIndexWriteOperationName(event->operation)});
    result.evidence.push_back({"dml_index_write_rollback_safe",
                               event->rollback_evidence_token.empty()
                                   ? "structural"
                                   : event->rollback_evidence_token});
    if (InsertLike(event->operation)) {
      if (planned.deferred_ledger_mutation) {
        const auto deferred = ApplyDeferredDeltaMutation(planned,
                                                         request.cleanup_horizon_token,
                                                         &result);
        if (deferred.error) {
          return FailWithEvidence(deferred, std::move(result.evidence));
        }
        result.evidence.push_back({"secondary_delta_ledger_appended", "true"});
        result.evidence.push_back({"delta_overlay_read_safe", "true"});
        result.evidence.push_back({"cleanup_horizon_bound", "true"});
        result.evidence.push_back({"durable_mga_inventory_remains_authority",
                                   "true"});
        result.evidence.push_back({"dml_index_physical_churn_deferred",
                                   event->index.index_uuid});
        continue;
      }
      for (const auto& cell : planned.new_cells) {
        const auto inserted =
            ApplyInsert(planned.tree, cell.cell, planned.unique, &result);
        if (inserted.error) {
          return FailWithEvidence(inserted, std::move(result.evidence));
        }
      }
      continue;
    }
    if (UpdateLike(event->operation)) {
      if (planned.hot_like_update ||
          KeyListEqual(planned.old_cells, planned.new_cells)) {
        ++result.unchanged_key_noops;
        ++result.hot_like_version_appends;
        result.evidence.push_back({"dml_index_unchanged_key_noop",
                                   event->index.index_uuid});
        result.evidence.push_back({"hot_like_version_append_selected", "true"});
        result.evidence.push_back({"unchanged_index_churn_avoided", "true"});
        result.evidence.push_back({"old_index_locator_remains_valid_through_mga_chain",
                                   event->index.index_uuid});
        result.evidence.push_back({"mga_version_chain_finality_engine_owned",
                                   "true"});
        continue;
      }
      if (planned.deferred_ledger_mutation) {
        const auto deferred = ApplyDeferredDeltaMutation(planned,
                                                         request.cleanup_horizon_token,
                                                         &result);
        if (deferred.error) {
          return FailWithEvidence(deferred, std::move(result.evidence));
        }
        result.evidence.push_back({"secondary_delta_ledger_appended", "true"});
        result.evidence.push_back({"delta_overlay_read_safe", "true"});
        result.evidence.push_back({"cleanup_horizon_bound", "true"});
        result.evidence.push_back({"durable_mga_inventory_remains_authority",
                                   "true"});
        result.evidence.push_back({"dml_index_physical_churn_deferred",
                                   event->index.index_uuid});
        continue;
      }
      for (const auto& cell : planned.old_cells) {
        const auto deleted = ApplyDelete(planned.tree, cell.cell, &result);
        if (deleted.error) {
          return FailWithEvidence(deleted, std::move(result.evidence));
        }
      }
      for (const auto& cell : planned.new_cells) {
        const auto inserted =
            ApplyInsert(planned.tree, cell.cell, planned.unique, &result);
        if (inserted.error) {
          return FailWithEvidence(inserted, std::move(result.evidence));
        }
      }
      continue;
    }
    if (DeleteLike(event->operation)) {
      if (planned.deferred_ledger_mutation) {
        const auto deferred = ApplyDeferredDeltaMutation(planned,
                                                         request.cleanup_horizon_token,
                                                         &result);
        if (deferred.error) {
          return FailWithEvidence(deferred, std::move(result.evidence));
        }
        result.evidence.push_back({"secondary_delta_ledger_appended", "true"});
        result.evidence.push_back({"delta_overlay_read_safe", "true"});
        result.evidence.push_back({"cleanup_horizon_bound", "true"});
        result.evidence.push_back({"durable_mga_inventory_remains_authority",
                                   "true"});
        result.evidence.push_back({"dml_index_physical_churn_deferred",
                                   event->index.index_uuid});
        continue;
      }
      for (const auto& cell : planned.old_cells) {
        const auto deleted = ApplyDelete(planned.tree, cell.cell, &result);
        if (deleted.error) {
          return FailWithEvidence(deleted, std::move(result.evidence));
        }
      }
      continue;
    }
  }

  for (auto& staged : staged_trees) {
    *staged.original = std::move(staged.staged);
  }
  for (auto& staged : staged_ledgers) {
    *staged.original = std::move(staged.staged);
  }

  result.ok = true;
  result.evidence.push_back({"dml_index_write_path_result", "applied"});
  result.evidence.push_back({"dml_index_write_committed_tree_count",
                             std::to_string(staged_trees.size())});
  result.evidence.push_back({"dml_secondary_delta_committed_ledger_count",
                             std::to_string(staged_ledgers.size())});
  result.evidence.push_back({"dml_index_physical_inserts",
                             std::to_string(result.physical_inserts)});
  result.evidence.push_back({"dml_index_physical_deletes",
                             std::to_string(result.physical_deletes)});
  result.evidence.push_back({"dml_index_unchanged_key_noops",
                             std::to_string(result.unchanged_key_noops)});
  result.evidence.push_back({"dml_index_merge_events",
                             std::to_string(result.merge_events)});
  result.evidence.push_back({"dml_hot_like_version_appends",
                             std::to_string(result.hot_like_version_appends)});
  result.evidence.push_back({"dml_secondary_delta_ledger_appends",
                             std::to_string(result.secondary_delta_ledger_appends)});
  result.evidence.push_back({"dml_secondary_delta_overlay_reads",
                             std::to_string(result.secondary_delta_overlay_reads)});
  return result;
}

}  // namespace scratchbird::engine::internal_api
