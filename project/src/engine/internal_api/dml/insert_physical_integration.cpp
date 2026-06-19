// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-INSERT-PHYSICAL-INTEGRATION-ANCHOR
#include "insert_physical_integration.hpp"

#include "api_diagnostics.hpp"
#include "bulk_constraint_proof.hpp"
#include "crud_support/crud_store.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/index_apply_locality_bridge.hpp"
#include "dml/insert_batch.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"
#include "dml/write_result_policy.hpp"
#include "bulk_placement_order.hpp"
#include "domain_support/domain_store.hpp"
#include "ipar_fault_injection.hpp"
#include "metric_contracts.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "ordered_ingest.hpp"
#include "observability/dml_summary_counters.hpp"
#include "index_bulk_publish_recovery.hpp"
#include "index_root_generation_publish.hpp"
#include "physical_mga_cow_store.hpp"
#include "sorted_bulk_index_build.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api::dml {
namespace {

using DirectSteadyClock = std::chrono::steady_clock;

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status IntegrationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status IntegrationErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

InsertPhysicalIntegrationResult Refuse(std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail) {
  InsertPhysicalIntegrationResult result;
  result.status = IntegrationErrorStatus();
  result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                             std::move(diagnostic_code),
                                                             std::move(message_key),
                                                             std::move(detail));
  return result;
}

std::string EvidenceRef(const std::string& kind, const TypedUuid& uuid) {
  std::ostringstream out;
  out << kind << ":" << scratchbird::core::uuid::UuidToString(uuid.value);
  return out.str();
}

bool DirectIndexIsUnique(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(),
                   index.key_envelopes.end(),
                   "unique") != index.key_envelopes.end();
}

bool DirectOptionEquals(const std::string& actual, const std::string& expected) {
  if (actual == expected) {
    return true;
  }
  const auto equals = expected.find('=');
  if (equals == std::string::npos) {
    return false;
  }
  return actual == expected.substr(0, equals) + ":" + expected.substr(equals + 1);
}

bool DirectOptionEnabled(const DirectPhysicalBulkAppendRequest& request,
                         const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (DirectOptionEquals(candidate, option)) {
      return true;
    }
  }
  return false;
}

TypedUuid ParseDirectTypedUuid(UuidKind kind, const std::string& text);

std::string DirectOptionValue(const DirectPhysicalBulkAppendRequest& request,
                              const std::string& key) {
  const std::string equals_prefix = key + "=";
  const std::string colon_prefix = key + ":";
  for (const auto& candidate : request.option_envelopes) {
    if (candidate.rfind(equals_prefix, 0) == 0) {
      return candidate.substr(equals_prefix.size());
    }
    if (candidate.rfind(colon_prefix, 0) == 0) {
      return candidate.substr(colon_prefix.size());
    }
  }
  return {};
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool IsDirectTruthyValue(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return lowered == "1" || lowered == "true" || lowered == "enabled" ||
         lowered == "on" || lowered == "required";
}

bool IsDirectFalsyValue(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return lowered == "0" || lowered == "false" || lowered == "disabled" ||
         lowered == "off";
}

bool DirectPageExtentPreallocationRequired(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string mode = DirectOptionValue(request, "page_extent_preallocation");
  if (!mode.empty()) {
    return IsDirectTruthyValue(mode);
  }
  const std::string odf_mode =
      DirectOptionValue(request, "odf042.page_extent_preallocation");
  if (!odf_mode.empty()) {
    return IsDirectTruthyValue(odf_mode);
  }
  return false;
}

std::string DirectPageExtentPreallocationPrecheckFailure(
    const DirectPhysicalBulkAppendRequest& request) {
  if (!DirectPageExtentPreallocationRequired(request)) {
    return {};
  }
  if (!DirectOptionEnabled(request, "page_allocation.runtime=enabled")) {
    return "page_extent_preallocation_disabled";
  }
  const TypedUuid database_uuid =
      ParseDirectTypedUuid(UuidKind::database, request.context.database_uuid.canonical);
  const TypedUuid transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction,
                           request.context.transaction_uuid.canonical);
  const TypedUuid object_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  if (!database_uuid.valid() || !transaction_uuid.valid() ||
      !object_uuid.valid() || request.context.local_transaction_id == 0) {
    return "page_extent_preallocation_authority_missing";
  }
  const std::string filespace = DirectOptionValue(request, "page_allocation.filespace_uuid");
  if (!filespace.empty() &&
      !ParseDirectTypedUuid(UuidKind::filespace, filespace).valid()) {
    return "page_extent_preallocation_invalid_filespace";
  }
  const std::string disabled = DirectOptionValue(request, "page_extent_preallocation");
  if (!disabled.empty() && IsDirectFalsyValue(disabled)) {
    return "page_extent_preallocation_disabled";
  }
  return {};
}

TypedUuid ParseDirectTypedUuid(UuidKind kind, const std::string& text) {
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(kind, text);
  return parsed.ok() ? parsed.value : TypedUuid{};
}

std::string TypedUuidText(const TypedUuid& uuid) {
  return uuid.valid() ? scratchbird::core::uuid::UuidToString(uuid.value) : std::string{};
}

std::string DiagnosticDetail(const DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      return argument.value;
    }
  }
  return {};
}

EngineApiDiagnostic CoreBulkDiagnosticToEngine(const DiagnosticRecord& diagnostic,
                                               const std::string& fallback_detail) {
  std::string detail = DiagnosticDetail(diagnostic);
  if (detail.empty()) {
    detail = fallback_detail;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty()
                                     ? "SB_ENGINE_API_INVALID_REQUEST"
                                     : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty()
                                     ? "engine.api.invalid_request"
                                     : diagnostic.message_key,
                                 std::move(detail),
                                 true);
}

void AddStrictBulkLifecycleEvidence(
    const scratchbird::core::bulk_load::StrictBulkLoadLedger& ledger,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  for (const auto& record : ledger.evidence) {
    const std::string previous =
        scratchbird::core::bulk_load::StrictBulkLoadStateName(record.previous_state);
    const std::string next =
        scratchbird::core::bulk_load::StrictBulkLoadStateName(record.new_state);
    evidence->push_back({"strict_bulk_load_lifecycle_action", record.action});
    evidence->push_back({"strict_bulk_load_state_transition", previous + "->" + next});
    evidence->push_back({"strict_bulk_load_state", next});
    if (record.bulk_load_id.valid()) {
      evidence->push_back({"strict_bulk_load_id", TypedUuidText(record.bulk_load_id)});
    }
    if (!record.visibility_fence.empty()) {
      evidence->push_back({"strict_bulk_load_visibility_fence", record.visibility_fence});
    }
    if (!record.diagnostic_code.empty()) {
      evidence->push_back({"strict_bulk_load_diagnostic_code", record.diagnostic_code});
    }
    if (record.new_state ==
        scratchbird::core::bulk_load::StrictBulkLoadState::finalize_evidence_durable) {
      evidence->push_back({"strict_bulk_load_finalize_evidence_durable", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::published_visible) {
      evidence->push_back({"strict_bulk_load_published_visible", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::recovery_required) {
      evidence->push_back({"strict_bulk_load_recovery_required", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::rolled_back) {
      evidence->push_back({"strict_bulk_load_rollback", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::refused) {
      evidence->push_back({"strict_bulk_load_refused_state", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::quarantine) {
      evidence->push_back({"strict_bulk_load_quarantine", "true"});
    }
  }
}

void AddStrictBulkRecoveryEvidence(
    const scratchbird::core::bulk_load::StrictBulkLoadRecoveryResult& recovery,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  for (const auto& classification : recovery.classifications) {
    evidence->push_back({"strict_bulk_load_recovery_observed_state",
                         scratchbird::core::bulk_load::StrictBulkLoadStateName(
                             classification.observed_state)});
    evidence->push_back({"strict_bulk_load_recovery_action",
                         scratchbird::core::bulk_load::StrictBulkLoadRecoveryActionName(
                             classification.action)});
    evidence->push_back({"strict_bulk_load_recovery_fail_closed",
                         classification.fail_closed ? "true" : "false"});
    evidence->push_back({"strict_bulk_load_recovery_reason",
                         classification.stable_reason});
  }
}

std::vector<std::string> DirectSplit(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (char ch : value) {
    if (ch == delimiter) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}

std::string DirectTrimAscii(std::string value) {
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\r' || value.front() == '\n')) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  return value;
}

std::map<std::string, std::string> DirectDescriptorFields(
    const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  for (const auto& raw_part : DirectSplit(descriptor, ';')) {
    const std::string part = DirectTrimAscii(raw_part);
    if (part.empty()) {
      continue;
    }
    const auto equal = part.find('=');
    if (equal == std::string::npos) {
      fields[LowerAscii(part)] = "true";
    } else {
      fields[LowerAscii(DirectTrimAscii(part.substr(0, equal)))] =
          DirectTrimAscii(part.substr(equal + 1));
    }
  }
  return fields;
}

std::string DirectFieldOrEmpty(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto found = fields.find(key);
    if (found != fields.end()) {
      return found->second;
    }
  }
  return {};
}

bool DirectBoolField(const std::map<std::string, std::string>& fields,
                     std::initializer_list<const char*> keys) {
  const std::string value = LowerAscii(DirectFieldOrEmpty(fields, keys));
  return value == "true" || value == "1" || value == "yes" ||
         value == "required" || value == "primary" || value == "unique";
}

bool DirectNullValue(const std::string& value) {
  return value == "<NULL>";
}

inline constexpr char kDirectNullMarker[] = "<NULL>";

std::string DirectConstraintUuid(
    const std::map<std::string, std::string>& fields,
    const CrudTableRecord& table,
    const std::string& column_name,
    const std::string& constraint_class) {
  const std::string explicit_uuid =
      DirectFieldOrEmpty(fields, {"constraint_uuid", "uuid"});
  if (!explicit_uuid.empty()) {
    return explicit_uuid;
  }
  return "descriptor:" + table.table_uuid + ":" + column_name + ":" +
         constraint_class;
}

struct DirectForeignKeyReference {
  std::string parent_table_uuid;
  std::string parent_column;
};

std::optional<DirectForeignKeyReference> DirectParseForeignKeyReference(
    const std::map<std::string, std::string>& fields) {
  DirectForeignKeyReference reference;
  reference.parent_table_uuid =
      DirectFieldOrEmpty(fields,
                         {"referenced_table_uuid",
                          "foreign_table_uuid",
                          "foreign_table"});
  reference.parent_column =
      DirectFieldOrEmpty(fields,
                         {"referenced_column",
                          "foreign_column",
                          "parent_column"});
  if (!reference.parent_table_uuid.empty() && !reference.parent_column.empty()) {
    return reference;
  }
  const std::string envelope =
      DirectFieldOrEmpty(fields, {"foreign_key", "references", "fk"});
  if (envelope.empty()) {
    return std::nullopt;
  }
  const auto colon = envelope.find(':');
  const auto dot = envelope.rfind('.');
  const auto open = envelope.find('(');
  const auto close = envelope.rfind(')');
  if (colon != std::string::npos) {
    reference.parent_table_uuid = envelope.substr(0, colon);
    reference.parent_column = envelope.substr(colon + 1);
  } else if (dot != std::string::npos) {
    reference.parent_table_uuid = envelope.substr(0, dot);
    reference.parent_column = envelope.substr(dot + 1);
  } else if (open != std::string::npos && close == envelope.size() - 1 &&
             close > open + 1) {
    reference.parent_table_uuid = envelope.substr(0, open);
    reference.parent_column = envelope.substr(open + 1, close - open - 1);
  }
  if (reference.parent_table_uuid.empty() || reference.parent_column.empty()) {
    return std::nullopt;
  }
  return reference;
}

bool DirectDescriptorDeclaresForeignKey(
    const std::map<std::string, std::string>& fields) {
  return !DirectFieldOrEmpty(
              fields,
              {"foreign_key",
               "references",
               "fk",
               "referenced_table_uuid",
               "foreign_table_uuid",
               "foreign_table",
               "referenced_column",
               "foreign_column",
               "parent_column"})
              .empty();
}

std::vector<std::string> DirectIndexKeyColumns(const CrudIndexRecord& index) {
  std::vector<std::string> columns;
  for (const auto& envelope : index.key_envelopes) {
    const std::string lowered = LowerAscii(envelope);
    if (lowered == "unique" || lowered == "primary_key" ||
        lowered == "pk" || lowered == "not_null") {
      continue;
    }
    if (envelope.rfind("identity:", 0) == 0) {
      columns.push_back(envelope.substr(9));
    } else {
      columns.push_back(envelope);
    }
  }
  if (columns.empty() && !index.column_name.empty()) {
    columns.push_back(index.column_name);
  }
  return columns;
}

bool DirectIndexCoversColumn(const CrudIndexRecord& index,
                             const std::string& column_name) {
  const auto columns = DirectIndexKeyColumns(index);
  return columns.size() == 1 && columns.front() == column_name;
}

std::optional<CrudIndexRecord> DirectVisibleUniqueIndexForColumn(
    const CrudState& state,
    const std::string& table_uuid,
    const std::string& column_name,
    std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (DirectIndexIsUnique(index) && DirectIndexCoversColumn(index, column_name)) {
      return index;
    }
  }
  return std::nullopt;
}

void AddCoreProofEvidence(
    const std::vector<scratchbird::core::bulk_load::BulkConstraintProofEvidence>& source,
    std::vector<EngineEvidenceReference>* target) {
  if (target == nullptr) {
    return;
  }
  for (const auto& evidence : source) {
    target->push_back({evidence.evidence_kind, evidence.evidence_id});
  }
}

struct DirectBulkConstraintProofSelection {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<EngineEvidenceReference> evidence;
};

scratchbird::core::bulk_load::BulkConstraintProofKeyRef DirectProofKey(
    std::string key,
    std::string row_uuid,
    std::string version_uuid,
    std::uint64_t source_ordinal) {
  scratchbird::core::bulk_load::BulkConstraintProofKeyRef ref;
  ref.encoded_key = std::move(key);
  ref.row_uuid = std::move(row_uuid);
  ref.version_uuid = std::move(version_uuid);
  ref.source_ordinal = source_ordinal;
  ref.null_key = ref.encoded_key.find("<NULL>") != std::string::npos ||
                 DirectNullValue(ref.encoded_key);
  return ref;
}

void AddVisibleRowKeysForProof(
    const CrudState& state,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    std::vector<scratchbird::core::bulk_load::BulkConstraintProofKeyRef>* keys) {
  if (keys == nullptr) {
    return;
  }
  std::uint64_t ordinal = 0;
  std::set<std::string> visible_row_keys;
  for (const auto& row :
       VisibleCrudRowsForContext(state, index.table_uuid, context)) {
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      keys->push_back(DirectProofKey(key,
                                     row.row_uuid,
                                     row.version_uuid,
                                     ordinal++));
      visible_row_keys.insert(row.row_uuid + "\n" + key);
    }
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != index.index_uuid ||
        entry.table_uuid != index.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id) ||
        visible_row_keys.count(entry.row_uuid + "\n" + entry.key_value) == 0) {
      continue;
    }
    keys->push_back(DirectProofKey(entry.key_value,
                                   entry.row_uuid,
                                   entry.version_uuid,
                                   ordinal++));
  }
}

void AddVisibleRowKeysForSortedBuild(
    const CrudState& state,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    std::vector<scratchbird::core::index::SortedBulkIndexRowInput>* keys) {
  if (keys == nullptr) {
    return;
  }
  std::uint64_t ordinal = 0;
  std::set<std::string> visible_row_keys;
  for (const auto& row :
       VisibleCrudRowsForContext(state, index.table_uuid, context)) {
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      scratchbird::core::index::SortedBulkIndexRowInput input;
      input.encoded_key = key;
      input.row_uuid = row.row_uuid;
      input.version_uuid = row.version_uuid;
      input.payload_value = CrudFieldValue(row.values, index.column_name);
      input.source_ordinal = ordinal++;
      input.null_key = DirectNullValue(input.encoded_key) ||
                       input.encoded_key.find("<NULL>") !=
                           std::string::npos;
      keys->push_back(std::move(input));
      visible_row_keys.insert(row.row_uuid + "\n" + key);
    }
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != index.index_uuid ||
        entry.table_uuid != index.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id) ||
        visible_row_keys.count(entry.row_uuid + "\n" + entry.key_value) ==
            0) {
      continue;
    }
    scratchbird::core::index::SortedBulkIndexRowInput input;
    input.encoded_key = entry.key_value;
    input.row_uuid = entry.row_uuid;
    input.version_uuid = entry.version_uuid;
    input.payload_value = entry.payload_value;
    input.source_ordinal = ordinal++;
    input.null_key = DirectNullValue(input.encoded_key) ||
                     input.encoded_key.find("<NULL>") != std::string::npos;
    keys->push_back(std::move(input));
  }
}

void AddVisibleParentKeysForProof(
    const CrudState& state,
    const EngineRequestContext& context,
    const std::string& parent_table_uuid,
    const std::string& parent_column,
    const CrudIndexRecord& parent_index,
    std::vector<scratchbird::core::bulk_load::BulkConstraintProofKeyRef>* keys) {
  if (keys == nullptr) {
    return;
  }
  std::uint64_t ordinal = 0;
  std::set<std::string> visible_parent_keys;
  for (const auto& row :
       VisibleCrudRowsForContext(state, parent_table_uuid, context)) {
    const std::string key = CrudFieldValue(row.values, parent_column);
    keys->push_back(DirectProofKey(key,
                                   row.row_uuid,
                                   row.version_uuid,
                                   ordinal++));
    visible_parent_keys.insert(row.row_uuid + "\n" + key);
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != parent_index.index_uuid ||
        entry.table_uuid != parent_table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id) ||
        visible_parent_keys.count(entry.row_uuid + "\n" + entry.key_value) ==
            0) {
      continue;
    }
    keys->push_back(DirectProofKey(entry.key_value,
                                   entry.row_uuid,
                                   entry.version_uuid,
                                   ordinal++));
  }
}

DirectBulkConstraintProofSelection BuildDirectBulkConstraintProof(
    const DirectPhysicalBulkAppendRequest& request,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& visible_indexes,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch) {
  DirectBulkConstraintProofSelection selection;
  scratchbird::core::bulk_load::BulkConstraintProofRequest proof_request;
  proof_request.database_uuid =
      ParseDirectTypedUuid(UuidKind::database, request.context.database_uuid.canonical);
  proof_request.object_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  proof_request.transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction,
                           request.context.transaction_uuid.canonical);
  proof_request.local_transaction_id = request.context.local_transaction_id;
  proof_request.route = "direct_physical_bulk";
  proof_request.direct_physical_bulk = true;
  proof_request.strict_bulk_load = request.strict_bulk_load_requested;
  std::set<std::string> proofed_unique_indexes;

  auto fail_before_proof = [&](std::string reason) {
    selection.ok = false;
    selection.failure_reason = std::move(reason);
    selection.diagnostic =
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     selection.failure_reason);
    return selection;
  };

  for (const auto& [column_name, descriptor] : table.columns) {
    const auto fields = DirectDescriptorFields(descriptor);
    const bool primary_key = DirectBoolField(fields, {"primary_key", "pk"});
    const bool unique_key =
        primary_key || DirectBoolField(fields, {"unique", "unique_key"});
    if (unique_key) {
      std::optional<CrudIndexRecord> support_index;
      for (const auto& index : visible_indexes) {
        if (DirectIndexIsUnique(index) &&
            DirectIndexCoversColumn(index, column_name)) {
          support_index = index;
          break;
        }
      }
      if (!support_index) {
        return fail_before_proof("bulk_unique_proof_support_index_missing");
      }

      scratchbird::core::bulk_load::BulkUniqueProofRequest unique;
      unique.constraint_uuid =
          DirectConstraintUuid(fields,
                               table,
                               column_name,
                               primary_key ? "primary_key" : "unique_key");
      unique.index_uuid = support_index->index_uuid;
      unique.table_uuid = table.table_uuid;
      unique.column_name = column_name;
      const std::string null_policy =
          LowerAscii(DirectFieldOrEmpty(fields,
                                        {"null_policy", "unique_null_policy"}));
      unique.nulls_distinct =
          !primary_key &&
          (null_policy.empty() || null_policy == "nulls_distinct");
      for (std::size_t row_index = 0; row_index < logical_value_batch.size();
           ++row_index) {
        for (const auto& key :
             CrudIndexKeysForValues(*support_index,
                                    logical_value_batch[row_index])) {
          unique.incoming_keys.push_back(
              DirectProofKey(key,
                             staged_rows[row_index].row_uuid,
                             staged_rows[row_index].version_uuid,
                             row_index));
        }
      }
      AddVisibleRowKeysForProof(state,
                                request.context,
                                *support_index,
                                &unique.visible_keys);
      proofed_unique_indexes.insert(support_index->index_uuid);
      proof_request.unique_proofs.push_back(std::move(unique));
    }

    if (!DirectDescriptorDeclaresForeignKey(fields)) {
      continue;
    }
    const auto reference = DirectParseForeignKeyReference(fields);
    if (!reference) {
      return fail_before_proof("bulk_fk_proof_descriptor_invalid");
    }
    const auto parent = FindVisibleCrudTable(state,
                                             reference->parent_table_uuid,
                                             request.context.local_transaction_id);
    if (!parent) {
      return fail_before_proof("bulk_fk_proof_parent_table_not_visible");
    }
    const auto parent_index = DirectVisibleUniqueIndexForColumn(
        state,
        parent->table_uuid,
        reference->parent_column,
        request.context.local_transaction_id);
    if (!parent_index) {
      return fail_before_proof("bulk_fk_proof_parent_index_missing");
    }

    scratchbird::core::bulk_load::BulkForeignKeyProofRequest foreign_key;
    foreign_key.constraint_uuid =
        DirectConstraintUuid(fields, table, column_name, "foreign_key");
    foreign_key.child_table_uuid = table.table_uuid;
    foreign_key.child_column_name = column_name;
    foreign_key.parent_table_uuid = parent->table_uuid;
    foreign_key.parent_column_name = reference->parent_column;
    foreign_key.parent_index_uuid = parent_index->index_uuid;
    foreign_key.batch_local_parent_allowed = true;
    for (std::size_t row_index = 0; row_index < logical_value_batch.size();
         ++row_index) {
      foreign_key.child_keys.push_back(
          DirectProofKey(CrudFieldValue(logical_value_batch[row_index],
                                        column_name),
                         staged_rows[row_index].row_uuid,
                         staged_rows[row_index].version_uuid,
                         row_index));
      if (parent->table_uuid == table.table_uuid) {
        foreign_key.batch_parent_keys.push_back(
            DirectProofKey(CrudFieldValue(logical_value_batch[row_index],
                                          reference->parent_column),
                           staged_rows[row_index].row_uuid,
                           staged_rows[row_index].version_uuid,
                           row_index));
      }
    }
    AddVisibleParentKeysForProof(state,
                                 request.context,
                                 parent->table_uuid,
                                 reference->parent_column,
                                 *parent_index,
                                 &foreign_key.visible_parent_keys);
    proof_request.foreign_key_proofs.push_back(std::move(foreign_key));
  }

  for (const auto& index : visible_indexes) {
    if (!DirectIndexIsUnique(index) ||
        proofed_unique_indexes.count(index.index_uuid) != 0) {
      continue;
    }
    scratchbird::core::bulk_load::BulkUniqueProofRequest unique;
    unique.constraint_uuid = "index:" + index.index_uuid + ":unique_key";
    unique.index_uuid = index.index_uuid;
    unique.table_uuid = table.table_uuid;
    const auto columns = DirectIndexKeyColumns(index);
    unique.column_name = columns.empty() ? index.column_name : columns.front();
    unique.nulls_distinct = true;
    for (std::size_t row_index = 0; row_index < logical_value_batch.size();
         ++row_index) {
      for (const auto& key : CrudIndexKeysForValues(index,
                                                    logical_value_batch[row_index])) {
        unique.incoming_keys.push_back(
            DirectProofKey(key,
                           staged_rows[row_index].row_uuid,
                           staged_rows[row_index].version_uuid,
                           row_index));
      }
    }
    AddVisibleRowKeysForProof(state,
                              request.context,
                              index,
                              &unique.visible_keys);
    proof_request.unique_proofs.push_back(std::move(unique));
  }

  const auto proven =
      scratchbird::core::bulk_load::ProveBulkConstraints(proof_request);
  AddCoreProofEvidence(proven.evidence, &selection.evidence);
  if (!proven.ok()) {
    selection.ok = false;
    selection.failure_reason = proven.refusal_reason.empty()
                                   ? "bulk_constraint_proof_refused"
                                   : proven.refusal_reason;
    selection.diagnostic = CoreBulkDiagnosticToEngine(proven.diagnostic,
                                                      selection.failure_reason);
    return selection;
  }
  return selection;
}

std::vector<CrudIndexRecord> DirectSynchronousIndexes(
    const InsertBatchContext& batch_context) {
  std::vector<CrudIndexRecord> indexes;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action == InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    indexes.push_back(entry.index);
  }
  return indexes;
}

EngineApiU64 DirectUniqueIndexProbeCount(
    const std::vector<CrudIndexRecord>& visible_indexes,
    std::size_t row_count) {
  EngineApiU64 unique_indexes = 0;
  for (const auto& index : visible_indexes) {
    if (DirectIndexIsUnique(index)) {
      ++unique_indexes;
    }
  }
  return unique_indexes * static_cast<EngineApiU64>(row_count);
}

std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> DirectDeltaEntries(
    const InsertBatchContext& batch_context,
    const CrudRowVersionRecord& row_record,
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action != InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    MgaSecondaryIndexDeltaLedgerEntryInput input;
    input.index = entry.index;
    input.table_uuid = batch_context.target_object_uuid;
    input.row_uuid = row_record.row_uuid;
    input.version_uuid = row_record.version_uuid;
    input.values = values;
    input.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::insert;
    input.source_evidence_reference =
        "engine.dml.direct_physical_bulk.secondary_index_delta:" +
        batch_context.statement_uuid;
    entries.push_back(std::move(input));
  }
  return entries;
}

std::vector<MgaIndexEntryAppendBatch> DirectIndexAppendBatches(
    const std::vector<CrudIndexRecord>& indexes,
    const std::string& table_uuid,
    const std::vector<MgaIndexEntryRowInput>& rows) {
  std::vector<MgaIndexEntryAppendBatch> batches;
  batches.reserve(indexes.size());
  for (const auto& index : indexes) {
    MgaIndexEntryAppendBatch batch;
    batch.index = index;
    batch.table_uuid = table_uuid;
    batch.rows = rows;
    batches.push_back(std::move(batch));
  }
  return batches;
}

scratchbird::core::index::IndexFamily DirectCoreIndexFamily(
    const CrudIndexRecord& index) {
  namespace idx = scratchbird::core::index;
  const std::string family =
      index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (family == kCrudIndexFamilyBtree) {
    return DirectIndexIsUnique(index) ? idx::IndexFamily::unique_btree
                                      : idx::IndexFamily::btree;
  }
  if (family == kCrudIndexFamilyExpression) {
    return idx::IndexFamily::expression;
  }
  if (family == kCrudIndexFamilyPartial) {
    return idx::IndexFamily::partial;
  }
  if (family == kCrudIndexFamilyCovering) {
    return idx::IndexFamily::covering;
  }
  return idx::IndexFamily::unknown;
}

bool DirectSortedBulkIndexBuildEnabled(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "sorted_bulk_index_build");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf = DirectOptionValue(request, "odf044.sorted_bulk_index_build");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

bool DirectOptionTruthy(const DirectPhysicalBulkAppendRequest& request,
                        const std::string& key) {
  const std::string value = DirectOptionValue(request, key);
  return !value.empty() && IsDirectTruthyValue(value);
}

bool DirectOpaqueColumnsAllowed(const DirectPhysicalBulkAppendRequest& request) {
  return DirectOptionEnabled(request, "bulk.allow_opaque_columns=true") ||
         DirectOptionTruthy(request, "bulk.allow_opaque_columns");
}

bool DirectDeferredIndexBenchmarkCleanRequired(
    const DirectPhysicalBulkAppendRequest& request) {
  return DirectOptionTruthy(
             request,
             "orh.deferred_index.require_benchmark_clean") ||
         DirectOptionTruthy(
             request,
             "orh.deferred_index_bulk_publish.require_benchmark_clean");
}

bool DirectDeferredIndexCallerProofFlagPresent(
    const DirectPhysicalBulkAppendRequest& request) {
  static constexpr const char* kCallerProofFlags[] = {
      "index_correctness_proven",
      "sorted_root_publish_recovery_proof",
      "rollback_proof",
      "reopen_repair_rebuild_proof",
      "mga_visibility_recheck_proof",
      "security_recheck_proof",
      "authoritative_base_repair_proof"};
  for (const auto* suffix : kCallerProofFlags) {
    if (!DirectOptionValue(request, std::string("orh.deferred_index.") + suffix)
             .empty() ||
        !DirectOptionValue(request,
                           std::string("orh.deferred_index_bulk_publish.") +
                               suffix)
             .empty()) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic DeferredIndexRouteNotSelectedDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.ROUTE_NOT_SELECTED",
      "orh.deferred_index_bulk_publish.route_not_selected",
      "benchmark-clean deferred sorted bulk publish was required but the live sorted bulk route was not selected",
      true);
}

EngineApiDiagnostic DeferredIndexFamilyUnsupportedDiagnostic(
    const std::string& family) {
  return MakeEngineApiDiagnostic(
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.FAMILY_ROUTE_UNSUPPORTED",
      "orh.deferred_index_bulk_publish.family_route_unsupported",
      "deferred sorted bulk publish is not an ordered write route for index family " +
          family,
      true);
}

EngineApiDiagnostic DeferredIndexUniqueReservationProofRequiredDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.UNIQUE_RESERVATION_PROOF_REQUIRED",
      "orh.deferred_index_bulk_publish.unique_reservation_proof_required",
      "deferred sorted bulk publish cannot claim unique index behavior without reservation ledger proof",
      true);
}

void AddRootPublishEvidence(
    const scratchbird::core::index::IndexRootGenerationPublishResult& publish,
    std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_publish_provider",
       "core.index.PublishIndexRootGeneration"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_publish_authorized",
       publish.root_publish_authorized ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_reopen_safe",
       publish.reopen_safe_metadata_contract ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_rollback_safe",
       publish.rollback_safe_metadata_contract ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_index_metadata_recovery_authority",
       publish.recovery_authority ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_index_metadata_finality_authority",
       publish.transaction_finality_authority ? "true" : "false"});
  for (const auto& item : publish.evidence) {
    evidence->push_back(
        {"sorted_bulk_root_publish." + item.evidence_kind,
         item.evidence_id});
  }
}

std::string DirectBulkActiveRootName(
    scratchbird::core::index::IndexBulkPublishActiveRoot active_root) {
  using scratchbird::core::index::IndexBulkPublishActiveRoot;
  switch (active_root) {
    case IndexBulkPublishActiveRoot::old_root:
      return "old_root";
    case IndexBulkPublishActiveRoot::new_root:
      return "new_root";
    case IndexBulkPublishActiveRoot::none:
      return "none";
  }
  return "unknown";
}

void AddRootRecoveryEvidence(
    const std::string& crash_point,
    const scratchbird::core::index::IndexBulkPublishRecoveryResult& recovery,
    std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_recovery_provider",
       "core.index.RecoverSortedBulkRootPublish"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point,
       recovery.crash_classification});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point +
           "_active_root",
       DirectBulkActiveRootName(recovery.active_root)});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point +
           "_half_root_exposed",
       recovery.half_root_exposed ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point +
           "_repair_classification",
       recovery.repair_classification});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_recovery_authority",
       recovery.transaction_finality_authority ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_recovery_benchmark_clean",
       recovery.benchmark_clean ? "true" : "false"});
  for (const auto& item : recovery.evidence) {
    evidence->push_back(
        {"sorted_bulk_root_recovery." + crash_point + "." +
             item.evidence_kind,
         item.evidence_id});
  }
}

bool RequireRootRecovery(
    const scratchbird::core::index::IndexBulkPublishRecoveryResult& recovery,
    scratchbird::core::index::IndexBulkPublishActiveRoot expected_root,
    std::string* failure_reason,
    EngineApiDiagnostic* diagnostic) {
  if (!recovery.ok() ||
      recovery.active_root != expected_root ||
      recovery.half_root_exposed) {
    *failure_reason = "sorted_bulk_index_root_recovery_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        recovery.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.ROOT_RECOVERY_REFUSED"
            : recovery.diagnostic.diagnostic_code,
        recovery.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.root_recovery_refused"
            : recovery.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }
  return true;
}

bool ProveDirectSortedRootPublishRecovery(
    const scratchbird::core::index::SortedBulkIndexBuildResult& built,
    const TypedUuid& table_uuid,
    scratchbird::core::index::IndexFamily family,
    const std::string& semantic_profile,
    std::vector<EngineEvidenceReference>* evidence,
    EngineApiDiagnostic* diagnostic,
    std::string* failure_reason) {
  namespace idx = scratchbird::core::index;

  idx::SortedBulkIndexBuildRequest old_request;
  old_request.metadata.index_uuid =
      built.candidate_root_generation.tree.index_uuid;
  old_request.metadata.table_uuid = table_uuid;
  old_request.metadata.family = family;
  old_request.metadata.family_name =
      idx::IndexFamilyName(family);
  old_request.metadata.semantic_profile = semantic_profile;
  old_request.metadata.leaf_entry_capacity = 128;
  const std::string old_row_uuid =
      built.entries.empty() ? std::string{} : built.entries.front().row_uuid;
  const std::string old_version_uuid =
      built.entries.empty() ? std::string{} : built.entries.front().version_uuid;
  old_request.rows.push_back({"__orh211_old_root__",
                              old_row_uuid,
                              old_version_uuid,
                              "__orh211_old_root_payload__",
                              0,
                              false});
  const auto old_built = idx::BuildSortedExactBulkIndex(old_request);
  if (!old_built.ok()) {
    *failure_reason = "sorted_bulk_index_old_root_fixture_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        old_built.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.OLD_ROOT_BUILD_REFUSED"
            : old_built.diagnostic.diagnostic_code,
        old_built.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.old_root_build_refused"
            : old_built.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }
  const auto old_image =
      scratchbird::storage::page::ExportIndexBtreePhysicalTreeImage(
          old_built.candidate_root_generation.tree);
  if (!old_image.ok()) {
    *failure_reason = "sorted_bulk_index_old_root_export_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        old_image.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.OLD_ROOT_EXPORT_REFUSED"
            : old_image.diagnostic.diagnostic_code,
        old_image.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.old_root_export_refused"
            : old_image.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }

  idx::IndexMetapageControl current;
  current.index_uuid = old_built.candidate_root_generation.tree.index_uuid;
  current.family = family;
  current.root_page_number =
      old_built.candidate_root_generation.root_page_number;
  current.resource_epoch = 1;
  current.mutation_epoch = 1;
  current.root_generation = 0;
  current.page_count = old_built.candidate_root_generation.page_count;
  current.tuple_count_estimate =
      old_built.candidate_root_generation.live_entry_count;
  current.semantic_profile_id = semantic_profile;

  idx::IndexRootGenerationPublishRequest publish_request;
  publish_request.current_metapage = current;
  publish_request.candidate = built.candidate_root_generation;
  publish_request.candidate_tree_validation_proof = true;
  publish_request.durable_metadata_write_evidence = true;
  publish_request.mga_finality_authority_evidence = true;
  publish_request.durable_metadata_evidence_token =
      "direct_physical_bulk.sorted_root_publish_metapage";
  publish_request.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  publish_request.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";

  const auto publish =
      idx::PublishIndexRootGeneration(publish_request);
  AddRootPublishEvidence(publish, evidence);
  if (!publish.ok()) {
    *failure_reason = "sorted_bulk_index_root_publish_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        publish.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.ROOT_PUBLISH_REFUSED"
            : publish.diagnostic.diagnostic_code,
        publish.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.root_publish_refused"
            : publish.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }

  idx::IndexBulkPublishRecoveryState recovery_state;
  recovery_state.old_metapage_present = true;
  recovery_state.old_metapage = publish.old_metapage;
  recovery_state.old_tree_image_present = true;
  recovery_state.old_tree_image = old_image.image;
  recovery_state.candidate_generation = built.candidate_root_generation;
  recovery_state.candidate_tree_image = publish.published_tree_image;
  recovery_state.candidate_tree_validation_proof = true;
  recovery_state.durable_metadata_write_evidence = true;
  recovery_state.root_publish_authorization_proof = true;
  recovery_state.mga_finality_authority_evidence = true;
  recovery_state.durable_metadata_evidence_token =
      "direct_physical_bulk.sorted_root_publish_metapage";
  recovery_state.root_publish_authorization_token =
      "direct_physical_bulk.root_publish_succeeded";
  recovery_state.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  recovery_state.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";
  recovery_state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_before_root_publish;
  recovery_state.durable_metapage_image.reset();
  const auto before_recovery =
      idx::RecoverSortedBulkRootPublish(recovery_state);
  AddRootRecoveryEvidence("before_root_publish", before_recovery, evidence);
  if (!RequireRootRecovery(before_recovery,
                           idx::IndexBulkPublishActiveRoot::old_root,
                           failure_reason,
                           diagnostic)) {
    return false;
  }

  recovery_state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_during_root_publish;
  recovery_state.durable_metapage_image.reset();
  const auto during_recovery =
      idx::RecoverSortedBulkRootPublish(recovery_state);
  AddRootRecoveryEvidence("during_root_publish", during_recovery, evidence);
  if (!RequireRootRecovery(during_recovery,
                           idx::IndexBulkPublishActiveRoot::old_root,
                           failure_reason,
                           diagnostic)) {
    return false;
  }

  recovery_state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_after_root_publish;
  recovery_state.durable_metapage_image = publish.published_metapage_image;
  const auto after_recovery =
      idx::RecoverSortedBulkRootPublish(recovery_state);
  AddRootRecoveryEvidence("after_root_publish", after_recovery, evidence);
  if (!RequireRootRecovery(after_recovery,
                           idx::IndexBulkPublishActiveRoot::new_root,
                           failure_reason,
                           diagnostic)) {
    return false;
  }
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_sorted_root_recovery_proven",
       "true"});
  return true;
}

struct DirectSortedBulkIndexBuildSelection {
  bool selected = false;
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<CrudIndexRecord> retail_indexes;
  std::vector<MgaExactIndexEntryAppendBatch> exact_batches;
  std::vector<EngineEvidenceReference> evidence;
};

DirectSortedBulkIndexBuildSelection BuildDirectSortedBulkIndexArtifacts(
    const DirectPhysicalBulkAppendRequest& request,
    const CrudState& state,
    const std::vector<CrudIndexRecord>& synchronous_indexes,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch) {
  DirectSortedBulkIndexBuildSelection selection;
  if (!DirectSortedBulkIndexBuildEnabled(request)) {
    if (DirectDeferredIndexBenchmarkCleanRequired(request)) {
      selection.selected = false;
      selection.ok = false;
      selection.failure_reason = "deferred_index_bulk_publish_route_not_selected";
      selection.diagnostic = DeferredIndexRouteNotSelectedDiagnostic();
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_selected", "false"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_benchmark_clean", "blocked"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_caller_proof_authority", "false"});
      if (DirectDeferredIndexCallerProofFlagPresent(request)) {
        selection.evidence.push_back(
            {"orh_deferred_index_bulk_publish_caller_proof_flags_ignored",
             "true"});
      }
      return selection;
    }
    selection.retail_indexes = synchronous_indexes;
    return selection;
  }
  selection.selected = true;
  selection.evidence.push_back(
      {"orh_deferred_index_bulk_publish_selected", "true"});
  selection.evidence.push_back(
      {"orh_deferred_index_bulk_publish_caller_proof_authority", "false"});
  selection.evidence.push_back(
      {"orh_deferred_index_bulk_publish_index_metadata_finality_authority",
       "false"});
  if (DirectDeferredIndexCallerProofFlagPresent(request)) {
    selection.evidence.push_back(
        {"orh_deferred_index_bulk_publish_caller_proof_flags_ignored",
         "true"});
  }

  const TypedUuid table_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  if (!table_uuid.valid()) {
    selection.ok = false;
    selection.failure_reason = "sorted_bulk_index_table_uuid_invalid";
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  for (const auto& index : synchronous_indexes) {
    const auto family = DirectCoreIndexFamily(index);
    const std::string family_name =
        index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
    if (family == scratchbird::core::index::IndexFamily::unknown) {
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_family_blocked",
           family_name + "=not_ordered_write_family"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_benchmark_clean", "blocked"});
      if (DirectDeferredIndexBenchmarkCleanRequired(request)) {
        selection.ok = false;
        selection.failure_reason =
            "deferred_index_bulk_publish_family_route_unsupported";
        selection.diagnostic =
            DeferredIndexFamilyUnsupportedDiagnostic(family_name);
        return selection;
      }
      selection.retail_indexes.push_back(index);
      continue;
    }
    const TypedUuid index_uuid =
        ParseDirectTypedUuid(UuidKind::object, index.index_uuid);
    if (!index_uuid.valid()) {
      selection.ok = false;
      selection.failure_reason = "sorted_bulk_index_uuid_invalid";
      selection.diagnostic = MakeInvalidRequestDiagnostic(
          "dml.direct_physical_bulk_append",
          selection.failure_reason);
      return selection;
    }
    scratchbird::core::index::SortedBulkIndexBuildRequest build;
    build.metadata.index_uuid = index_uuid;
    build.metadata.table_uuid = table_uuid;
    build.metadata.family = family;
    build.metadata.family_name =
        family_name;
    build.metadata.semantic_profile = NormalizeCrudIndexProfile(index.profile);
    build.metadata.unique = DirectIndexIsUnique(index);
    build.metadata.rebuild = false;
    build.metadata.input_presorted = false;
    build.metadata.order_proof_valid = false;
    build.metadata.policy_allows_mutation = true;
    build.metadata.leaf_entry_capacity = 128;
    scratchbird::core::index::UniqueIndexReservationLedger unique_ledger;
    for (std::size_t row_index = 0; row_index < staged_rows.size(); ++row_index) {
      for (const auto& key : CrudIndexKeysForValues(index, logical_value_batch[row_index])) {
        scratchbird::core::index::SortedBulkIndexRowInput input;
        input.encoded_key = key;
        input.row_uuid = staged_rows[row_index].row_uuid;
        input.version_uuid = staged_rows[row_index].version_uuid;
        input.payload_value = CrudFieldValue(logical_value_batch[row_index], index.column_name);
        input.source_ordinal = static_cast<std::uint64_t>(row_index);
        input.null_key = DirectNullValue(input.encoded_key) ||
                         input.encoded_key.find("<NULL>") != std::string::npos;
        build.rows.push_back(std::move(input));
      }
    }
    if (build.metadata.unique) {
      AddVisibleRowKeysForSortedBuild(state,
                                      request.context,
                                      index,
                                      &build.visible_unique_keys);
      build.unique_reservation_ledger = &unique_ledger;
      build.validate_unique_reservation_batch = true;
      build.unique_constraint_uuid = index_uuid;
      build.transaction_uuid =
          ParseDirectTypedUuid(UuidKind::transaction,
                               request.context.transaction_uuid.canonical);
      build.local_transaction_id = request.context.local_transaction_id;
      build.unique_reservation_validation_evidence_token =
          "direct_sorted_bulk_unique_reservation_validation";
      scratchbird::core::index::UniqueIndexReservationTransactionProof proof;
      proof.transaction_uuid = build.transaction_uuid;
      proof.local_transaction_id = build.local_transaction_id;
      proof.state = scratchbird::transaction::mga::TransactionState::active;
      proof.engine_mga_authority = true;
      proof.durable_transaction_inventory_authoritative = true;
      proof.evidence_token =
          "direct_sorted_bulk_engine_transaction_inventory_active";
      build.unique_transaction_state_proofs.push_back(std::move(proof));
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_unique_deferred_gated",
           "reservation_ledger_validated"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_unique_constraint_uuid",
           index.index_uuid});
    }

    const auto built = scratchbird::core::index::BuildSortedExactBulkIndex(build);
    if (!built.ok()) {
      selection.ok = false;
      selection.failure_reason = built.uniqueness_refused
                                     ? "sorted_bulk_index_unique_duplicate"
                                     : "sorted_bulk_index_build_refused";
      selection.diagnostic = MakeEngineApiDiagnostic(
          built.diagnostic.diagnostic_code.empty()
              ? (build.metadata.unique
                     ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.UNIQUE_RESERVATION_PROOF_REQUIRED"
                     : "SB_ENGINE_API_INVALID_REQUEST")
              : built.diagnostic.diagnostic_code,
          built.diagnostic.message_key.empty()
              ? (build.metadata.unique
                     ? "orh.deferred_index_bulk_publish.unique_reservation_proof_required"
                     : "engine.api.invalid_request")
              : built.diagnostic.message_key,
          selection.failure_reason,
          true);
      return selection;
    }
    if (build.metadata.unique &&
        (!built.unique_reservation_ledger_used ||
         !built.unique_reservation_validation_passed)) {
      selection.ok = false;
      selection.failure_reason =
          "deferred_index_bulk_publish_unique_reservation_proof_required";
      selection.diagnostic = DeferredIndexUniqueReservationProofRequiredDiagnostic();
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_unique_deferred_gated",
           "reservation_ledger_required"});
      return selection;
    }
    if (!ProveDirectSortedRootPublishRecovery(built,
                                              table_uuid,
                                              family,
                                              build.metadata.semantic_profile,
                                              &selection.evidence,
                                              &selection.diagnostic,
                                              &selection.failure_reason)) {
      selection.ok = false;
      return selection;
    }

    MgaExactIndexEntryAppendBatch batch;
    batch.index = index;
    batch.table_uuid = request.target_table.uuid.canonical;
    batch.entries.reserve(built.entries.size());
    for (const auto& entry : built.entries) {
      batch.entries.push_back({entry.encoded_key,
                               entry.payload_value,
                               entry.row_uuid,
                               entry.version_uuid});
    }
    selection.exact_batches.push_back(std::move(batch));
    for (const auto& item : built.evidence) {
      selection.evidence.push_back({item.evidence_kind, item.evidence_id});
    }
    selection.evidence.push_back({"sorted_bulk_index_uuid", index.index_uuid});
    selection.evidence.push_back({"sorted_bulk_index_build_selected", "true"});
    if (build.metadata.unique && built.uniqueness_proven) {
      selection.evidence.push_back({"sorted_bulk_index_uniqueness_proof",
                                    "sorted_duplicate_runs_absent"});
    }
  }

  selection.evidence.push_back({"sorted_bulk_index_build_route",
                                "direct_physical_bulk"});
  selection.evidence.push_back({"sorted_bulk_index_exact_append_path",
                                "mga_index_append_path"});
  if (!selection.exact_batches.empty()) {
    selection.evidence.push_back(
        {"orh_deferred_index_bulk_publish_consumed_provider",
         "core.index.sorted_bulk_index_build"});
  }
  return selection;
}

std::uint64_t DirectOptionU64(const DirectPhysicalBulkAppendRequest& request,
                              const std::string& key,
                              std::uint64_t fallback) {
  const std::string value = DirectOptionValue(request, key);
  if (value.empty()) {
    return fallback;
  }
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return fallback;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return fallback;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed == 0 ? fallback : parsed;
}

std::uint64_t EstimateDirectPhysicalRowBytes(
    const CrudRowVersionRecord& row) {
  std::uint64_t bytes = 112;  // row header plus slot directory entry.
  for (const auto& field : row.values) {
    bytes += 16;  // cell header.
    bytes += static_cast<std::uint64_t>(field.second.size());
  }
  return std::max<std::uint64_t>(128, bytes);
}

std::uint64_t DefaultDirectPhysicalRowsPerPage(
    const DirectPhysicalBulkAppendRequest& request,
    const std::vector<CrudRowVersionRecord>& staged_rows) {
  if (staged_rows.empty()) {
    return 16;
  }
  const std::uint64_t page_size =
      DirectOptionU64(request, "physical_mga_cow.page_size_bytes", 8192);
  const std::uint64_t usable_bytes =
      page_size > 512 ? page_size - 512 : std::max<std::uint64_t>(1, page_size / 2);
  const std::size_t sample_count =
      std::min<std::size_t>(staged_rows.size(), 64);
  std::uint64_t sampled_bytes = 0;
  for (std::size_t index = 0; index < sample_count; ++index) {
    sampled_bytes += EstimateDirectPhysicalRowBytes(staged_rows[index]);
  }
  const std::uint64_t average_row_bytes =
      std::max<std::uint64_t>(128, sampled_bytes / sample_count);
  const std::uint64_t target_bytes = (usable_bytes * 80) / 100;
  const std::uint64_t rows =
      std::max<std::uint64_t>(1, target_bytes / average_row_bytes);
  return std::clamp<std::uint64_t>(rows, 4, 256);
}

bool DirectBulkInputMatchesEncoderOrder(
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan) {
  if (row_encoder_plan.columns.empty()) {
    return true;
  }
  if (input_row.fields.size() != row_encoder_plan.columns.size()) {
    return false;
  }
  for (std::size_t index = 0; index < input_row.fields.size(); ++index) {
    if (input_row.fields[index].first !=
        row_encoder_plan.columns[index].column_name) {
      return false;
    }
  }
  return true;
}

PreparedInsertRow PrepareDirectBulkOrderedRowFast(
    const EngineRowValue& input_row,
    const BoundInsertRowTemplate& row_template,
    const std::string& row_uuid,
    bool force_large_values) {
  PreparedInsertRow row;
  row.row_uuid = row_uuid;
  row.values.reserve(input_row.fields.size());
  for (const auto& [field, typed] : input_row.fields) {
    if (typed.is_null) {
      row.values.push_back({field, kDirectNullMarker});
      row.encoded_bytes += field.size() + sizeof(kDirectNullMarker) - 1;
    } else {
      row.values.push_back({field, typed.encoded_value});
      row.encoded_bytes += field.size() + typed.encoded_value.size();
    }
  }
  row.toast_required = row.encoded_bytes > row_template.max_inline_encoded_bytes ||
                       force_large_values;
  return row;
}

std::string DirectNotNullValidationFailure(
    const InsertRowEncoderPlan& row_encoder_plan,
    const std::vector<std::pair<std::string, std::string>>& values) {
  for (const auto& column : row_encoder_plan.columns) {
    if (!column.not_null_bound) {
      continue;
    }
    const auto found = std::find_if(values.begin(), values.end(), [&](const auto& value) {
      return value.first == column.column_name;
    });
    if (found == values.end() || DirectNullValue(found->second)) {
      return column.column_name;
    }
  }
  return {};
}

std::vector<std::size_t> DirectNotNullValidationOrdinals(
    const InsertRowEncoderPlan& row_encoder_plan) {
  std::vector<std::size_t> ordinals;
  ordinals.reserve(static_cast<std::size_t>(
      row_encoder_plan.not_null_validator_count));
  for (const auto& column : row_encoder_plan.columns) {
    if (column.not_null_bound) {
      ordinals.push_back(column.ordinal);
    }
  }
  return ordinals;
}

std::string DirectNotNullValidationFailureOrdered(
    const InsertRowEncoderPlan& row_encoder_plan,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::vector<std::size_t>& ordinals) {
  for (const std::size_t ordinal : ordinals) {
    if (ordinal >= values.size() || DirectNullValue(values[ordinal].second)) {
      if (ordinal < row_encoder_plan.columns.size()) {
        return row_encoder_plan.columns[ordinal].column_name;
      }
      return "ordinal:" + std::to_string(ordinal);
    }
  }
  return {};
}

bool DirectPhysicalMgaCowRequested(const DirectPhysicalBulkAppendRequest& request) {
  const std::string value = DirectOptionValue(request, "physical_mga_cow");
  if (value.empty()) {
    return true;
  }
  return !IsDirectFalsyValue(value);
}

bool DirectPhysicalMgaCowRequired(const DirectPhysicalBulkAppendRequest& request) {
  const std::string value = DirectOptionValue(request, "physical_mga_cow");
  return LowerAscii(value) == "required" ||
         IsDirectTruthyValue(DirectOptionValue(request, "physical_mga_cow.required"));
}

scratchbird::storage::page::RowDataCell DirectPhysicalCell(
    std::uint16_t ordinal,
    const EngineTypedValue& value) {
  scratchbird::storage::page::RowDataCell cell;
  cell.column_ordinal = ordinal;
  cell.value.type_id = scratchbird::core::datatypes::CanonicalTypeId::character;
  cell.value.is_null = value.state == EngineValueState::sql_null || value.is_null;
  cell.value.payload.assign(value.encoded_value.begin(), value.encoded_value.end());
  return cell;
}

std::vector<scratchbird::storage::page::RowDataCell> DirectPhysicalCells(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<scratchbird::storage::page::RowDataCell> cells;
  cells.reserve(values.size());
  std::uint16_t ordinal = 1;
  for (const auto& value : values) {
    EngineTypedValue typed;
    typed.encoded_value = value.second;
    typed.state = EngineValueState::value;
    cells.push_back(DirectPhysicalCell(ordinal++, typed));
  }
  return cells;
}

struct DirectPhysicalMgaCowWriteResult {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
  std::uint64_t written_rows = 0;
};

DirectPhysicalMgaCowWriteResult WriteDirectPhysicalMgaCowRows(
    const DirectPhysicalBulkAppendRequest& request,
    const std::vector<CrudRowVersionRecord>& staged_rows) {
  DirectPhysicalMgaCowWriteResult result;
  if (!DirectPhysicalMgaCowRequested(request)) {
    result.evidence.push_back({"direct_physical_bulk_row_page_writer", "disabled"});
    return result;
  }

  const TypedUuid relation_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  const TypedUuid transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction,
                           request.context.transaction_uuid.canonical);
  if (!relation_uuid.valid() || !transaction_uuid.valid() ||
      request.context.local_transaction_id == 0) {
    result.ok = false;
    result.diagnostic =
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "physical_mga_cow_authority_missing");
    return result;
  }

  const std::uint64_t base_page =
      DirectOptionU64(request, "physical_mga_cow.page_number", 1024);
  const std::uint64_t requested_rows_per_page =
      DirectOptionU64(request, "physical_mga_cow.rows_per_page", 0);
  const std::uint64_t rows_per_page =
      requested_rows_per_page == 0
          ? DefaultDirectPhysicalRowsPerPage(request, staged_rows)
          : requested_rows_per_page;
  const std::uint64_t row_offset =
      DirectOptionU64(request, "physical_mga_cow.row_offset", 0);
  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    const auto& row = staged_rows[index];
    const std::uint64_t absolute_index =
        row_offset + static_cast<std::uint64_t>(index);
    scratchbird::storage::database::PhysicalMgaCowMutationRequest cow;
    cow.database_path = request.context.database_path;
    cow.relation_uuid = relation_uuid;
    cow.row_uuid = ParseDirectTypedUuid(UuidKind::row, row.row_uuid);
    cow.transaction_uuid = transaction_uuid;
    cow.existing_local_transaction_id =
        scratchbird::transaction::mga::MakeLocalTransactionId(
            request.context.local_transaction_id);
    cow.use_existing_transaction = true;
    cow.kind = scratchbird::storage::database::PhysicalMgaCowMutationKind::insert;
    cow.page_number =
        base_page + (absolute_index / std::max<std::uint64_t>(1, rows_per_page));
    cow.begin_unix_epoch_millis = 0;
    cow.stable_slot_id = static_cast<std::uint32_t>(absolute_index + 1);
    cow.cells = DirectPhysicalCells(row.values);
    const auto written =
        scratchbird::storage::database::WritePhysicalMgaCowUnpublishedMutation(cow);
    if (!written.ok()) {
      result.ok = false;
      result.diagnostic = MakeEngineApiDiagnostic(
          written.diagnostic.diagnostic_code.empty()
              ? "SB-IPAR-PHYSICAL-MGA-COW-WRITE-FAILED"
              : written.diagnostic.diagnostic_code,
          written.diagnostic.message_key.empty()
              ? "dml.direct_physical_bulk.physical_mga_cow_failed"
              : written.diagnostic.message_key,
          "row=" + row.row_uuid,
          true);
      return result;
    }
    ++result.written_rows;
    for (const auto& item : written.evidence) {
      result.evidence.push_back({"direct_physical_bulk_row_page_evidence", item});
    }
  }
  result.evidence.push_back({"direct_physical_bulk_row_page_writer",
                             "physical_mga_cow"});
  result.evidence.push_back({"direct_physical_bulk_row_page_written_rows",
                             std::to_string(result.written_rows)});
  result.evidence.push_back({"direct_physical_bulk_row_page_finality_authority",
                             "false"});
  result.evidence.push_back({"direct_physical_bulk_row_page_visibility_authority",
                             "durable_transaction_inventory"});
  return result;
}

bool DirectOrderedIngestRequested(const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "ordered_ingest");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf = DirectOptionValue(request, "odf047.ordered_ingest");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

bool DirectOrderedIngestDeriveForLargeLoad(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary =
      DirectOptionValue(request, "ordered_ingest.derive_for_large_load");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf =
      DirectOptionValue(request, "odf047.derive_order_for_large_load");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

std::string DirectOrderedPlacementKeyColumn(
    const DirectPhysicalBulkAppendRequest& request) {
  std::string key = DirectOptionValue(request, "ordered_ingest.placement_key");
  if (!key.empty()) {
    return key;
  }
  key = DirectOptionValue(request, "placement_key");
  if (!key.empty()) {
    return key;
  }
  return DirectOptionValue(request, "odf047.placement_key");
}

bool DirectPhysicalClusteringRequested(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "physical_clustering");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string enabled =
      DirectOptionValue(request, "physical_clustering.enabled");
  if (!enabled.empty()) {
    return IsDirectTruthyValue(enabled);
  }
  return false;
}

std::string DirectPhysicalClusteringKeyColumn(
    const DirectPhysicalBulkAppendRequest& request,
    const std::string& placement_key_column) {
  std::string key = DirectOptionValue(request, "physical_clustering.key");
  if (!key.empty()) {
    return key;
  }
  key = DirectOptionValue(request, "physical_clustering.placement_key");
  if (!key.empty()) {
    return key;
  }
  return placement_key_column;
}

std::string DirectValueForColumn(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& column_name) {
  for (const auto& [name, value] : values) {
    if (name == column_name) {
      return value;
    }
  }
  return {};
}

template <typename T>
std::vector<T> ApplySourceOrdinalPermutation(
    const std::vector<T>& source,
    const std::vector<std::uint64_t>& ordinals) {
  std::vector<T> reordered;
  reordered.reserve(source.size());
  for (const auto ordinal : ordinals) {
    if (ordinal < source.size()) {
      reordered.push_back(source[static_cast<std::size_t>(ordinal)]);
    }
  }
  return reordered;
}

struct DirectOrderedIngestSelection {
  bool ok = true;
  bool selected = false;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<EngineEvidenceReference> evidence;
};

DirectOrderedIngestSelection ApplyDirectOrderedIngestPlan(
    const DirectPhysicalBulkAppendRequest& request,
    std::vector<CrudRowVersionRecord>* staged_rows,
    std::vector<std::vector<std::pair<std::string, std::string>>>* logical_value_batch) {
  DirectOrderedIngestSelection selection;
  if (staged_rows == nullptr || logical_value_batch == nullptr ||
      staged_rows->size() != logical_value_batch->size()) {
    selection.ok = false;
    selection.failure_reason = "ordered_ingest_batch_shape_invalid";
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  const std::string placement_key_column =
      DirectOrderedPlacementKeyColumn(request);
  scratchbird::engine::optimizer::BulkPlacementOrderRequest plan_request;
  plan_request.ordered_ingest_requested = DirectOrderedIngestRequested(request);
  plan_request.derive_for_large_load =
      DirectOrderedIngestDeriveForLargeLoad(request);
  plan_request.large_load_row_threshold =
      DirectOptionU64(request, "ordered_ingest.large_load_threshold", 1024);
  plan_request.placement_key_column = placement_key_column;
  plan_request.rows.reserve(staged_rows->size());
  for (std::size_t index = 0; index < staged_rows->size(); ++index) {
    scratchbird::engine::optimizer::BulkPlacementOrderRow row;
    row.source_ordinal = static_cast<std::uint64_t>(index);
    row.row_uuid = (*staged_rows)[index].row_uuid;
    row.placement_key =
        DirectValueForColumn((*logical_value_batch)[index], placement_key_column);
    plan_request.rows.push_back(std::move(row));
  }

  const auto plan =
      scratchbird::engine::optimizer::PlanBulkPlacementOrder(plan_request);
  for (const auto& item : plan.evidence) {
    selection.evidence.push_back({item.first, item.second});
  }
  if (!plan.ok) {
    selection.ok = false;
    selection.failure_reason = plan.diagnostic_code.empty()
                                   ? "ordered_ingest_refused"
                                   : plan.diagnostic_code;
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  scratchbird::storage::page::OrderedIngestPhysicalClusteringRequest clustering;
  clustering.current_descriptor.relation_uuid =
      request.target_table.uuid.canonical;
  clustering.current_descriptor.placement_key_column =
      DirectOptionValue(request, "physical_clustering.current_key");
  clustering.current_descriptor.policy_uuid =
      DirectOptionValue(request, "physical_clustering.current_policy_uuid");
  clustering.current_descriptor.descriptor_generation =
      DirectOptionU64(request, "physical_clustering.current_generation", 0);
  clustering.current_descriptor.physical_clustering_enabled =
      !clustering.current_descriptor.placement_key_column.empty();
  clustering.requested_placement_key_column =
      DirectPhysicalClusteringKeyColumn(request, placement_key_column);
  clustering.requested_policy_uuid =
      DirectOptionValue(request, "physical_clustering.policy_uuid");
  clustering.ordered_ingest_selected = plan.ordered_ingest_selected;
  clustering.physical_clustering_requested =
      DirectPhysicalClusteringRequested(request);
  clustering.explicit_policy_present =
      DirectOptionEnabled(request, "physical_clustering.policy=explicit") ||
      !clustering.requested_policy_uuid.empty();
  clustering.allow_clustering_key_change =
      IsDirectTruthyValue(DirectOptionValue(request,
                                            "physical_clustering.allow_key_change"));
  const auto clustering_result =
      scratchbird::storage::page::ResolveOrderedIngestPhysicalClustering(
          clustering);
  for (const auto& item : clustering_result.evidence) {
    selection.evidence.push_back({item.first, item.second});
  }
  if (!clustering_result.ok) {
    selection.ok = false;
    selection.failure_reason = clustering_result.diagnostic_detail.empty()
                                   ? "physical_clustering_policy_refused"
                                   : clustering_result.diagnostic_detail;
    selection.diagnostic = MakeEngineApiDiagnostic(
        clustering_result.diagnostic_code.empty()
            ? "SB_ENGINE_API_INVALID_REQUEST"
            : clustering_result.diagnostic_code,
        "storage.ordered_ingest.physical_clustering_refused",
        selection.failure_reason,
        true);
    return selection;
  }

  selection.selected = plan.ordered_ingest_selected;
  if (plan.ordered_ingest_selected &&
      plan.source_ordinals_in_apply_order.size() == staged_rows->size()) {
    *staged_rows = ApplySourceOrdinalPermutation(
        *staged_rows,
        plan.source_ordinals_in_apply_order);
    *logical_value_batch = ApplySourceOrdinalPermutation(
        *logical_value_batch,
        plan.source_ordinals_in_apply_order);
    selection.evidence.push_back({"ordered_ingest_apply_order",
                                  "placement_key"});
    selection.evidence.push_back({"ordered_ingest_applied_rows",
                                  std::to_string(staged_rows->size())});
  }
  return selection;
}

EngineInsertRowsRequest SyntheticInsertRequestForDirectBulk(
    const DirectPhysicalBulkAppendRequest& request) {
  EngineInsertRowsRequest insert;
  insert.context = request.context;
  insert.operation_id = "dml.direct_physical_bulk_append";
  insert.target_table = request.target_table;
  insert.borrowed_input_rows = request.borrowed_input_rows;
  insert.require_generated_row_uuid = request.require_generated_row_uuid;
  insert.estimated_row_count = request.estimated_row_count == 0
                                   ? static_cast<EngineApiU64>(
                                         request.borrowed_input_rows.size())
                                   : request.estimated_row_count;
  insert.insert_mode = request.lane_operation == "native_bulk" ? "native_bulk" : "copy_import";
  insert.duplicate_mode = request.duplicate_mode;
  insert.strict_bulk_load_requested = request.strict_bulk_load_requested;
  insert.option_envelopes = request.option_envelopes;
  insert.diagnostic_options = request.diagnostic_options;
  return insert;
}

void AddDirectLaneBaseEvidence(const DirectPhysicalBulkAppendRequest& request,
                               DirectPhysicalBulkAppendResult* result) {
  result->evidence.push_back({"direct_physical_bulk_lane", "direct_physical"});
  result->evidence.push_back({"direct_physical_bulk_operation", request.lane_operation});
  result->evidence.push_back({"direct_physical_bulk_delegate", "none"});
  result->evidence.push_back({"direct_physical_bulk_rows",
                              std::to_string(request.borrowed_input_rows.size())});
  result->evidence.push_back({"direct_physical_bulk_batch_source",
                              "borrowed_binary_typed_rows"});
  result->evidence.push_back({"direct_physical_bulk_batch_consumed_by",
                              "engine.dml.direct_physical_bulk_append"});
  result->evidence.push_back({"parser_finality_authority", "false"});
  result->evidence.push_back({"reference_finality_authority", "false"});
  result->evidence.push_back({"mga_finality_authority", "engine_transaction_inventory"});
}

DirectPhysicalBulkAppendResult DirectBulkFailure(
    const DirectPhysicalBulkAppendRequest& request,
    EngineApiDiagnostic diagnostic,
    std::string reason,
    EngineDmlSummaryCounters summary = {}) {
  const std::string fallback_reason = reason;
  DirectPhysicalBulkAppendResult result;
  result.ok = false;
  result.operation_id = "dml.direct_physical_bulk_append";
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.diagnostics.push_back(std::move(diagnostic));
  result.dml_summary = std::move(summary);
  AddEmbeddedTrustModeEvidence(request.context, &result);
  AddDirectLaneBaseEvidence(request, &result);
  result.evidence.push_back({"direct_physical_bulk_refused", std::move(reason)});
  result.evidence.push_back({"direct_physical_bulk_fail_closed", "true"});
  AddDmlSummaryFallbackReason(&result.dml_summary, fallback_reason);
  AddDmlSummaryEvidence(&result);
  return result;
}

DirectPhysicalBulkAppendResult DirectBulkFailureWithEvidence(
    const DirectPhysicalBulkAppendRequest& request,
    EngineApiDiagnostic diagnostic,
    std::string reason,
    const std::vector<EngineEvidenceReference>& evidence,
    EngineDmlSummaryCounters summary = {}) {
  auto result = DirectBulkFailure(request, std::move(diagnostic), std::move(reason), std::move(summary));
  result.evidence.insert(result.evidence.end(), evidence.begin(), evidence.end());
  return result;
}

void AddHotAppendCounterEvidence(const MgaRelationHotAppendCounters& counters,
                                 DirectPhysicalBulkAppendResult* result) {
  result->evidence.push_back({"mga_hot_append_row_stream_opens",
                              std::to_string(counters.row_stream_opens)});
  result->evidence.push_back({"mga_hot_append_row_stream_flushes",
                              std::to_string(counters.row_stream_flushes)});
  result->evidence.push_back({"mga_hot_append_row_range_reservations",
                              std::to_string(counters.row_range_reservations)});
  result->evidence.push_back({"mga_hot_append_row_versions",
                              std::to_string(counters.row_versions_appended)});
  result->evidence.push_back({"mga_hot_append_index_stream_opens",
                              std::to_string(counters.index_stream_opens)});
  result->evidence.push_back({"mga_hot_append_index_stream_flushes",
                              std::to_string(counters.index_stream_flushes)});
  result->evidence.push_back({"mga_hot_append_index_range_reservations",
                              std::to_string(counters.index_range_reservations)});
  result->evidence.push_back({"mga_hot_append_index_entries",
                              std::to_string(counters.index_entries_appended)});
}

bool HasEvidence(const std::vector<EngineEvidenceReference>& evidence,
                 const std::string& kind,
                 const std::string& id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string FirstEvidenceId(const std::vector<EngineEvidenceReference>& evidence,
                            const std::string& kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

std::string RequiredPreallocationFailureReason(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family) {
  if (allocation.preallocation_capped ||
      HasEvidence(allocation.evidence, "dml_demand_hint_decision", "capped")) {
    return family + "_page_extent_preallocation_cap_exceeded";
  }
  if (allocation.preallocation_refused ||
      HasEvidence(allocation.evidence, "dml_demand_runtime_outcome", "capacity_refused") ||
      HasEvidence(allocation.evidence, "dml_demand_runtime_outcome", "capacity_request_refused") ||
      HasEvidence(allocation.evidence, "dml_demand_runtime_outcome", "not_accepted")) {
    return family + "_page_extent_preallocation_refused";
  }
  if (!allocation.active) {
    return family + "_page_extent_preallocation_authority_missing";
  }
  const std::string source_kind =
      family == "index" ? "index_page_allocation_source" : "row_page_allocation_source";
  if (!HasEvidence(allocation.evidence,
                   source_kind,
                   "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT")) {
    return family + "_page_extent_preallocation_not_granted";
  }
  return {};
}

void AddPreallocationRuntimeCounters(const DmlPageAllocationRuntimeResult& allocation,
                                     EngineDmlSummaryCounters* summary) {
  if (summary == nullptr) {
    return;
  }
  summary->preallocation_requests += allocation.preallocation_requested ? 1 : 0;
  summary->preallocation_granted_pages += allocation.granted_preallocation_pages;
  summary->preallocation_capped += allocation.preallocation_capped ? 1 : 0;
  summary->preallocation_refused += allocation.preallocation_refused ? 1 : 0;
}

EngineApiU64 DirectElapsedMicros(DirectSteadyClock::time_point start,
                                 DirectSteadyClock::time_point finish) {
  return static_cast<EngineApiU64>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

std::string DirectPreallocationOutcome(
    const DmlPageAllocationRuntimeResult& allocation) {
  if (!allocation.active) {
    return "runtime_inactive";
  }
  if (!allocation.preallocation_requested) {
    return "reservation_only";
  }
  if (allocation.preallocation_refused) {
    return "preallocation_refused";
  }
  if (allocation.preallocation_capped) {
    return allocation.preallocation_granted ? "preallocation_capped"
                                            : "preallocation_cap_refused";
  }
  if (allocation.preallocation_granted ||
      allocation.granted_preallocation_pages != 0) {
    return "preallocated";
  }
  return "reservation_only";
}

std::string DirectPreallocationFallbackReason(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family) {
  if (!allocation.active) {
    return family + "_page_allocation_runtime_inactive";
  }
  if (!allocation.preallocation_requested) {
    return {};
  }
  if (allocation.preallocation_refused) {
    return family + "_page_preallocation_refused";
  }
  if (allocation.preallocation_capped && !allocation.preallocation_granted) {
    return family + "_page_preallocation_cap_refused";
  }
  if (!allocation.preallocation_granted &&
      allocation.granted_preallocation_pages == 0) {
    return family + "_page_preallocation_reservation_only";
  }
  return {};
}

EngineApiU64 DirectAllocationEvidenceU64(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& kind) {
  for (const auto& item : allocation.evidence) {
    if (item.evidence_kind != kind) {
      continue;
    }
    std::istringstream in(item.evidence_id);
    EngineApiU64 value = 0;
    in >> value;
    return in.fail() ? 0 : value;
  }
  return 0;
}

EngineApiU64 DirectFilespaceGrowthPages(
    const DmlPageAllocationRuntimeResult& allocation) {
  return DirectAllocationEvidenceU64(
      allocation,
      "filespace_runtime_capacity_window_materialized");
}

void AddDirectAllocationResourceSummary(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family,
    EngineApiU64 row_count,
    EngineApiU64 elapsed_microseconds,
    const InsertBatchContext& batch_context,
    bool update_summary,
    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  if (allocation.active && update_summary) {
    if (family == "row") {
      result->dml_summary.row_extent_reservations += row_count;
      result->dml_summary.version_extent_reservations += row_count;
      result->dml_summary.page_extent_reservations += allocation.requested_pages;
    } else if (family == "index") {
      result->dml_summary.index_extent_reservations += allocation.requested_pages;
    }
    AddPreallocationRuntimeCounters(allocation, &result->dml_summary);
  }
  result->evidence.push_back({family + "_page_allocation_runtime",
                              allocation.active ? "active" : "inactive"});
  result->evidence.push_back({family + "_page_reservation_requested_pages",
                              std::to_string(allocation.requested_pages)});
  result->evidence.push_back({family + "_page_preallocation_requested",
                              allocation.preallocation_requested ? "true" : "false"});
  result->evidence.push_back({family + "_page_preallocation_granted_pages",
                              std::to_string(allocation.granted_preallocation_pages)});
  result->evidence.push_back({family + "_page_preallocation_outcome",
                              DirectPreallocationOutcome(allocation)});
  result->evidence.push_back({family + "_page_preallocation_claim",
                              allocation.granted_preallocation_pages != 0
                                  ? "physical_preallocated_pages"
                                  : "reservation_or_no_runtime_only"});
  const std::string fallback =
      DirectPreallocationFallbackReason(allocation, family);
  if (!fallback.empty()) {
    result->evidence.push_back({family + "_page_preallocation_degraded_reason",
                                fallback});
  }
  const EngineApiU64 growth_pages = DirectFilespaceGrowthPages(allocation);
  const EngineApiU64 growth_agent_pages =
      DirectAllocationEvidenceU64(allocation, "filespace_agent_granted_pages");
  result->evidence.push_back({family + "_filespace_growth_pages",
                              std::to_string(growth_pages)});
  if (growth_agent_pages != 0 || growth_pages != 0) {
    result->evidence.push_back({family + "_filespace_growth_agent_granted_pages",
                                std::to_string(growth_agent_pages)});
  }
  result->evidence.push_back({family + "_filespace_growth_claim",
                              growth_pages != 0
                                  ? "capacity_window_materialized"
                                  : (allocation.active ? "not_materialized"
                                                       : "runtime_inactive")});
  result->evidence.push_back({family + "_allocation_stall_microseconds",
                              std::to_string(elapsed_microseconds)});
  if (allocation.active && allocation.granted_preallocation_pages != 0) {
    (void)scratchbird::core::metrics::RecordInsertPreallocatedPages(
        static_cast<double>(allocation.granted_preallocation_pages),
        batch_context.target_object_uuid,
        InsertBatchModeName(batch_context.insert_mode),
        family,
        DirectPreallocationOutcome(allocation),
        fallback.empty() ? "none" : fallback);
  }
  RecordInsertBatchMetric(batch_context,
                          "sb_dml_insert_allocation_stall_microseconds",
                          static_cast<double>(elapsed_microseconds),
                          allocation.active ? "ok" : "inactive",
                          family + "_page_allocation");
  if (growth_pages != 0) {
    RecordInsertBatchMetric(batch_context,
                            "sb_filespace_insert_growth_request_total",
                            static_cast<double>(growth_pages),
                            "capacity_window_materialized",
                            family + "_filespace_growth");
    RecordInsertBatchMetric(batch_context,
                            "sb_filespace_insert_growth_wait_microseconds",
                            static_cast<double>(elapsed_microseconds),
                            "ok",
                            family + "_filespace_growth");
  }
  if (!fallback.empty() && allocation.preallocation_requested) {
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_slow_path_total",
                            1.0,
                            "resource_degraded",
                            fallback);
  }
}

void AddRequiredPreallocationSummary(
    const DmlPageAllocationRuntimeResult& row_allocation,
    const DmlPageAllocationRuntimeResult& index_allocation,
    std::size_t row_count,
    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  result->dml_summary.row_extent_reservations += static_cast<EngineApiU64>(row_count);
  result->dml_summary.version_extent_reservations += static_cast<EngineApiU64>(row_count);
  result->dml_summary.page_extent_reservations += row_allocation.requested_pages;
  result->dml_summary.index_extent_reservations += index_allocation.requested_pages;
  AddPreallocationRuntimeCounters(row_allocation, &result->dml_summary);
  AddPreallocationRuntimeCounters(index_allocation, &result->dml_summary);

  const std::string row_allocation_id =
      FirstEvidenceId(row_allocation.evidence, "row_page_allocation");
  const std::string index_allocation_id =
      FirstEvidenceId(index_allocation.evidence, "index_page_allocation");
  result->evidence.push_back({"row_extent_reservation_count", std::to_string(row_count)});
  result->evidence.push_back({"version_extent_reservation_count", std::to_string(row_count)});
  result->evidence.push_back({"page_extent_reservation_count",
                              std::to_string(row_allocation.requested_pages)});
  result->evidence.push_back({"index_extent_reservation_count",
                              std::to_string(index_allocation.requested_pages)});
  if (!row_allocation_id.empty()) {
    result->evidence.push_back({"row_extent_reservation_id", row_allocation_id});
    result->evidence.push_back({"version_extent_reservation_id",
                                row_allocation_id + ":versions"});
    result->evidence.push_back({"page_extent_reservation_id", row_allocation_id});
    result->evidence.push_back({"dml_summary.row_extent_reservation_id",
                                row_allocation_id});
    result->evidence.push_back({"dml_summary.version_extent_reservation_id",
                                row_allocation_id + ":versions"});
    result->evidence.push_back({"dml_summary.page_extent_reservation_id",
                                row_allocation_id});
  }
  if (!index_allocation_id.empty()) {
    result->evidence.push_back({"index_extent_reservation_id", index_allocation_id});
    result->evidence.push_back({"dml_summary.index_extent_reservation_id",
                                index_allocation_id});
  }
  result->evidence.push_back({"page_extent_preallocation_requested", "true"});
  result->evidence.push_back({"page_extent_preallocation_granted",
                              result->dml_summary.preallocation_granted_pages != 0
                                  ? "true"
                                  : "false"});
  result->evidence.push_back({"page_extent_preallocation_capped",
                              result->dml_summary.preallocation_capped != 0
                                  ? "true"
                                  : "false"});
  result->evidence.push_back({"page_extent_preallocation_refused",
                              result->dml_summary.preallocation_refused != 0
                                  ? "true"
                                  : "false"});
  result->evidence.push_back({"filespace_page_agent_handoff",
                              "filespace_capacity_manager->page_allocation_manager"});
  result->evidence.push_back({"page_extent_reservation_before_physical_append", "true"});
}

struct DirectBulkUuidBatch {
  std::vector<std::string> row_uuids;
  std::vector<std::string> version_uuids;
  std::size_t generated_row_uuids = 0;
  std::size_t caller_row_uuids = 0;
  std::string batch_evidence_id;
};

std::uint64_t DirectBulkUuidSeedBase(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::uint64_t transaction_component =
      request.context.local_transaction_id % 1000000ull;
  const std::uint64_t row_component =
      static_cast<std::uint64_t>(request.borrowed_input_rows.size() % 10000ull);
  return 1850000000000ull + (transaction_component * 100000ull) +
         (row_component * 10ull);
}

std::string GeneratedDirectBulkUuid(UuidKind kind, std::uint64_t seed) {
  const auto generated =
      scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok()
             ? scratchbird::core::uuid::UuidToString(generated.value.value)
             : std::string{};
}

DirectBulkUuidBatch BuildDirectBulkUuidBatch(
    const DirectPhysicalBulkAppendRequest& request) {
  DirectBulkUuidBatch batch;
  batch.row_uuids.reserve(request.borrowed_input_rows.size());
  batch.version_uuids.reserve(request.borrowed_input_rows.size());
  const std::uint64_t seed_base = DirectBulkUuidSeedBase(request);
  batch.batch_evidence_id =
      "direct-bulk-uuid-batch:" + request.context.request_id + ":" +
      std::to_string(request.borrowed_input_rows.size());
  for (std::size_t index = 0; index < request.borrowed_input_rows.size(); ++index) {
    const auto& input_row = request.borrowed_input_rows[index];
    if (input_row.requested_row_uuid.canonical.empty()) {
      std::string row_uuid = GeneratedDirectBulkUuid(
          UuidKind::row,
          seed_base + static_cast<std::uint64_t>(index));
      if (row_uuid.empty()) {
        row_uuid = GenerateCrudEngineUuid("row");
      }
      ++batch.generated_row_uuids;
      batch.row_uuids.push_back(std::move(row_uuid));
    } else {
      ++batch.caller_row_uuids;
      batch.row_uuids.push_back(input_row.requested_row_uuid.canonical);
    }
    std::string version_uuid = GeneratedDirectBulkUuid(
        UuidKind::row,
        seed_base + 50000ull + static_cast<std::uint64_t>(index));
    if (version_uuid.empty()) {
      version_uuid = GenerateCrudEngineUuid("row");
    }
    batch.version_uuids.push_back(std::move(version_uuid));
  }
  return batch;
}

void AddDirectBulkUuidBatchEvidence(const DirectBulkUuidBatch& batch,
                                    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({"direct_bulk_uuid_generation_mode", "batched"});
  result->evidence.push_back({"direct_bulk_uuid_batch", batch.batch_evidence_id});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_row_capacity",
       std::to_string(batch.row_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_version_capacity",
       std::to_string(batch.version_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_generated_row_uuids",
       std::to_string(batch.generated_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_caller_row_uuids",
       std::to_string(batch.caller_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_version_uuid_generation_mode", "batched"});
  result->evidence.push_back(
      {"orh_210_batched_uuid_generation", "row_and_version_batch"});
}

struct DirectStrictBulkLifecycleResult {
  bool active = false;
  bool ok = true;
  bool recovery_required = false;
  TypedUuid bulk_load_id;
  scratchbird::core::bulk_load::StrictBulkLoadLedger ledger;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<EngineEvidenceReference> evidence;
};

std::string EncodedStrictBulkRow(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::ostringstream encoded;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      encoded << ';';
    }
    encoded << values[index].first << '=' << values[index].second;
  }
  return encoded.str().empty() ? "empty-row" : encoded.str();
}

DirectStrictBulkLifecycleResult RunDirectStrictBulkLifecycle(
    const DirectPhysicalBulkAppendRequest& request,
    const InsertBatchContext& batch_context,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch) {
  DirectStrictBulkLifecycleResult result;
  if (!batch_context.strict_bulk_load_selected) {
    return result;
  }
  result.active = true;

  auto fail_before_begin = [&](std::string reason) {
    result.ok = false;
    result.failure_reason = reason;
    result.diagnostic = MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                                     result.failure_reason);
    return result;
  };

  const TypedUuid database_uuid =
      ParseDirectTypedUuid(UuidKind::database, request.context.database_uuid.canonical);
  const TypedUuid object_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  const TypedUuid transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction, request.context.transaction_uuid.canonical);
  if (!database_uuid.valid() || !object_uuid.valid() || !transaction_uuid.valid()) {
    return fail_before_begin("strict_bulk_load_invalid_identity");
  }

  scratchbird::core::bulk_load::StrictBulkLoadLedger ledger;
  scratchbird::core::bulk_load::StrictBulkLoadPolicySnapshot policy;
  policy.policy_uuid = GeneratedId(UuidKind::object,
                                   710000 + request.context.local_transaction_id);
  policy.enabled = !DirectOptionEnabled(request, "strict_bulk_load.simulate_begin_refused=true");
  policy.require_all_constraints_valid = true;
  policy.require_all_indexes_valid = true;
  policy.require_all_domains_valid = true;
  policy.require_all_policy_gates_valid = true;

  scratchbird::core::bulk_load::StrictBulkLoadBeginRequest begin_request;
  begin_request.database_uuid = database_uuid;
  begin_request.object_uuid = object_uuid;
  begin_request.transaction_uuid = transaction_uuid;
  begin_request.local_transaction_id = request.context.local_transaction_id;
  begin_request.policy = policy;
  begin_request.staging_target =
      "direct_physical_bulk:" + request.lane_operation + ":strict_staging";
  const auto begin =
      scratchbird::core::bulk_load::BeginStrictBulkLoad(&ledger, begin_request);
  if (!begin.ok()) {
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    result.ok = false;
    result.failure_reason = "strict_bulk_load_begin_refused";
    result.diagnostic = CoreBulkDiagnosticToEngine(begin.diagnostic,
                                                   result.failure_reason);
    return result;
  }
  result.bulk_load_id = begin.operation.bulk_load_id;

  std::vector<scratchbird::core::bulk_load::StrictBulkLoadRow> strict_rows;
  strict_rows.reserve(staged_rows.size());
  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    scratchbird::core::bulk_load::StrictBulkLoadRow row;
    row.row_uuid = ParseDirectTypedUuid(UuidKind::row, staged_rows[index].row_uuid);
    row.encoded_row = EncodedStrictBulkRow(logical_value_batch[index]);
    row.constraints_valid = true;
    row.indexes_valid = true;
    row.domains_valid = true;
    row.policy_gates_valid = true;
    strict_rows.push_back(std::move(row));
  }
  const auto append = scratchbird::core::bulk_load::AppendStrictBulkLoadRows(
      &ledger,
      scratchbird::core::bulk_load::StrictBulkLoadAppendRequest{
          begin.operation.bulk_load_id,
          transaction_uuid,
          request.context.local_transaction_id,
          std::move(strict_rows)});
  if (!append.ok()) {
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    result.ok = false;
    result.failure_reason = "strict_bulk_load_append_refused";
    result.diagnostic = CoreBulkDiagnosticToEngine(append.diagnostic,
                                                   result.failure_reason);
    return result;
  }

  if (DirectOptionEnabled(request, "strict_bulk_load.simulate_rollback_before_publication=true")) {
    const auto rollback = scratchbird::core::bulk_load::RollbackStrictBulkLoad(
        &ledger,
        scratchbird::core::bulk_load::StrictBulkLoadRollbackRequest{
            begin.operation.bulk_load_id,
            transaction_uuid,
            request.context.local_transaction_id,
            "direct physical strict bulk rollback before publication"});
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(ledger);
    AddStrictBulkRecoveryEvidence(recovery, &result.evidence);
    result.ok = false;
    result.failure_reason = rollback.ok()
                                ? "strict_bulk_load_rollback_requested"
                                : "strict_bulk_load_rollback_refused";
    result.diagnostic = rollback.ok()
                            ? MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                                           result.failure_reason)
                            : CoreBulkDiagnosticToEngine(rollback.diagnostic,
                                                         result.failure_reason);
    return result;
  }

  if (DirectOptionEnabled(request, "strict_bulk_load.simulate_quarantine_before_publication=true")) {
    const auto quarantine = scratchbird::core::bulk_load::QuarantineStrictBulkLoad(
        &ledger,
        scratchbird::core::bulk_load::StrictBulkLoadQuarantineRequest{
            begin.operation.bulk_load_id,
            transaction_uuid,
            request.context.local_transaction_id,
            "direct physical strict bulk quarantine before publication"});
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(ledger);
    AddStrictBulkRecoveryEvidence(recovery, &result.evidence);
    result.ok = false;
    result.failure_reason = quarantine.ok()
                                ? "strict_bulk_load_quarantine_requested"
                                : "strict_bulk_load_quarantine_refused";
    result.diagnostic = quarantine.ok()
                            ? MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                                           result.failure_reason)
                            : CoreBulkDiagnosticToEngine(quarantine.diagnostic,
                                                         result.failure_reason);
    return result;
  }

  const bool simulate_finalize_failure = DirectOptionEnabled(
      request,
      "strict_bulk_load.simulate_finalize_failure_after_evidence=true");
  const auto finalize = scratchbird::core::bulk_load::FinalizeStrictBulkLoadEvidenceDurable(
      &ledger,
      scratchbird::core::bulk_load::StrictBulkLoadFinalizeRequest{
          begin.operation.bulk_load_id,
          transaction_uuid,
          request.context.local_transaction_id,
          simulate_finalize_failure,
          "direct-physical-strict-bulk-visibility-fence"});
  AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
  if (!finalize.ok()) {
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(ledger);
    AddStrictBulkRecoveryEvidence(recovery, &result.evidence);
    result.ok = false;
    result.recovery_required = finalize.recovery_required;
    result.failure_reason = finalize.recovery_required
                                ? "strict_bulk_load_recovery_required"
                                : "strict_bulk_load_finalize_refused";
    result.diagnostic = CoreBulkDiagnosticToEngine(finalize.diagnostic,
                                                   result.failure_reason);
    return result;
  }

  result.evidence.push_back({"strict_bulk_load_direct_publication_fence",
                             "finalize_evidence_durable_before_mga_visibility"});
  result.ledger = std::move(ledger);
  return result;
}

void AddStrictPhysicalPublicationFailureEvidence(
    DirectStrictBulkLifecycleResult* lifecycle,
    const std::string& stage,
    const EngineApiDiagnostic& diagnostic,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr || lifecycle == nullptr || !lifecycle->active) {
    return;
  }
  evidence->push_back({"strict_bulk_load_physical_publication_failed", stage});
  evidence->push_back({"strict_bulk_load_physical_publication_diagnostic",
                       diagnostic.detail});
  const auto transaction_uuid = lifecycle->ledger.operations.empty()
                                    ? TypedUuid{}
                                    : lifecycle->ledger.operations.front().transaction_uuid;
  const auto local_transaction_id = lifecycle->ledger.operations.empty()
                                        ? 0
                                        : lifecycle->ledger.operations.front().local_transaction_id;
  if (lifecycle->bulk_load_id.valid() && transaction_uuid.valid() && local_transaction_id != 0) {
    (void)scratchbird::core::bulk_load::QuarantineStrictBulkLoad(
        &lifecycle->ledger,
        scratchbird::core::bulk_load::StrictBulkLoadQuarantineRequest{
            lifecycle->bulk_load_id,
            transaction_uuid,
            local_transaction_id,
            "direct physical publication failed after finalize evidence: " + stage});
    AddStrictBulkLifecycleEvidence(lifecycle->ledger, evidence);
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(
            lifecycle->ledger);
    AddStrictBulkRecoveryEvidence(recovery, evidence);
  }
}

DirectPhysicalBulkAppendResult DirectStrictPhysicalPublicationFailure(
    const DirectPhysicalBulkAppendRequest& request,
    DirectStrictBulkLifecycleResult* lifecycle,
    const DirectPhysicalBulkAppendResult& partial_result,
    EngineApiDiagnostic diagnostic,
    std::string reason,
    const std::string& stage) {
  auto evidence = partial_result.evidence;
  AddStrictPhysicalPublicationFailureEvidence(lifecycle, stage, diagnostic, &evidence);
  return DirectBulkFailureWithEvidence(request,
                                       std::move(diagnostic),
                                       std::move(reason),
                                       evidence,
                                       partial_result.dml_summary);
}

DirectPhysicalBulkAppendResult PublishDirectStrictBulkAfterPhysicalSuccess(
    const DirectPhysicalBulkAppendRequest& request,
    DirectStrictBulkLifecycleResult* lifecycle,
    DirectPhysicalBulkAppendResult result) {
  if (lifecycle == nullptr || !lifecycle->active) {
    return result;
  }
  result.evidence.push_back({"strict_bulk_load_physical_publication_succeeded",
                             "row_index_append_flush"});
  const auto transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction, request.context.transaction_uuid.canonical);
  const auto published = scratchbird::core::bulk_load::PublishStrictBulkLoadVisible(
      &lifecycle->ledger,
      scratchbird::core::bulk_load::StrictBulkLoadPublishRequest{
          lifecycle->bulk_load_id,
          transaction_uuid,
          request.context.local_transaction_id,
          "direct-physical-strict-bulk-visibility-fence"});
  if (!published.ok()) {
    auto evidence = result.evidence;
    AddStrictBulkLifecycleEvidence(lifecycle->ledger, &evidence);
    return DirectBulkFailureWithEvidence(
        request,
        CoreBulkDiagnosticToEngine(published.diagnostic,
                                   "strict_bulk_load_publish_refused"),
        "strict_bulk_load_publish_refused",
        evidence,
        result.dml_summary);
  }
  AddStrictBulkLifecycleEvidence(lifecycle->ledger, &result.evidence);
  result.evidence.push_back({"strict_bulk_load_direct_lane_published_after",
                             "strict_bulk_load_physical_publication_succeeded"});
  return result;
}

}  // namespace

InsertPhysicalIntegrationResult ExecuteInsertPhysicalIntegration(
    InsertPhysicalIntegrationContext* context,
    const InsertPhysicalIntegrationRequest& request) {
  if (context == nullptr ||
      context->page_reservation_ledger == nullptr ||
      context->page_selection_ledger == nullptr) {
    return Refuse("insert_physical_integration_missing_page_authority",
                  "engine.insert.physical.missing_page_authority",
                  "page reservation and selection ledgers are required");
  }
  if (!request.database_uuid.valid() ||
      !request.object_uuid.valid() ||
      !request.transaction_uuid.valid() ||
      request.local_transaction_id == 0) {
    return Refuse("insert_physical_integration_invalid_identity",
                  "engine.insert.physical.invalid_identity",
                  "database, object, transaction UUIDs and local transaction ID are required");
  }

  InsertPhysicalIntegrationResult result;
  TypedUuid resolved_filespace_uuid = request.filespace_uuid;
  auto resolved_object_class = request.placement_object_class;
  auto resolved_growth_role = request.growth_filespace_role;

  const bool placement_resolution_required =
      request.require_placement_policy ||
      request.placement_policy.present ||
      request.placement_object_class !=
          scratchbird::storage::filespace::FilespaceObjectClass::unspecified;
  if (placement_resolution_required) {
    if (context->filespace_registry == nullptr) {
      return Refuse("insert_physical_integration_missing_placement_registry",
                    "engine.insert.physical.missing_placement_registry",
                    "filespace registry is required for placement policy resolution");
    }
    scratchbird::storage::filespace::FilespacePlacementRequest placement_request;
    placement_request.database_uuid = request.database_uuid;
    placement_request.preferred_filespace_uuid = request.filespace_uuid;
    placement_request.owner_object_uuid = request.object_uuid;
    placement_request.policy_uuid = request.policy_uuid;
    placement_request.object_class = request.placement_object_class;
    placement_request.page_family = request.page_family;
    placement_request.page_size = request.page_size;
    placement_request.require_preallocation =
        request.require_placement_preallocation;
    placement_request.requested_preallocation_pages =
        request.placement_preallocation_pages;
    placement_request.reason = "insert_physical_integration";
    placement_request.policy = request.placement_policy;
    const auto placement = scratchbird::storage::filespace::ResolveFilespacePlacement(
        *context->filespace_registry,
        placement_request);
    if (!placement.ok()) {
      return Refuse(placement.diagnostic.diagnostic_code,
                    placement.diagnostic.message_key,
                    "filespace placement refused");
    }
    resolved_filespace_uuid = placement.descriptor.filespace_uuid;
    resolved_object_class = placement.object_class;
    resolved_growth_role = placement.descriptor.role;
    result.filespace_placement_resolved = true;
    result.resolved_filespace_uuid = resolved_filespace_uuid;
    result.resolved_filespace_class =
        scratchbird::storage::filespace::FilespaceClassName(
            placement.filespace_class);
    result.resolved_filespace_role =
        scratchbird::storage::filespace::FilespaceRoleName(
            placement.descriptor.role);
    result.evidence_refs.push_back(EvidenceRef("filespace_placement",
                                               resolved_filespace_uuid));
    for (const auto& evidence : placement.evidence) {
      result.evidence_refs.push_back("filespace_placement:" + evidence);
    }

    if (placement.preallocation_required) {
      if (context->filespace_growth_ledger == nullptr) {
        return Refuse("insert_physical_integration_missing_preallocation_ledger",
                      "engine.insert.physical.missing_preallocation_ledger",
                      "filespace growth ledger is required for placement preallocation");
      }
      scratchbird::storage::filespace::FilespacePreallocationRequest preallocate;
      preallocate.request_uuid = request.request_id.valid()
                                     ? request.request_id
                                     : GeneratedId(UuidKind::object, 300010);
      preallocate.database_uuid = request.database_uuid;
      preallocate.filespace_uuid = resolved_filespace_uuid;
      preallocate.policy_uuid = request.policy_uuid;
      preallocate.storage_profile_uuid =
          placement.descriptor.writer_identity_uuid.valid()
              ? placement.descriptor.writer_identity_uuid
              : GeneratedId(UuidKind::object, 300011);
      preallocate.requested_page_count = placement.preallocation_page_count;
      preallocate.page_size_bytes = request.page_size;
      preallocate.policy_generation = 1;
      preallocate.observed_policy_generation = 1;
      preallocate.catalog_generation =
          placement.descriptor.generation == 0 ? 1 : placement.descriptor.generation;
      preallocate.observed_catalog_generation = preallocate.catalog_generation;
      preallocate.member_capacity.present = true;
      preallocate.member_capacity.explicit_capacity_context = true;
      preallocate.member_capacity.file_member_uuid =
          placement.descriptor.writer_identity_uuid.valid()
              ? placement.descriptor.writer_identity_uuid
              : GeneratedId(UuidKind::object, 300012);
      preallocate.member_capacity.start_page_number = 0;
      preallocate.member_capacity.current_page_count =
          placement.descriptor.total_pages;
      preallocate.member_capacity.preallocated_page_count =
          placement.descriptor.preallocated_pages;
      preallocate.member_capacity.maximum_page_count =
          placement.descriptor.total_pages +
          placement.descriptor.preallocated_pages +
          placement.preallocation_page_count + 1024;
      preallocate.member_capacity.physical_path = placement.descriptor.path;
      preallocate.member_capacity.online = true;
      preallocate.member_capacity.writable = !placement.descriptor.read_only;
      preallocate.transaction_context.present = true;
      preallocate.transaction_context.transaction_uuid = request.transaction_uuid;
      preallocate.transaction_context.transaction_number =
          request.local_transaction_id;
      preallocate.transaction_context.durable_inventory_admitted = true;
      preallocate.transaction_context.write_intent = true;
      preallocate.transaction_context.durability_fence_satisfied = true;
      preallocate.evidence_store_present = true;
      preallocate.evidence_before_success = true;
      preallocate.require_mga_transaction_context = true;
      preallocate.reason = "insert_physical_integration.placement_preallocation";
      const auto preallocated =
          scratchbird::storage::filespace::PreallocateFilespace(
              context->filespace_growth_ledger,
              *context->filespace_registry,
              preallocate);
      if (!preallocated.ok()) {
        return Refuse(preallocated.diagnostic.diagnostic_code,
                      preallocated.diagnostic.message_key,
                      "filespace placement preallocation refused");
      }
      result.filespace_preallocation_admitted = true;
      result.preallocation_operation_id =
          preallocated.operation.preallocation_operation_id;
      result.evidence_refs.push_back(EvidenceRef("filespace_preallocation",
                                                 result.preallocation_operation_id));
    }
  }

  scratchbird::storage::page::InsertPageReservationRequest reservation_request;
  reservation_request.database_uuid = request.database_uuid;
  reservation_request.transaction_uuid = request.transaction_uuid;
  reservation_request.local_transaction_id = request.local_transaction_id;
  reservation_request.object_uuid = request.object_uuid;
  reservation_request.page_family = request.page_family;
  reservation_request.estimated_row_count = request.estimated_row_count;
  reservation_request.estimated_payload_bytes = request.estimated_payload_bytes;
  reservation_request.preferred_filespace_uuid = resolved_filespace_uuid;
  reservation_request.policy_uuid = request.policy_uuid;
  reservation_request.request_id = request.request_id.valid() ? request.request_id : GeneratedId(UuidKind::object, 300000);
  reservation_request.object_class = resolved_object_class;
  reservation_request.page_size = request.page_size;
  reservation_request.current_time_authority_tick = request.time_authority_tick;
  reservation_request.lease_duration_ticks = request.reservation_lease_ticks;

  const auto reservation = scratchbird::storage::page::ReserveInsertPagesDurable(
      context->page_reservation_ledger,
      reservation_request);
  if (!reservation.ok()) {
    return Refuse(reservation.diagnostic.diagnostic_code,
                  reservation.diagnostic.message_key,
                  "page reservation refused");
  }
  result.page_reserved = true;
  result.reservation_id = reservation.reservation.reservation_id;
  result.evidence_refs.push_back(EvidenceRef("page_reservation", reservation.reservation.reservation_id));

  auto refuse_after_reservation = [&](std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
    (void)scratchbird::storage::page::ReleaseInsertPageReservation(
        context->page_reservation_ledger,
        scratchbird::storage::page::ReleasePageReservationRequest{
            result.reservation_id,
            "insert_physical_integration_failure:" + diagnostic_code});
    return Refuse(std::move(diagnostic_code), std::move(message_key), std::move(detail));
  };

  scratchbird::storage::page::InsertPageSelectionRequest selection_request;
  selection_request.database_uuid = request.database_uuid;
  selection_request.transaction_uuid = request.transaction_uuid;
  selection_request.local_transaction_id = request.local_transaction_id;
  selection_request.object_uuid = request.object_uuid;
  selection_request.reservation_id = reservation.reservation.reservation_id;
  selection_request.page_family = request.page_family;
  selection_request.encoded_row_bytes = request.encoded_row_bytes;

  auto selection = scratchbird::storage::page::SelectInsertTargetPage(
      context->page_selection_ledger,
      context->page_reservation_ledger,
      selection_request);
  if (!selection.ok() && request.request_filespace_growth_on_missing_page) {
    if (context->filespace_growth_ledger == nullptr || context->filespace_registry == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_filespace_authority",
                                      "engine.insert.physical.missing_filespace_authority",
                                      "filespace growth ledger and registry are required for growth admission");
    }
    scratchbird::storage::filespace::InsertFilespaceGrowthRequest growth_request;
    growth_request.database_uuid = request.database_uuid;
    growth_request.filespace_uuid = resolved_filespace_uuid;
    growth_request.filespace_role = resolved_growth_role;
    growth_request.page_family = request.page_family;
    growth_request.requested_page_count = request.growth_page_count == 0 ? 1 : request.growth_page_count;
    growth_request.urgency_class = request.growth_urgency;
    growth_request.predicted_insert_pressure_pages = request.predicted_insert_pressure_pages;
    growth_request.policy_uuid = request.policy_uuid;
    const auto growth = scratchbird::storage::filespace::RequestInsertFilespaceGrowth(
        context->filespace_growth_ledger,
        *context->filespace_registry,
        growth_request);
    if (!growth.ok()) {
      return refuse_after_reservation(growth.diagnostic.diagnostic_code,
                                      growth.diagnostic.message_key,
                                      "filespace growth admission refused");
    }
    result.filespace_growth_admitted = true;
    result.growth_operation_id = growth.growth_operation_id;
    result.evidence_refs.push_back(EvidenceRef("filespace_growth", growth.growth_operation_id));
    (void)scratchbird::storage::page::ReleaseInsertPageReservation(
        context->page_reservation_ledger,
        scratchbird::storage::page::ReleasePageReservationRequest{
            result.reservation_id,
            "insert_physical_integration_growth_admitted_without_append"});
    result.status = IntegrationOkStatus();
    result.integrated = false;
    result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                               "ok",
                                                               "engine.insert.physical.growth_admitted",
                                                               "filespace growth admitted; row was not physically integrated");
    return result;
  } else if (!selection.ok()) {
    return refuse_after_reservation(selection.diagnostic.diagnostic_code,
                                    selection.diagnostic.message_key,
                                    "page selection refused");
  } else {
    result.page_selected = true;
    result.selection_fence = selection.selection.selection_fence;
    result.evidence_refs.push_back("page_selection:" + selection.selection.selection_fence);
  }

  if (request.enable_deferred_secondary_index) {
    if (!(request.deferred_index_overlay_gate &&
          request.deferred_index_merge_gate &&
          request.deferred_index_cleanup_gate &&
          request.deferred_index_recovery_gate)) {
      return refuse_after_reservation("insert_physical_integration_deferred_index_gates_unproven",
                                      "engine.insert.physical.deferred_index_gates_unproven",
                                      "deferred secondary-index maintenance requires overlay, merge, cleanup, and recovery gates");
    }
    if (context->secondary_index_overlay_ledger == nullptr ||
        context->secondary_index_merge_ledger == nullptr ||
        context->secondary_index_base_entries == nullptr ||
        context->secondary_index_delta_ledger == nullptr ||
        !request.secondary_index_uuid.valid()) {
      return refuse_after_reservation("insert_physical_integration_missing_index_authority",
                                      "engine.insert.physical.missing_index_authority",
                                      "secondary-index overlay, merge, base index, and delta ledgers are required");
    }
    scratchbird::core::index::SecondaryIndexOverlayRequest overlay_request;
    overlay_request.index_uuid = request.secondary_index_uuid;
    overlay_request.table_uuid = request.object_uuid;
    overlay_request.transaction_uuid = request.transaction_uuid;
    overlay_request.local_transaction_id = request.local_transaction_id;
    overlay_request.snapshot_high_water_local_transaction_id =
        request.secondary_index_snapshot_high_water_local_transaction_id;
    const auto overlay = scratchbird::core::index::BuildSecondaryIndexDeltaOverlay(
        context->secondary_index_overlay_ledger,
        *context->secondary_index_base_entries,
        *context->secondary_index_delta_ledger,
        overlay_request);
    if (!overlay.ok()) {
      return refuse_after_reservation(overlay.diagnostic.diagnostic_code,
                                      overlay.diagnostic.message_key,
                                      "secondary-index overlay refused");
    }
    scratchbird::core::index::SecondaryIndexMergeRequest merge_request;
    merge_request.index_uuid = request.secondary_index_uuid;
    merge_request.table_uuid = request.object_uuid;
    merge_request.merge_id = GeneratedId(UuidKind::object, 300001);
    merge_request.authoritative_cleanup_horizon_local_transaction_id =
        request.secondary_index_cleanup_horizon_local_transaction_id;
    merge_request.cleanup_horizon_authoritative = true;
    const auto merge = scratchbird::core::index::MergeSecondaryIndexDeltas(
        context->secondary_index_merge_ledger,
        context->secondary_index_base_entries,
        context->secondary_index_delta_ledger,
        merge_request);
    if (!merge.ok()) {
      return refuse_after_reservation(merge.diagnostic.diagnostic_code,
                                      merge.diagnostic.message_key,
                                      "secondary-index merge refused");
    }
    const auto recovery = scratchbird::core::index::ClassifySecondaryIndexMergeLedgerForRecovery(
        *context->secondary_index_merge_ledger);
    if (!recovery.ok()) {
      return refuse_after_reservation(recovery.diagnostic.diagnostic_code,
                                      recovery.diagnostic.message_key,
                                      "secondary-index recovery classification refused");
    }
    result.deferred_secondary_index_verified = true;
    result.evidence_refs.push_back(EvidenceRef("secondary_index_merge", merge.evidence.evidence_id));
  }

  if (request.persist_overflow_payload) {
    if (context->overflow_ledger == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_overflow_authority",
                                      "engine.insert.physical.missing_overflow_authority",
                                      "overflow ledger is required");
    }
    scratchbird::storage::page::OverflowPersistRequest overflow_request;
    overflow_request.row_uuid = request.row_uuid;
    overflow_request.object_uuid = request.object_uuid;
    overflow_request.transaction_uuid = request.transaction_uuid;
    overflow_request.local_transaction_id = request.local_transaction_id;
    overflow_request.value_descriptor = request.overflow_value_descriptor;
    overflow_request.payload_bytes = request.overflow_payload;
    overflow_request.chunk_policy_uuid = request.policy_uuid;
    overflow_request.chunk_size = request.overflow_chunk_size;
    const auto overflow = scratchbird::storage::page::PersistOverflowValue(context->overflow_ledger, overflow_request);
    if (!overflow.ok()) {
      return refuse_after_reservation(overflow.diagnostic.diagnostic_code,
                                      overflow.diagnostic.message_key,
                                      "overflow persistence refused");
    }
    const auto commit = scratchbird::storage::page::CommitOverflowValue(
        context->overflow_ledger,
        scratchbird::storage::page::OverflowCommitRequest{
            overflow.overflow_value_uuid,
            request.transaction_uuid,
            request.local_transaction_id,
            "insert_physical_integration"});
    if (!commit.ok()) {
      return refuse_after_reservation(commit.diagnostic.diagnostic_code,
                                      commit.diagnostic.message_key,
                                      "overflow commit refused");
    }
    result.overflow_persisted = true;
    result.overflow_value_uuid = overflow.overflow_value_uuid;
    result.evidence_refs.push_back(EvidenceRef("overflow", overflow.overflow_value_uuid));
  }

  if (request.run_strict_bulk_load) {
    if (context->strict_bulk_load_ledger == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_bulk_load_authority",
                                      "engine.insert.physical.missing_bulk_load_authority",
                                      "strict bulk-load ledger is required");
    }
    scratchbird::core::bulk_load::StrictBulkLoadBeginRequest begin_request;
    begin_request.database_uuid = request.database_uuid;
    begin_request.object_uuid = request.object_uuid;
    begin_request.transaction_uuid = request.transaction_uuid;
    begin_request.local_transaction_id = request.local_transaction_id;
    begin_request.policy = request.strict_bulk_load_policy;
    begin_request.staging_target = request.strict_bulk_load_staging_target;
    const auto begin = scratchbird::core::bulk_load::BeginStrictBulkLoad(
        context->strict_bulk_load_ledger,
        begin_request);
    if (!begin.ok()) {
      return refuse_after_reservation(begin.diagnostic.diagnostic_code,
                                      begin.diagnostic.message_key,
                                      "strict bulk-load begin refused");
    }
    const auto append = scratchbird::core::bulk_load::AppendStrictBulkLoadRows(
        context->strict_bulk_load_ledger,
        scratchbird::core::bulk_load::StrictBulkLoadAppendRequest{
            begin.operation.bulk_load_id,
            request.transaction_uuid,
            request.local_transaction_id,
            request.strict_bulk_load_rows});
    if (!append.ok()) {
      return refuse_after_reservation(append.diagnostic.diagnostic_code,
                                      append.diagnostic.message_key,
                                      "strict bulk-load append refused");
    }
    const auto finalize = scratchbird::core::bulk_load::FinalizeStrictBulkLoad(
        context->strict_bulk_load_ledger,
        scratchbird::core::bulk_load::StrictBulkLoadFinalizeRequest{
            begin.operation.bulk_load_id,
            request.transaction_uuid,
            request.local_transaction_id,
            false,
            "insert-physical-strict-bulk-load-fence"});
    if (!finalize.ok()) {
      return refuse_after_reservation(finalize.diagnostic.diagnostic_code,
                                      finalize.diagnostic.message_key,
                                      "strict bulk-load finalize refused");
    }
    result.strict_bulk_load_finalized = true;
    result.strict_bulk_load_id = begin.operation.bulk_load_id;
    result.evidence_refs.push_back(EvidenceRef("strict_bulk_load", begin.operation.bulk_load_id));
  }

  if (result.page_selected) {
    const auto append = scratchbird::storage::page::AppendRowToSelectedPageWithReservationLedger(
        context->page_selection_ledger,
        context->page_reservation_ledger,
        scratchbird::storage::page::InsertPageAppendRequest{
            result.selection_fence,
            request.encoded_row_bytes,
            "insert_physical_integration"});
    if (!append.ok()) {
      return refuse_after_reservation(append.diagnostic.diagnostic_code,
                                      append.diagnostic.message_key,
                                      "selected page append refused");
    }
  }

  result.status = IntegrationOkStatus();
  result.integrated = result.page_selected;
  result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                            "ok",
                                                            "engine.insert.physical.integrated",
                                                            "insert physical integration completed");
  return result;
}

DiagnosticRecord MakeInsertPhysicalIntegrationDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "engine.insert.physical_integration",
                                                     status.ok() ? "" : "fall back to existing safe insert path and do not claim physical integration success");
}

DirectPhysicalBulkAppendResult ExecuteDirectPhysicalBulkAppend(
    const DirectPhysicalBulkAppendRequest& request) {
  const auto write_result_policy = ResolveWriteResultPolicyOptions(
      request.option_envelopes,
      "dml.direct_physical_bulk_append");
  if (!write_result_policy.ok) {
    auto failure = DirectBulkFailure(
        request,
        write_result_policy.diagnostic,
        "write_result_policy_refused");
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  if (!request.direct_lane_enabled) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "direct_physical_lane_disabled"),
        "direct_physical_lane_disabled");
  }
  if (request.context.local_transaction_id == 0) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "local_transaction_id_required"),
        "local_transaction_id_required");
  }
  if (request.target_table.uuid.canonical.empty()) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "target_table_uuid_required"),
        "target_table_uuid_required");
  }
  if (request.borrowed_input_rows.empty()) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "canonical_rows_required"),
        "canonical_rows_required");
  }
  if (request.duplicate_mode != "error") {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "direct_physical_lane_duplicate_mode_unsupported"),
        "direct_physical_lane_duplicate_mode_unsupported");
  }

  const auto loaded = LoadMgaRelationStoreStateForInsertTarget(
      request.context,
      request.target_table.uuid.canonical);
  (void)scratchbird::core::metrics::RecordInsertRelationStateLoad(
      request.target_table.uuid.canonical,
      "copy_import",
      loaded.full_state_load,
      loaded.scoped_state_load,
      "direct_physical_bulk_insert_target_scoped");
  if (!loaded.ok) {
    return DirectBulkFailure(request,
                             loaded.diagnostic,
                             "mga_relation_store_load_failed");
  }
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindVisibleCrudTable(state,
                                          request.target_table.uuid.canonical,
                                          request.context.local_transaction_id);
  if (!table) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "target_table_not_visible"),
        "target_table_not_visible");
  }
  if (table->temporary && request.context.session_uuid.canonical.empty()) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "temporary_table_requires_session_uuid"),
        "temporary_table_requires_session_uuid");
  }
  if (CrudRowsTouchOpaqueColumn(*table, request.borrowed_input_rows) &&
      !DirectOpaqueColumnsAllowed(request)) {
    return DirectBulkFailure(
        request,
        UnsupportedCrudFeatureDiagnostic("dml.direct_physical_bulk_append",
                                         "opaque_column_mutation_denied"),
        "opaque_column_mutation_denied");
  }
  const std::string precheck_failure =
      DirectPageExtentPreallocationPrecheckFailure(request);
  if (!precheck_failure.empty()) {
    EngineDmlSummaryCounters summary;
    summary.preallocation_refused = 1;
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     precheck_failure),
        precheck_failure,
        summary);
  }

  const auto visible_indexes = VisibleCrudIndexesForTable(
      state,
      request.target_table.uuid.canonical,
      request.context.local_transaction_id);
  MgaRelationStorageDescriptor relation_descriptor;
  const auto descriptor_ready = EnsureMgaRelationStorageDescriptor(
      request.context,
      *table,
      visible_indexes,
      &relation_descriptor);
  if (descriptor_ready.error) {
    return DirectBulkFailure(request,
                             descriptor_ready,
                             "relation_descriptor_refused");
  }

  const EngineInsertRowsRequest synthetic_insert =
      SyntheticInsertRequestForDirectBulk(request);
  const bool force_large_values_for_insert =
      InsertBatchOptionEnabled(synthetic_insert, "large_value.force_toast=true");
  InsertBatchContext batch_context =
      BeginInsertBatchContext(synthetic_insert, state, *table, visible_indexes);
  if (!batch_context.accepted) {
    const std::string reason = batch_context.fallback_reason.empty()
                                   ? "direct_physical_batch_refused"
                                   : batch_context.fallback_reason;
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_batch_fallback_total",
                            1.0,
                            "fallback",
                            reason);
    EngineDmlSummaryCounters summary;
    AddDmlSummaryFallbackReason(&summary, reason);
    auto failure = DirectBulkFailure(request,
                                     MakeInvalidRequestDiagnostic(
                                         "dml.direct_physical_bulk_append",
                                         reason),
                                     reason,
                                     summary);
    AddInsertBatchEvidenceToResult(batch_context, &failure);
    return failure;
  }
  const auto bulk_validation = ValidateStrictBulkLoadEligibility(batch_context, *table);
  if (bulk_validation.error) {
    return DirectBulkFailureWithEvidence(request,
                                         bulk_validation,
                                         "strict_bulk_load_refused",
                                         batch_context.evidence);
  }

  DirectPhysicalBulkAppendResult result;
  result.ok = true;
  result.operation_id = "dml.direct_physical_bulk_append";
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.direct_lane_selected = true;
  AddEmbeddedTrustModeEvidence(request.context, &result);
  AddDirectLaneBaseEvidence(request, &result);
  result.evidence.insert(result.evidence.end(),
                         loaded.evidence.begin(),
                         loaded.evidence.end());
  result.evidence.push_back({"relation_state_full_loads",
                             loaded.full_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_scoped_loads",
                             loaded.scoped_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_load_reason",
                             "direct_physical_bulk_insert_target_scoped"});
  result.evidence.push_back({"relation_descriptor",
                             relation_descriptor.descriptor_uuid.canonical});
  const DirectBulkUuidBatch uuid_batch = BuildDirectBulkUuidBatch(request);
  AddDirectBulkUuidBatchEvidence(uuid_batch, &result);
  if (batch_context.page_reservation.reservation_available) {
    ++result.dml_summary.page_reservations;
  }

  ConstraintDmlValidationCache constraint_cache;
  std::vector<CrudRowVersionRecord> staged_rows;
  std::vector<CrudRowVersionRecord> returning_rows;
  std::vector<std::vector<std::pair<std::string, std::string>>> logical_value_batch;
  const bool suppress_payload_rows =
      WriteResultPolicySuppressesPayloadRows(write_result_policy);
  const bool has_default_validators =
      batch_context.row_encoder_plan.default_validator_count != 0;
  const bool has_domain_validators =
      batch_context.row_encoder_plan.domain_validator_count != 0;
  const bool has_not_null_validators =
      batch_context.row_encoder_plan.not_null_validator_count != 0;
  const bool has_check_validators =
      batch_context.row_encoder_plan.check_validator_count != 0;
  const bool has_immediate_row_validators =
      has_not_null_validators || has_check_validators;
  const bool can_use_direct_not_null_validation =
      has_not_null_validators && !has_check_validators;
  const bool can_use_ordered_row_fast_path =
      !has_default_validators &&
      !has_domain_validators &&
      !has_check_validators;
  const bool rowset_shared_shape =
      DirectOptionTruthy(request, "sblr.canonical_rowset_shared_shape");
  const bool can_use_shared_ordered_row_fast_path =
      can_use_ordered_row_fast_path &&
      rowset_shared_shape &&
      !request.borrowed_input_rows.empty() &&
      DirectBulkInputMatchesEncoderOrder(request.borrowed_input_rows.front(),
                                         batch_context.row_encoder_plan);
  const std::vector<std::size_t> not_null_ordinals =
      can_use_direct_not_null_validation && can_use_shared_ordered_row_fast_path
          ? DirectNotNullValidationOrdinals(batch_context.row_encoder_plan)
          : std::vector<std::size_t>{};
  std::vector<unsigned char> not_null_ordinal_mask;
  const bool can_use_shared_row_stage_fast_path =
      can_use_shared_ordered_row_fast_path &&
      !has_default_validators &&
      !has_domain_validators &&
      !has_check_validators;
  if (can_use_shared_row_stage_fast_path && can_use_direct_not_null_validation &&
      !request.borrowed_input_rows.empty()) {
    not_null_ordinal_mask.assign(request.borrowed_input_rows.front().fields.size(), 0);
    for (const std::size_t ordinal : not_null_ordinals) {
      if (ordinal < not_null_ordinal_mask.size()) {
        not_null_ordinal_mask[ordinal] = 1;
      }
    }
  }
  staged_rows.reserve(request.borrowed_input_rows.size());
  returning_rows.reserve(request.borrowed_input_rows.size());
  logical_value_batch.reserve(request.borrowed_input_rows.size());
  constexpr std::size_t kDirectBulkMaxStoredInsertTraceEvents = 128;
  constexpr std::size_t kDirectBulkTraceRowsToStore =
      kDirectBulkMaxStoredInsertTraceEvents / 2;

  std::size_t row_ordinal = 0;
  for (const auto& input_row : request.borrowed_input_rows) {
    const bool store_row_trace =
        row_ordinal < kDirectBulkTraceRowsToStore;
    if (store_row_trace) {
      AddInsertTrace(&batch_context,
                     "direct_physical_bulk.row.convert",
                     "row",
                     std::to_string(batch_context.actual_row_count));
    }

    if (can_use_shared_row_stage_fast_path) {
      CrudRowVersionRecord row_record;
      row_record.creator_tx = request.context.local_transaction_id;
      row_record.table_uuid = request.target_table.uuid.canonical;
      row_record.row_uuid = uuid_batch.row_uuids[row_ordinal];
      row_record.version_uuid = uuid_batch.version_uuids[row_ordinal];
      row_record.temporary_session_uuid =
          table->temporary ? request.context.session_uuid.canonical : "";
      row_record.deleted = false;
      row_record.values.reserve(input_row.fields.size());

      std::uint64_t encoded_bytes = 0;
      bool saw_default_marker = false;
      std::string not_null_failure;
      for (std::size_t field_index = 0; field_index < input_row.fields.size(); ++field_index) {
        const auto& [field, typed] = input_row.fields[field_index];
        if (!typed.is_null && typed.encoded_value == "<DEFAULT>") {
          saw_default_marker = true;
          break;
        }
        if (field_index < not_null_ordinal_mask.size() &&
            not_null_ordinal_mask[field_index] &&
            (typed.is_null || DirectNullValue(typed.encoded_value))) {
          not_null_failure =
              field_index < batch_context.row_encoder_plan.columns.size()
                  ? batch_context.row_encoder_plan.columns[field_index].column_name
                  : field;
          break;
        }
        if (typed.is_null) {
          row_record.values.push_back({field, kDirectNullMarker});
          encoded_bytes += field.size() + sizeof(kDirectNullMarker) - 1;
        } else {
          row_record.values.push_back({field, typed.encoded_value});
          encoded_bytes += field.size() + typed.encoded_value.size();
        }
      }
      if (!saw_default_marker) {
        if (!not_null_failure.empty()) {
          return DirectBulkFailure(
              request,
              MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                           "not_null_validation_refused"),
              "not_null_validation_refused:" + not_null_failure);
        }
        const bool toast_required =
            encoded_bytes > batch_context.row_template.max_inline_encoded_bytes ||
            force_large_values_for_insert;
        const std::uint64_t projected_memory_bytes =
            toast_required ? batch_context.row_template.max_inline_encoded_bytes
                           : encoded_bytes;
        if (!batch_context.memory_policy.spill_allowed &&
            projected_memory_bytes > batch_context.memory_policy.context_budget_bytes) {
          const auto memory_validation = ValidateInsertBatchMemoryBudget(
              batch_context,
              projected_memory_bytes);
          return DirectBulkFailure(request,
                                   memory_validation,
                                   "insert_batch_memory_budget_refused");
        }
        if (store_row_trace) {
          AddInsertTrace(&batch_context,
                         "direct_physical_bulk.row.stage",
                         "stage",
                         row_record.row_uuid);
        }
        logical_value_batch.push_back(row_record.values);
        staged_rows.push_back(std::move(row_record));
        ++row_ordinal;
        continue;
      }
    }

    PreparedInsertRow prepared =
        can_use_shared_ordered_row_fast_path
            ? PrepareDirectBulkOrderedRowFast(input_row,
                                              batch_context.row_template,
                                              uuid_batch.row_uuids[row_ordinal],
                                              force_large_values_for_insert)
            : can_use_ordered_row_fast_path &&
                DirectBulkInputMatchesEncoderOrder(input_row,
                                                   batch_context.row_encoder_plan)
                  ? PrepareDirectBulkOrderedRowFast(input_row,
                                                    batch_context.row_template,
                                                    uuid_batch.row_uuids[row_ordinal],
                                                    force_large_values_for_insert)
                  : PrepareInsertRowForBatch(synthetic_insert,
                                             input_row,
                                             batch_context.row_template,
                                             batch_context.row_encoder_plan);
    prepared.row_uuid = uuid_batch.row_uuids[row_ordinal];
    auto values = std::move(prepared.values);

    ConstraintDmlValidationOptions direct_constraint_options;
    direct_constraint_options.validate_unique_constraints = false;
    direct_constraint_options.validate_foreign_key_constraints = false;
    bool values_mutated_by_validation = false;

    const bool default_requested =
        std::any_of(values.begin(), values.end(), [](const auto& field) {
          return field.second == "<DEFAULT>";
        });
    if (has_default_validators || default_requested) {
      const auto default_validation =
          ApplyConstraintDefaultsForInsert(request.context, *table, values);
      if (!default_validation.ok) {
        return DirectBulkFailure(request,
                                 default_validation.diagnostic,
                                 "constraint_default_refused");
      }
      values = default_validation.values;
      values_mutated_by_validation = true;
      result.evidence.insert(result.evidence.end(),
                             default_validation.evidence.begin(),
                             default_validation.evidence.end());
    }

    if (has_domain_validators) {
      const auto domain_validation = ApplyDomainRulesToCrudValues(
          request.context,
          table->columns,
          values,
          request.context.local_transaction_id,
          &constraint_cache);
      if (!domain_validation.ok) {
        return DirectBulkFailure(request,
                                 domain_validation.diagnostic,
                                 "domain_validation_refused");
      }
      values = domain_validation.values;
      values_mutated_by_validation = true;
      result.evidence.insert(result.evidence.end(),
                             domain_validation.evidence.begin(),
                             domain_validation.evidence.end());
    }

    if (can_use_direct_not_null_validation) {
      const std::string not_null_failure =
          can_use_shared_ordered_row_fast_path
              ? DirectNotNullValidationFailureOrdered(
                    batch_context.row_encoder_plan, values, not_null_ordinals)
              : DirectNotNullValidationFailure(batch_context.row_encoder_plan,
                                               values);
      if (!not_null_failure.empty()) {
        return DirectBulkFailure(
            request,
            MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                         "not_null_validation_refused"),
            "not_null_validation_refused:" + not_null_failure);
      }
    } else if (has_immediate_row_validators) {
      const auto constraint_validation = ValidateImmediateRowConstraintsWithOptions(
          request.context,
          state,
          *table,
          prepared.row_uuid,
          values,
          "insert",
          direct_constraint_options,
          &constraint_cache);
      if (!constraint_validation.ok) {
        return DirectBulkFailure(request,
                                 constraint_validation.diagnostic,
                                 "constraint_validation_refused");
      }
      values = constraint_validation.values;
      values_mutated_by_validation = true;
      result.evidence.insert(result.evidence.end(),
                             constraint_validation.evidence.begin(),
                             constraint_validation.evidence.end());
    }

    prepared.values = std::move(values);
    if (values_mutated_by_validation) {
      prepared.encoded_bytes =
          static_cast<EngineApiU64>(EncodedValueBytes(prepared.values));
      prepared.toast_required =
          prepared.encoded_bytes > batch_context.row_template.max_inline_encoded_bytes ||
          force_large_values_for_insert;
    }
    const std::uint64_t projected_memory_bytes =
        prepared.toast_required ? batch_context.row_template.max_inline_encoded_bytes
                                : prepared.encoded_bytes;
    if (!batch_context.memory_policy.spill_allowed &&
        projected_memory_bytes > batch_context.memory_policy.context_budget_bytes) {
      const auto memory_validation = ValidateInsertBatchMemoryBudget(
          batch_context,
          projected_memory_bytes);
      return DirectBulkFailure(request,
                               memory_validation,
                               "insert_batch_memory_budget_refused");
    }
    const auto batch_constraint =
        ValidateInsertBatchConstraints(batch_context, state, prepared);
    if (batch_constraint.error) {
      return DirectBulkFailure(request,
                               batch_constraint,
                               "insert_batch_constraint_refused");
    }

    CrudRowVersionRecord row_record;
    row_record.creator_tx = request.context.local_transaction_id;
    row_record.table_uuid = request.target_table.uuid.canonical;
    row_record.row_uuid = prepared.row_uuid;
    row_record.version_uuid = uuid_batch.version_uuids[row_ordinal];
    row_record.temporary_session_uuid =
        table->temporary ? request.context.session_uuid.canonical : "";
    row_record.deleted = false;
    row_record.values = prepared.values;
    if (store_row_trace) {
      AddInsertTrace(&batch_context,
                     "direct_physical_bulk.row.stage",
                     "stage",
                     prepared.row_uuid);
    }
    staged_rows.push_back(std::move(row_record));
    logical_value_batch.push_back(std::move(prepared.values));
    ++row_ordinal;
  }
  if (request.borrowed_input_rows.size() > kDirectBulkTraceRowsToStore) {
    const std::uint64_t omitted =
        static_cast<std::uint64_t>(
            request.borrowed_input_rows.size() - kDirectBulkTraceRowsToStore) *
        2u;
    batch_context.trace_event_count += omitted;
    batch_context.trace_event_compacted_count += omitted;
  }

  const auto constraint_proof = BuildDirectBulkConstraintProof(
      request,
      state,
      *table,
      visible_indexes,
      staged_rows,
      logical_value_batch);
  if (!constraint_proof.ok) {
    return DirectBulkFailureWithEvidence(request,
                                         constraint_proof.diagnostic,
                                         constraint_proof.failure_reason,
                                         constraint_proof.evidence,
                                         result.dml_summary);
  }
  result.evidence.insert(result.evidence.end(),
                         constraint_proof.evidence.begin(),
                         constraint_proof.evidence.end());
  result.dml_summary.index_probes +=
      DirectUniqueIndexProbeCount(visible_indexes, staged_rows.size());

  const auto synchronous_indexes = DirectSynchronousIndexes(batch_context);
  const auto sorted_index_build = BuildDirectSortedBulkIndexArtifacts(
      request,
      state,
      synchronous_indexes,
      staged_rows,
      logical_value_batch);
  if (!sorted_index_build.ok) {
    auto evidence = result.evidence;
    evidence.insert(evidence.end(),
                    sorted_index_build.evidence.begin(),
                    sorted_index_build.evidence.end());
    return DirectBulkFailureWithEvidence(request,
                                         sorted_index_build.diagnostic,
                                         sorted_index_build.failure_reason,
                                         evidence,
                                         result.dml_summary);
  }
  result.evidence.insert(result.evidence.end(),
                         sorted_index_build.evidence.begin(),
                         sorted_index_build.evidence.end());

  auto ordered_ingest =
      ApplyDirectOrderedIngestPlan(request, &staged_rows, &logical_value_batch);
  if (!ordered_ingest.ok) {
    return DirectBulkFailureWithEvidence(request,
                                         ordered_ingest.diagnostic,
                                         ordered_ingest.failure_reason,
                                         ordered_ingest.evidence,
                                         result.dml_summary);
  }
  result.evidence.insert(result.evidence.end(),
                         ordered_ingest.evidence.begin(),
                         ordered_ingest.evidence.end());

  const auto row_allocation_start = DirectSteadyClock::now();
  const auto row_allocation = ReserveDmlPageAllocationRuntime(
      request.context,
      request.option_envelopes,
      request.target_table.uuid.canonical,
      DmlPageAllocationRuntimeFamily::row_data,
      static_cast<std::uint64_t>(staged_rows.size()),
      "direct_physical_bulk.row_data");
  const EngineApiU64 row_allocation_elapsed =
      DirectElapsedMicros(row_allocation_start, DirectSteadyClock::now());
  if (!row_allocation.ok()) {
    AddDirectAllocationResourceSummary(
        row_allocation,
        "row",
        static_cast<EngineApiU64>(staged_rows.size()),
        row_allocation_elapsed,
        batch_context,
        false,
        &result);
    auto evidence = result.evidence;
    evidence.insert(evidence.end(),
                    row_allocation.evidence.begin(),
                    row_allocation.evidence.end());
    const std::string reason =
        DirectPageExtentPreallocationRequired(request)
            ? RequiredPreallocationFailureReason(row_allocation, "row")
            : std::string{};
    EngineDmlSummaryCounters summary = result.dml_summary;
    AddPreallocationRuntimeCounters(row_allocation, &summary);
    if (!reason.empty()) {
      summary.preallocation_refused = std::max<EngineApiU64>(
          summary.preallocation_refused,
          1);
    }
    return DirectBulkFailureWithEvidence(
        request,
        row_allocation.diagnostic,
        reason.empty() ? "row_page_allocation_refused" : reason,
        evidence,
        summary);
  }
  AddDmlPageAllocationRuntimeEvidence(row_allocation, &result);
  AddDirectAllocationResourceSummary(
      row_allocation,
      "row",
      static_cast<EngineApiU64>(staged_rows.size()),
      row_allocation_elapsed,
      batch_context,
      !DirectPageExtentPreallocationRequired(request),
      &result);
  if (row_allocation.active) {
    ++result.dml_summary.page_reservations;
  }
  if (DirectPageExtentPreallocationRequired(request)) {
    const std::string reason =
        RequiredPreallocationFailureReason(row_allocation, "row");
    if (!reason.empty()) {
      EngineDmlSummaryCounters summary = result.dml_summary;
      AddPreallocationRuntimeCounters(row_allocation, &summary);
      summary.preallocation_refused = std::max<EngineApiU64>(
          summary.preallocation_refused,
          1);
      return DirectBulkFailureWithEvidence(
          request,
          MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append", reason),
          reason,
          result.evidence,
          summary);
    }
  }

  const auto index_allocation_start = DirectSteadyClock::now();
  const auto index_allocation = ReserveDmlIndexPageAllocationRuntimeForRows(
      request.context,
      request.option_envelopes,
      state,
      request.target_table.uuid.canonical,
      logical_value_batch,
      "direct_physical_bulk.index");
  const EngineApiU64 index_allocation_elapsed =
      DirectElapsedMicros(index_allocation_start, DirectSteadyClock::now());
  if (!index_allocation.ok()) {
    AddDirectAllocationResourceSummary(index_allocation,
                                       "index",
                                       0,
                                       index_allocation_elapsed,
                                       batch_context,
                                       false,
                                       &result);
    auto evidence = result.evidence;
    evidence.insert(evidence.end(),
                    index_allocation.evidence.begin(),
                    index_allocation.evidence.end());
    const std::string reason =
        DirectPageExtentPreallocationRequired(request)
            ? RequiredPreallocationFailureReason(index_allocation, "index")
            : std::string{};
    EngineDmlSummaryCounters summary = result.dml_summary;
    AddPreallocationRuntimeCounters(index_allocation, &summary);
    if (!reason.empty()) {
      summary.preallocation_refused = std::max<EngineApiU64>(
          summary.preallocation_refused,
          1);
    }
    return DirectBulkFailureWithEvidence(
        request,
        index_allocation.diagnostic,
        reason.empty() ? "index_page_allocation_refused" : reason,
        evidence,
        summary);
  }
  AddDmlPageAllocationRuntimeEvidence(index_allocation, &result);
  AddDirectAllocationResourceSummary(index_allocation,
                                     "index",
                                     0,
                                     index_allocation_elapsed,
                                     batch_context,
                                     !DirectPageExtentPreallocationRequired(request),
                                     &result);
  if (index_allocation.active) {
    ++result.dml_summary.page_reservations;
  }
  if (DirectPageExtentPreallocationRequired(request)) {
    const std::string reason =
        RequiredPreallocationFailureReason(index_allocation, "index");
    if (!reason.empty()) {
      EngineDmlSummaryCounters summary = result.dml_summary;
      AddPreallocationRuntimeCounters(index_allocation, &summary);
      summary.preallocation_refused = std::max<EngineApiU64>(
          summary.preallocation_refused,
          1);
      return DirectBulkFailureWithEvidence(
          request,
          MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append", reason),
          reason,
          result.evidence,
          summary);
    }
    AddRequiredPreallocationSummary(row_allocation,
                                    index_allocation,
                                    staged_rows.size(),
                                    &result);
  }

  bool large_value_persistence_required = false;
  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    if (force_large_values_for_insert ||
        EncodedValueBytes(logical_value_batch[index]) >
            kCrudVerticalSliceMaxEncodedValueBytes) {
      large_value_persistence_required = true;
      break;
    }
  }
  if (large_value_persistence_required) {
    std::vector<std::vector<std::pair<std::string, std::string>>> storage_value_batch =
        logical_value_batch;
    std::vector<MgaLargeValuePersistBatchRowInput> large_value_rows;
    large_value_rows.reserve(staged_rows.size());
    for (std::size_t index = 0; index < staged_rows.size(); ++index) {
      large_value_rows.push_back(
          {request.target_table.uuid.canonical,
           staged_rows[index].row_uuid,
           staged_rows[index].version_uuid,
           force_large_values_for_insert ||
               EncodedValueBytes(storage_value_batch[index]) >
                   kCrudVerticalSliceMaxEncodedValueBytes,
           &storage_value_batch[index]});
    }
    MgaLargeValuePersistBatchCounters large_value_counters;
    const auto large_value_persisted = PersistMgaLargeValuesForRows(
        request.context,
        large_value_rows,
        &large_value_counters,
        &result.evidence);
    if (large_value_persisted.error) {
      return DirectBulkFailure(request,
                               large_value_persisted,
                               "large_value_persistence_refused");
    }
    for (std::size_t index = 0; index < staged_rows.size(); ++index) {
      staged_rows[index].values = std::move(storage_value_batch[index]);
    }
    result.evidence.push_back({"mga_large_value_batch_rows_seen",
                               std::to_string(large_value_counters.rows_seen)});
  }

  const auto row_page_write =
      WriteDirectPhysicalMgaCowRows(request, staged_rows);
  result.evidence.insert(result.evidence.end(),
                         row_page_write.evidence.begin(),
                         row_page_write.evidence.end());
  if (!row_page_write.ok) {
    if (DirectPhysicalMgaCowRequired(request)) {
      return DirectBulkFailureWithEvidence(
          request,
          row_page_write.diagnostic,
          "physical_mga_cow_row_page_refused",
          result.evidence,
          result.dml_summary);
    }
    result.evidence.push_back({"direct_physical_bulk_row_page_writer",
                               "fallback"});
    result.evidence.push_back({"direct_physical_bulk_row_page_fallback_reason",
                               row_page_write.diagnostic.detail});
  }

  auto strict_lifecycle = RunDirectStrictBulkLifecycle(
      request,
      batch_context,
      staged_rows,
      logical_value_batch);
  if (!strict_lifecycle.ok) {
    return DirectBulkFailureWithEvidence(request,
                                         strict_lifecycle.diagnostic,
                                         strict_lifecycle.failure_reason,
                                         strict_lifecycle.evidence,
                                         result.dml_summary);
  }
  if (strict_lifecycle.active) {
    result.evidence.insert(result.evidence.end(),
                           strict_lifecycle.evidence.begin(),
                           strict_lifecycle.evidence.end());
    result.evidence.push_back({"strict_bulk_load_direct_lane", "enabled"});
    result.evidence.push_back({"strict_bulk_load_direct_lane_id",
                               TypedUuidText(strict_lifecycle.bulk_load_id)});
  }

  MgaRelationHotAppendContext hot_append(request.context);
  std::vector<std::uint64_t> written_event_sequences;
  if (strict_lifecycle.active &&
      DirectOptionEnabled(request,
                          "strict_bulk_load.simulate_physical_publication_failure_after_evidence=true")) {
    return DirectStrictPhysicalPublicationFailure(
        request,
        &strict_lifecycle,
        result,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "strict_bulk_load_physical_publication_row_append_failed"),
        "strict_bulk_load_physical_publication_failed",
        "row_append");
  }
  const auto rows_appended =
      IparFaultPointRequested(request.option_envelopes, "row_append")
          ? IparFaultDiagnostic("dml.direct_physical_bulk_append",
                                "row_append",
                                "phase=direct_physical_row_append")
          :
      hot_append.AppendRowVersions(&staged_rows, &written_event_sequences);
  if (rows_appended.error) {
    if (IparFaultPointRequested(request.option_envelopes, "row_append")) {
      std::vector<EngineEvidenceReference> evidence = result.evidence;
      AppendIparFaultEvidence(&evidence,
                              "row_append",
                              "rollback_required_before_direct_physical_row_append");
      return DirectBulkFailureWithEvidence(request,
                                           rows_appended,
                                           "ipar_fault_injection_row_append",
                                           evidence,
                                           result.dml_summary);
    }
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(request,
                                                    &strict_lifecycle,
                                                    result,
                                                    rows_appended,
                                                    "mga_row_append_refused",
                                                    "row_append");
    }
    return DirectBulkFailure(request,
                             rows_appended,
                             "mga_row_append_refused");
  }
  const auto rows_flushed = hot_append.FlushRowVersions();
  if (rows_flushed.error) {
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(request,
                                                    &strict_lifecycle,
                                                    result,
                                                    rows_flushed,
                                                    "mga_row_flush_refused",
                                                    "row_flush");
    }
    return DirectBulkFailure(request,
                             rows_flushed,
                             "mga_row_flush_refused");
  }

  std::vector<MgaIndexEntryRowInput> index_rows;
  index_rows.reserve(staged_rows.size());
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> delta_entries;
  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    const auto& row = staged_rows[index];
    AddInsertTrace(&batch_context,
                   "direct_physical_bulk.row.write",
                   "write",
                   row.row_uuid);
    AddInsertTrace(&batch_context,
                   "direct_physical_bulk.index.maintain",
                   "index",
                   row.row_uuid);
    index_rows.push_back({row.row_uuid,
                          row.version_uuid,
                          logical_value_batch[index]});
    auto row_delta_entries =
        DirectDeltaEntries(batch_context, row, logical_value_batch[index]);
    delta_entries.insert(delta_entries.end(),
                         std::make_move_iterator(row_delta_entries.begin()),
                         std::make_move_iterator(row_delta_entries.end()));
  }

  const auto delta_appended = AppendMgaSecondaryIndexDeltaLedgerEntries(
      request.context,
      delta_entries,
      &result.evidence);
  if (delta_appended.error) {
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(
          request,
          &strict_lifecycle,
          result,
          delta_appended,
          "secondary_index_delta_append_refused",
          "secondary_index_delta_append");
    }
    return DirectBulkFailure(request,
                             delta_appended,
                             "secondary_index_delta_append_refused");
  }

  if (!sorted_index_build.exact_batches.empty()) {
    const auto exact_appended = hot_append.AppendExactIndexEntryBatches(
        sorted_index_build.exact_batches);
    if (exact_appended.error) {
      if (strict_lifecycle.active) {
        return DirectStrictPhysicalPublicationFailure(request,
                                                      &strict_lifecycle,
                                                      result,
                                                      exact_appended,
                                                      "mga_sorted_index_append_refused",
                                                      "sorted_index_append");
      }
      return DirectBulkFailure(request,
                               exact_appended,
                               "mga_sorted_index_append_refused");
    }
    result.evidence.push_back({"sorted_bulk_index_exact_append",
                               "mga_index_append_path"});
    if (DirectDeferredIndexBenchmarkCleanRequired(request) &&
        sorted_index_build.selected &&
        sorted_index_build.retail_indexes.empty()) {
      result.evidence.push_back(
          {"orh_deferred_index_bulk_publish_benchmark_clean", "proven"});
      result.evidence.push_back(
          {"orh_deferred_index_bulk_publish_internal_proof",
           "sorted_build_root_publish_recovery_exact_append"});
      result.evidence.push_back(
          {"orh_deferred_index_bulk_publish_mga_publish_provider",
           "mga_relation_store.exact_index_entry_append"});
    }
  }

  const auto index_apply_plan = PlanLocalityAwareIndexApplyBatches(
      DirectIndexAppendBatches(sorted_index_build.retail_indexes,
                               request.target_table.uuid.canonical,
                               index_rows));
  if (!sorted_index_build.retail_indexes.empty()) {
    if (index_apply_plan.diagnostic.error) {
      if (strict_lifecycle.active) {
        return DirectStrictPhysicalPublicationFailure(
            request,
            &strict_lifecycle,
            result,
            index_apply_plan.diagnostic,
            "index_apply_locality_plan_refused",
            "index_apply_locality_plan");
      }
      return DirectBulkFailure(request,
                               index_apply_plan.diagnostic,
                               "index_apply_locality_plan_refused");
    }
    AddLocalityAwareIndexApplyEvidence(index_apply_plan, &result.evidence);
  }
  if (IparFaultPointRequested(request.option_envelopes, "index_append")) {
    std::vector<EngineEvidenceReference> evidence = result.evidence;
    AppendIparFaultEvidence(&evidence,
                            "index_append",
                            "rollback_required_after_direct_physical_row_append_before_index_append");
    evidence.push_back({"ipar_fault_injection_row_versions_staged",
                        std::to_string(staged_rows.size())});
    return DirectBulkFailureWithEvidence(
        request,
        IparFaultDiagnostic("dml.direct_physical_bulk_append",
                            "index_append",
                            "phase=direct_physical_index_append"),
        "ipar_fault_injection_index_append",
        evidence,
        result.dml_summary);
  }
  const auto index_appended = hot_append.AppendIndexEntryBatches(
      index_apply_plan.batches);
  if (index_appended.error) {
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(request,
                                                    &strict_lifecycle,
                                                    result,
                                                    index_appended,
                                                    "mga_index_append_refused",
                                                    "index_append");
    }
    return DirectBulkFailure(request,
                             index_appended,
                             "mga_index_append_refused");
  }
  const auto index_flushed = hot_append.FlushIndexEntries();
  if (index_flushed.error) {
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(request,
                                                    &strict_lifecycle,
                                                    result,
                                                    index_flushed,
                                                    "mga_index_flush_refused",
                                                    "index_flush");
    }
    return DirectBulkFailure(request,
                             index_flushed,
                             "mga_index_flush_refused");
  }

  result = PublishDirectStrictBulkAfterPhysicalSuccess(request,
                                                       &strict_lifecycle,
                                                       std::move(result));
  if (!result.ok) {
    return result;
  }

  const auto hot_counters = hot_append.counters();
  if (hot_counters.row_versions_appended != 0) {
    ++result.dml_summary.append_calls;
  }
  if (hot_counters.index_entries_appended != 0) {
    ++result.dml_summary.append_calls;
  }
  if (!delta_entries.empty()) {
    ++result.dml_summary.append_calls;
  }
  result.dml_summary.file_opens +=
      hot_counters.row_stream_opens + hot_counters.index_stream_opens;
  result.dml_summary.flushes +=
      hot_counters.row_stream_flushes + hot_counters.index_stream_flushes;

  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    const auto& row = staged_rows[index];
    if (!suppress_payload_rows) {
      CrudRowVersionRecord returning_row;
      returning_row.creator_tx = request.context.local_transaction_id;
      returning_row.event_sequence = row.event_sequence;
      returning_row.sequence = row.sequence;
      returning_row.table_uuid = request.target_table.uuid.canonical;
      returning_row.row_uuid = row.row_uuid;
      returning_row.version_uuid = row.version_uuid;
      returning_row.deleted = false;
      returning_row.values = logical_value_batch[index];
      returning_rows.push_back(std::move(returning_row));
      result.row_uuids.push_back({row.row_uuid});
    }
    ++result.inserted_rows;
    ++batch_context.actual_row_count;
  }

  AddInsertTrace(&batch_context,
                 "direct_physical_bulk.batch.finish",
                 "finish",
                 std::to_string(batch_context.actual_row_count));
  result.accepted_rows = static_cast<EngineApiU64>(request.borrowed_input_rows.size());
  result.rejected_rows = 0;
  if (suppress_payload_rows) {
    result.result_shape.result_kind = "dml_direct_physical_bulk_result_suppressed";
  } else {
    result.result_shape = CrudRowsToResultShape(returning_rows);
  }
  result.dml_summary.rows_changed = result.inserted_rows;
  result.evidence.push_back({"mga_row_version", "row_insert"});
  result.evidence.push_back({"mga_row_store", "row_insert"});
  if (hot_counters.index_entries_appended != 0) {
    result.evidence.push_back({"mga_index_store", "row_insert"});
  }
  result.evidence.push_back({"direct_mga_append", "row_version_batch"});
  result.evidence.push_back({"direct_mga_append_module",
                             "storage.mga_relation_store"});
  result.evidence.push_back({"orh_210_runtime_consumed", "true"});
  result.evidence.push_back({"orh_210_consumed_module",
                             "engine.internal_api.dml+storage.mga_relation_store"});
  result.evidence.push_back({"direct_physical_bulk_row_count",
                             std::to_string(result.inserted_rows)});
  result.evidence.push_back({"unique_index_physical_probes", "0"});
  result.evidence.push_back({"unique_index_scan_fallbacks", "0"});
  result.evidence.push_back({"unique_index_bulk_proof_probes",
                             std::to_string(result.dml_summary.index_probes)});
  result.evidence.push_back({"row_uuid_generation",
                             request.require_generated_row_uuid ? "required"
                                                                : "caller_allowed"});
  result.evidence.push_back({"trigger_udr_hooks", "inactive_checked"});
  AddHotAppendCounterEvidence(hot_counters, &result);
  AddInsertBatchEvidenceToResult(batch_context, &result);
  AddDmlSummaryEvidence(&result);
  ApplyWriteResultPolicy(write_result_policy, &result);
  RecordInsertBatchMetric(batch_context,
                          "sb_dml_insert_batch_started_total",
                          1.0,
                          "ok");
  if (result.dml_summary.index_probes != 0) {
    RecordInsertBatchMetric(batch_context,
                            "sb_index_insert_unique_physical_probe_total",
                            static_cast<double>(result.dml_summary.index_probes),
                            "bulk_unique_proof",
                            "direct_copy_bulk_unique_proof");
  }
  RecordInsertBatchMetric(batch_context,
                          "sb_dml_insert_rows_inserted_total",
                          static_cast<double>(result.inserted_rows),
                          "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api::dml
