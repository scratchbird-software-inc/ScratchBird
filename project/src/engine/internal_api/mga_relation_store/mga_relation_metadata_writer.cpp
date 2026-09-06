// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_contextual_text_descriptor.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "query/contextual_text_policy_registry_v2.hpp"
#include "query/contextual_text_target_authority_resolver_v2.hpp"
#include "query/plan_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "typed_update_carrier_codec.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_METADATA_WRITER_IMPLEMENTATION_AUTHORITY

namespace {

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::string_view kSealedTableMetadataKindV2 =
    "TABLE_METADATA_SEALED_DESCRIPTOR_V2";
constexpr std::string_view kSealedTableMetadataFormatV2 =
    "mga_sealed_contextual_text_sidecar_set_v2";
namespace sealed_table_metadata_field_v2 {
inline constexpr std::size_t kMagic = 0;
inline constexpr std::size_t kRecordKind = 1;
inline constexpr std::size_t kCreatorTx = 2;
inline constexpr std::size_t kEventSequence = 3;
inline constexpr std::size_t kFormat = 4;
inline constexpr std::size_t kSealState = 5;
inline constexpr std::size_t kTableUuid = 6;
inline constexpr std::size_t kDefaultName = 7;
inline constexpr std::size_t kColumns = 8;
inline constexpr std::size_t kTemporary = 9;
inline constexpr std::size_t kTemporaryScope = 10;
inline constexpr std::size_t kTemporarySessionUuid = 11;
inline constexpr std::size_t kOnCommitAction = 12;
inline constexpr std::size_t kRelationDescriptorUuid = 13;
inline constexpr std::size_t kRelationDescriptorGeneration = 14;
inline constexpr std::size_t kDescriptorFieldCount = 15;
inline constexpr std::size_t kDescriptorFieldBytes = 16;
inline constexpr std::size_t kContextualSidecarCount = 17;
inline constexpr std::size_t kDescriptorFields = 18;
inline constexpr std::size_t kFieldCount = 19;
}  // namespace sealed_table_metadata_field_v2
constexpr std::string_view kBigintMigrationFormat =
    "datatype_bigint_identity_migration_v1";
constexpr std::string_view kBigintMigrationId =
    "core.datatype.bigint.identity.v1";
constexpr std::string_view kLegacyBigintTypeUuid =
    "67000000-696e-7436-b400-000000000000";
constexpr std::string_view kCanonicalBigintTypeUuid =
    "019d0000-0000-7000-8000-00000000d712";
constexpr std::string_view kInt32MigrationFormat =
    "datatype_int32_identity_migration_v1";
constexpr std::string_view kInt32MigrationId =
    "core.datatype.int32.identity.v1";
constexpr std::string_view kLegacyInt32DescriptorUuid =
    "66000000-696e-7433-b200-000000000000";
constexpr std::string_view kLegacyInt32TypeUuid =
    "66000000-696e-7433-b200-000000000000";
constexpr std::string_view kCanonicalInt32DescriptorUuid =
    "019d0000-0000-7000-8000-00000000d716";
constexpr std::string_view kCanonicalInt32TypeUuid =
    "019d0000-0000-7000-8000-00000000d717";
constexpr std::string_view kTextMigrationFormat =
    "datatype_text_identity_migration_v1";
constexpr std::string_view kTextMigrationId =
    "core.datatype.text.identity.v1";
constexpr std::string_view kLegacyTextDescriptorUuid =
    "2c010000-6368-7172-a163-746572000000";
constexpr std::string_view kLegacyTextTypeUuid =
    "2c010000-6368-7172-a163-746572000000";
constexpr std::string_view kCanonicalTextDescriptorUuid =
    "019d0000-0000-7000-8000-00000000d718";
constexpr std::string_view kCanonicalTextTypeUuid =
    "019d0000-0000-7000-8000-00000000d719";
constexpr std::string_view kCanonicalTextCodecUuid =
    "019d0000-0000-7000-8000-00000000d71a";
constexpr std::string_view kCanonicalTextCodecId =
    "datatype.text.utf8.v1";

std::string MetadataStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_metadata";
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) line.push_back('\t');
    line += fields[index];
  }
  return line;
}

bool AppendLine(const std::string& path, const std::string& line) {
  if (path.empty()) return false;
  std::ofstream output(path, std::ios::app | std::ios::binary);
  if (!output) return false;
  output << line << '\n';
  output.flush();
  return static_cast<bool>(output);
}

}  // namespace

EngineApiDiagnostic AppendMgaTableMetadata(const EngineRequestContext& context,
                                           const CrudTableRecord& table) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthorityForStoreModule(
      context, "mga.relation_metadata.table_create");
  if (authority.error) { return authority; }
  CrudTableRecord writable = table;
  writable.creator_tx = context.local_transaction_id;
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  writable.event_sequence = reservation.first;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "TABLE_METADATA",
                                     std::to_string(writable.creator_tx),
                                     std::to_string(writable.event_sequence),
                                     writable.table_uuid,
                                     EncodeCrudText(writable.default_name),
                                     EncodeCrudPairs(writable.columns),
                                     writable.temporary ? "1" : "0",
                                     writable.temporary_scope,
                                     writable.temporary_session_uuid,
                                     writable.on_commit_action});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "table_metadata_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaTableMetadataWithSealedContextualTextDescriptorV2(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes,
    MgaRelationStorageDescriptor* descriptor) {
  constexpr const char* kOperation =
      "mga.relation_metadata.table_create.sealed_descriptor_v2";
  if (descriptor == nullptr) {
    return MakeInvalidRequestDiagnostic(kOperation, "descriptor_output_required");
  }
  *descriptor = {};
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "database_path_required");
  }
  const auto authority =
      ValidateMgaMutatingTransactionAuthorityForStoreModule(context,
                                                            kOperation);
  if (authority.error) return authority;
  if (!CanonicalNonNilMigrationUuid(table.table_uuid) ||
      table.columns.empty()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "table_identity_or_columns_invalid");
  }

  std::vector<std::string> allocator_lines;
  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); },
      &allocator_lines);
  if (!reservation.ok) return reservation.diagnostic;
  const auto abandon_reservation = [&]() {
    AbandonDeferredEventSequenceReservation(reservation);
    allocator_lines.clear();
  };

  CrudTableRecord writable = table;
  writable.creator_tx = context.local_transaction_id;
  writable.event_sequence = reservation.first;
  auto base_fields = BuildPersistedMgaRelationDescriptorFields(
      context, writable, indexes);
  auto relation_descriptor =
      DeserializeMgaRelationStorageDescriptor(base_fields);
  EngineApiDiagnostic material_diagnostic;
  if (!BindFreshCanonicalTextColumnIdentitiesV2(
          &writable, &relation_descriptor, &material_diagnostic)) {
    abandon_reservation();
    return material_diagnostic;
  }
  const auto validated =
      ValidateMgaRelationStorageDescriptor(relation_descriptor);
  if (validated.error) {
    abandon_reservation();
    return validated;
  }

  const bool requires_contextual_policy = std::ranges::any_of(
      relation_descriptor.columns, [](const auto& column) {
        const auto fields = StrictRelationDescriptorFields(
            column.value_descriptor.encoded_descriptor);
        const auto embedded =
            fields == std::nullopt
                ? std::map<std::string, std::string>::const_iterator{}
                : fields->find("datatype_descriptor_uuid");
        const bool canonical_text =
            column.value_descriptor.descriptor_uuid.canonical ==
                kCanonicalTextDescriptorUuid ||
            (fields != std::nullopt && embedded != fields->end() &&
             embedded->second == kCanonicalTextDescriptorUuid);
        return canonical_text && !column.charset_uuid.empty() &&
               !column.collation_uuid.empty();
      });
  EngineContextualTextPolicyRowSetV2 policy_rows;
  if (requires_contextual_policy) {
    const auto policy =
        LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
    if (!policy.ok) {
      abandon_reservation();
      return policy.diagnostic;
    }
    policy_rows = policy.rows;
  }
  MgaSealedContextualTextDescriptorMaterialV2 material;
  if (!BuildMgaSealedContextualTextDescriptorMaterialV2(
          context, writable, std::move(relation_descriptor), policy_rows,
          &material, &material_diagnostic)) {
    abandon_reservation();
    return material_diagnostic;
  }

  std::vector<std::pair<std::string, std::string>> complete_fields;
  complete_fields.reserve(material.sealed_set.descriptor_fields.size());
  for (const auto& field : material.sealed_set.descriptor_fields) {
    complete_fields.emplace_back(
        std::string(field.key_raw_bytes.begin(), field.key_raw_bytes.end()),
        std::string(field.value_raw_bytes.begin(), field.value_raw_bytes.end()));
  }
  if (complete_fields.empty() ||
      complete_fields.size() !=
          material.sealed_set.descriptor_field_count ||
      complete_fields.back().first !=
          kMgaContextualTextSidecarSetSealKeyV2 ||
      complete_fields.back().second.size() !=
          material.sealed_set.seal_sha256.size()) {
    abandon_reservation();
    return ContextualTextMgaDiagnostic(
        "sealed descriptor vector header or final seal is invalid");
  }

  namespace stf = sealed_table_metadata_field_v2;
  std::vector<std::string> fields(stf::kFieldCount);
  fields[stf::kMagic] = kRowStoreMagic;
  fields[stf::kRecordKind] = std::string(kSealedTableMetadataKindV2);
  fields[stf::kCreatorTx] = std::to_string(writable.creator_tx);
  fields[stf::kEventSequence] = std::to_string(writable.event_sequence);
  fields[stf::kFormat] = std::string(kSealedTableMetadataFormatV2);
  fields[stf::kSealState] = "sealed";
  fields[stf::kTableUuid] = writable.table_uuid;
  fields[stf::kDefaultName] = EncodeCrudText(writable.default_name);
  fields[stf::kColumns] = EncodeCrudPairs(writable.columns);
  fields[stf::kTemporary] = writable.temporary ? "1" : "0";
  fields[stf::kTemporaryScope] = writable.temporary_scope;
  fields[stf::kTemporarySessionUuid] = writable.temporary_session_uuid;
  fields[stf::kOnCommitAction] = writable.on_commit_action;
  fields[stf::kRelationDescriptorUuid] =
      material.relation_descriptor.descriptor_uuid.canonical;
  fields[stf::kRelationDescriptorGeneration] =
      std::to_string(material.relation_descriptor.descriptor_generation);
  fields[stf::kDescriptorFieldCount] =
      std::to_string(material.sealed_set.descriptor_field_count);
  fields[stf::kDescriptorFieldBytes] =
      std::to_string(material.sealed_set.descriptor_field_bytes);
  fields[stf::kContextualSidecarCount] =
      std::to_string(material.sealed_set.contextual_sidecar_count);
  fields[stf::kDescriptorFields] = EncodeCrudPairs(complete_fields);
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    abandon_reservation();
    return MakeInvalidRequestDiagnostic(
        kOperation, "sealed_table_descriptor_append_failed");
  }
  // The sealed line is the atomic recovery authority. The allocator record is
  // acceleration only and is published after that one visibility barrier.
  (void)AppendDeferredEventSequenceAllocatorLines(
      context, &allocator_lines, nullptr);
  *descriptor = std::move(material.relation_descriptor);
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaConstraintMutationBatch(
    const EngineRequestContext& context,
    const MgaConstraintMutationBatch& batch) {
  constexpr const char* kOperation = "mga.constraint_mutation_batch";
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthorityForStoreModule(
      context, kOperation);
  if (authority.error) return authority;
  if (batch.format_version != "neutral_fk_mutation_batch_v1" ||
      // The caller supplies the complete semantics, never the seal.  The
      // engine reserves the MGA event and hashes the final record below.
      !ValidConstraintBatchUuid(
          batch.batch_uuid,
          scratchbird::core::platform::UuidKind::row) ||
      !batch.batch_hash.empty() || batch.mutation_count != 1 ||
      !ValidConstraintBatchUuid(
          batch.database_uuid,
          scratchbird::core::platform::UuidKind::database) ||
      !ValidConstraintBatchUuid(
          batch.constraint_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.database_uuid != context.database_uuid.canonical ||
      !ValidConstraintBatchUuid(
          batch.owner_table_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.child_schema_uuid,
          scratchbird::core::platform::UuidKind::schema) ||
      !ValidConstraintBatchUuid(
          batch.child_relation_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.child_relation_descriptor_generation == 0 ||
      !ValidConstraintBatchUuid(
          batch.child_column_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_table_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_schema_uuid,
          scratchbird::core::platform::UuidKind::schema) ||
      !ValidConstraintBatchUuid(
          batch.parent_relation_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.parent_relation_descriptor_generation == 0 ||
      !ValidConstraintBatchUuid(
          batch.parent_column_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_candidate_key_constraint_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.key_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.support_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.support_family != "btree" ||
      batch.support_policy != "required_exact_unique_index" ||
      batch.match_policy != "simple" ||
      batch.on_update_action != "no_action" ||
      batch.on_delete_action != "no_action" ||
      batch.enforcement_timing != "immediate" ||
      batch.constraint_metadata_generation == 0 ||
      batch.base_table_event_sequence == 0 ||
      batch.parent_base_table_event_sequence == 0 ||
      batch.constraint_kind != "foreign_key" ||
      batch.canonical_constraint_envelope.empty() ||
      batch.updated_table.table_uuid != batch.owner_table_uuid) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "complete_prevalidated_batch_required");
  }
  if (batch.updated_table.temporary ||
      !batch.updated_table.temporary_scope.empty() ||
      !batch.updated_table.temporary_session_uuid.empty() ||
      !batch.updated_table.on_commit_action.empty()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "temporary_constraint_mutation_batch_unsupported");
  }
  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) return current.diagnostic;
  const RelationReadSnapshot current_state = BuildCrudCompatibilityStateFromMga(
      current.state);
  const auto current_owner = FindVisibleCrudTable(
      current_state, batch.owner_table_uuid, context.local_transaction_id);
  if (!current_owner ||
      current_owner->event_sequence != batch.base_table_event_sequence) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "owner_metadata_event_changed_before_append");
  }
  const auto current_parent = FindVisibleCrudTable(
      current_state, batch.parent_table_uuid, context.local_transaction_id);
  if (!current_parent ||
      current_parent->event_sequence !=
          batch.parent_base_table_event_sequence) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_metadata_event_changed_before_append");
  }
  const CrudTableRecord& updated = batch.updated_table;
  if (updated.table_uuid != current_owner->table_uuid ||
      updated.default_name != current_owner->default_name ||
      updated.temporary != current_owner->temporary ||
      updated.temporary_scope != current_owner->temporary_scope ||
      updated.temporary_session_uuid !=
          current_owner->temporary_session_uuid ||
      updated.on_commit_action != current_owner->on_commit_action ||
      updated.columns.size() != current_owner->columns.size()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_table_projection_changed");
  }
  std::size_t changed_column_count = 0;
  const std::pair<std::string, std::string>* old_changed_column = nullptr;
  const std::pair<std::string, std::string>* new_changed_column = nullptr;
  for (std::size_t index = 0; index < updated.columns.size(); ++index) {
    const auto& old_column = current_owner->columns[index];
    const auto& new_column = updated.columns[index];
    if (old_column.first != new_column.first) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_column_order_or_name_changed");
    }
    if (old_column.second == new_column.second) continue;
    ++changed_column_count;
    old_changed_column = &old_column;
    new_changed_column = &new_column;
  }
  if (changed_column_count != batch.mutation_count ||
      changed_column_count != 1 || old_changed_column == nullptr ||
      new_changed_column == nullptr) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_mutation_count_mismatch");
  }
  const auto old_fields =
      StrictRelationDescriptorFields(old_changed_column->second);
  const auto new_fields =
      StrictRelationDescriptorFields(new_changed_column->second);
  const auto envelope_fields = StrictRelationDescriptorFields(
      batch.canonical_constraint_envelope);
  if (!old_fields || !new_fields || !envelope_fields) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_descriptor_encoding_invalid");
  }
  const std::set<std::string> fk_projection_fields = {
      "foreign_key",
      "constraint_uuid",
      "constraint_name",
      "constraint_class",
      "owner_object_uuid",
      "owner_object_name",
      "child_column_uuid",
      "referenced_table_uuid",
      "referenced_table_name",
      "referenced_column_uuid",
      "referenced_column",
      "key_descriptor_uuid",
      "referenced_key_descriptor_uuid",
      "referenced_candidate_key_constraint_uuid",
      "support_uuid",
      "referenced_support_uuid",
      "support_family",
      "on_update",
      "on_delete",
      "referential_action",
      "enforcement_timing",
      "deferrable",
      "constraint_mutation_batch_uuid",
      "constraint_mutation_batch_state"};
  for (const auto& [key, value] : *old_fields) {
    const auto found = new_fields->find(key);
    if (fk_projection_fields.find(key) != fk_projection_fields.end() ||
        found == new_fields->end() || found->second != value) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_changed_base_column_descriptor");
    }
  }
  for (const auto& [key, value] : *new_fields) {
    (void)value;
    if (old_fields->find(key) == old_fields->end() &&
        fk_projection_fields.find(key) == fk_projection_fields.end()) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_added_unrelated_column_field");
    }
  }
  auto require_field = [&](const std::map<std::string, std::string>& fields,
                           const char* key,
                           const std::string& expected) {
    const auto found = fields.find(key);
    return found != fields.end() && found->second == expected;
  };
  if (!require_field(*new_fields, "foreign_key", "true") ||
      !require_field(*new_fields, "constraint_uuid", batch.constraint_uuid) ||
      !require_field(*new_fields, "constraint_name", batch.constraint_name) ||
      !require_field(*new_fields, "constraint_class", "foreign_key") ||
      !require_field(*new_fields, "owner_object_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*new_fields, "owner_object_name",
                     current_owner->default_name) ||
      !require_field(*new_fields, "child_column_uuid",
                     batch.child_column_uuid) ||
      !require_field(*new_fields, "referenced_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*new_fields, "referenced_table_name",
                     current_parent->default_name) ||
      !require_field(*new_fields, "referenced_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*new_fields, "key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*new_fields, "referenced_key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*new_fields,
                     "referenced_candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*new_fields, "support_uuid", batch.support_uuid) ||
      !require_field(*new_fields, "referenced_support_uuid",
                     batch.support_uuid) ||
      !require_field(*new_fields, "support_family",
                     batch.support_family) ||
      !require_field(*new_fields, "on_update", batch.on_update_action) ||
      !require_field(*new_fields, "on_delete", batch.on_delete_action) ||
      !require_field(*new_fields, "referential_action", "no_action") ||
      !require_field(*new_fields, "enforcement_timing",
                     batch.enforcement_timing) ||
      !require_field(*new_fields, "deferrable", "false") ||
      !require_field(*new_fields, "constraint_mutation_batch_uuid",
                     batch.batch_uuid) ||
      !require_field(*new_fields, "constraint_mutation_batch_state",
                     "sealed")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_column_projection_incoherent");
  }
  const std::set<std::string> final_envelope_fields = {
      "descriptor_version",
      "child_table_uuid",
      "child_column_uuid",
      "child_relation_descriptor_uuid",
      "child_relation_descriptor_generation",
      "parent_table_uuid",
      "parent_column_uuid",
      "parent_relation_descriptor_uuid",
      "parent_relation_descriptor_generation",
      "referenced_table_uuid",
      "referenced_column_uuid",
      "referenced_column",
      "child_column",
      "constraint_name_quoted",
      "on_update",
      "on_delete",
      "referential_action",
      "enforcement_timing",
      "deferrable",
      "constraint_uuid",
      "constraint_name",
      "owner_object_uuid",
      "key_descriptor_uuid",
      "referenced_candidate_key_constraint_uuid",
      "support_uuid",
      "support_family",
      "constraint_mutation_batch_uuid",
      "constraint_mutation_batch_state"};
  std::set<std::string> actual_envelope_fields;
  for (const auto& [key, value] : *envelope_fields) {
    (void)value;
    actual_envelope_fields.insert(key);
  }
  if (actual_envelope_fields != final_envelope_fields ||
      !require_field(*envelope_fields, "descriptor_version",
                     "neutral_fk_single_column_v1") ||
      !require_field(*envelope_fields, "child_table_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*envelope_fields, "child_column_uuid",
                     batch.child_column_uuid) ||
      !require_field(*envelope_fields, "child_relation_descriptor_uuid",
                     batch.child_relation_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "child_relation_descriptor_generation",
                     std::to_string(
                         batch.child_relation_descriptor_generation)) ||
      !require_field(*envelope_fields, "parent_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*envelope_fields, "parent_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*envelope_fields, "parent_relation_descriptor_uuid",
                     batch.parent_relation_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "parent_relation_descriptor_generation",
                     std::to_string(
                         batch.parent_relation_descriptor_generation)) ||
      !require_field(*envelope_fields, "referenced_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*envelope_fields, "referenced_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*envelope_fields, "constraint_uuid",
                     batch.constraint_uuid) ||
      !require_field(*envelope_fields, "constraint_name",
                     batch.constraint_name) ||
      !require_field(*envelope_fields, "owner_object_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*envelope_fields, "key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "referenced_candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*envelope_fields, "support_uuid", batch.support_uuid) ||
      !require_field(*envelope_fields, "support_family",
                     batch.support_family) ||
      !require_field(*envelope_fields, "on_update",
                     batch.on_update_action) ||
      !require_field(*envelope_fields, "on_delete",
                     batch.on_delete_action) ||
      !require_field(*envelope_fields, "referential_action", "no_action") ||
      !require_field(*envelope_fields, "enforcement_timing",
                     batch.enforcement_timing) ||
      !require_field(*envelope_fields, "deferrable", "false") ||
      !require_field(*envelope_fields, "constraint_mutation_batch_uuid",
                     batch.batch_uuid) ||
      !require_field(*envelope_fields, "constraint_mutation_batch_state",
                     "sealed") ||
      !require_field(*new_fields, "referenced_column",
                     RelationDescriptorFieldOrEmpty(
                         *envelope_fields, {"referenced_column"}))) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_canonical_envelope_incoherent");
  }
  const auto child_storage = LoadMgaRelationStorageDescriptor(
      context, batch.owner_table_uuid);
  const auto parent_storage = LoadMgaRelationStorageDescriptor(
      context, batch.parent_table_uuid);
  if (!child_storage.ok) return child_storage.diagnostic;
  if (!parent_storage.ok) return parent_storage.diagnostic;
  const auto& child_relation = child_storage.descriptor;
  const auto& parent_relation = parent_storage.descriptor;
  if (child_relation.database_uuid.canonical != batch.database_uuid ||
      child_relation.relation_uuid.canonical != batch.owner_table_uuid ||
      child_relation.schema_uuid.canonical != batch.child_schema_uuid ||
      child_relation.descriptor_uuid.canonical !=
          batch.child_relation_descriptor_uuid ||
      child_relation.descriptor_generation !=
          batch.child_relation_descriptor_generation ||
      parent_relation.database_uuid.canonical != batch.database_uuid ||
      parent_relation.relation_uuid.canonical != batch.parent_table_uuid ||
      parent_relation.schema_uuid.canonical != batch.parent_schema_uuid ||
      parent_relation.descriptor_uuid.canonical !=
          batch.parent_relation_descriptor_uuid ||
      parent_relation.descriptor_generation !=
          batch.parent_relation_descriptor_generation) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_relation_descriptor_binding_changed");
  }
  const auto child_column = std::find_if(
      child_relation.columns.begin(), child_relation.columns.end(),
      [&](const MgaRelationColumnStorageDescriptor& column) {
        return column.column_uuid.canonical == batch.child_column_uuid;
      });
  const auto parent_column = std::find_if(
      parent_relation.columns.begin(), parent_relation.columns.end(),
      [&](const MgaRelationColumnStorageDescriptor& column) {
        return column.column_uuid.canonical == batch.parent_column_uuid;
      });
  const std::string quoted = RelationDescriptorFieldOrEmpty(
      *envelope_fields, {"constraint_name_quoted"});
  if (child_column == child_relation.columns.end() ||
      parent_column == parent_relation.columns.end() ||
      child_column->canonical_name_key != new_changed_column->first ||
      !require_field(*envelope_fields, "child_column",
                     child_column->canonical_name_key) ||
      !require_field(*envelope_fields, "referenced_column",
                     parent_column->canonical_name_key) ||
      !require_field(*new_fields, "referenced_column",
                     parent_column->canonical_name_key) ||
      (quoted != "true" && quoted != "false")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_column_descriptor_binding_changed");
  }
  const auto parent_metadata_column = std::find_if(
      current_parent->columns.begin(), current_parent->columns.end(),
      [&](const auto& column) {
        return column.first == parent_column->canonical_name_key;
      });
  if (parent_metadata_column == current_parent->columns.end()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_column_not_visible");
  }
  const auto parent_key_fields =
      StrictRelationDescriptorFields(parent_metadata_column->second);
  if (!parent_key_fields ||
      !require_field(*parent_key_fields,
                     "candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*parent_key_fields,
                     "candidate_key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*parent_key_fields, "support_uuid",
                     batch.support_uuid) ||
      !require_field(*parent_key_fields, "support_family", "btree") ||
      (RelationDescriptorFieldOrEmpty(
           *parent_key_fields, {"candidate_key_class"}) != "primary_key" &&
       RelationDescriptorFieldOrEmpty(
           *parent_key_fields, {"candidate_key_class"}) != "unique")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_projection_changed");
  }
  const auto descriptor_support = std::find_if(
      parent_relation.indexes.begin(), parent_relation.indexes.end(),
      [&](const MgaRelationIndexStorageDescriptor& index) {
        return index.index_uuid.canonical == batch.support_uuid &&
               index.unique && index.family == "btree";
      });
  auto key_columns = [](const CrudIndexRecord& index) {
    std::vector<std::string> columns;
    for (const std::string& envelope : index.key_envelopes) {
      if (envelope.empty() || envelope == "unique" ||
          envelope == "primary_key" || envelope.starts_with("include:") ||
          envelope.starts_with("where_eq:") ||
          envelope.starts_with("where_mod_eq:") ||
          envelope == "where_true") {
        continue;
      }
      if (envelope.starts_with("identity:")) {
        columns.push_back(envelope.substr(9));
      } else if (envelope.starts_with("desc:")) {
        columns.push_back(envelope.substr(5));
      } else if (envelope.starts_with("cast:")) {
        const std::string rest = envelope.substr(5);
        const auto separator = rest.find(':');
        columns.push_back(separator == std::string::npos
                              ? rest
                              : rest.substr(0, separator));
      } else {
        columns.push_back(envelope);
      }
    }
    if (columns.empty() && !index.column_name.empty()) {
      columns.push_back(index.column_name);
    }
    return columns;
  };
  std::size_t exact_support_count = 0;
  for (const auto& index : VisibleCrudIndexesForTable(
           current_state,
           current_parent->table_uuid,
           context.local_transaction_id)) {
    const auto columns = key_columns(index);
    const std::string visible_support_family =
        index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                             : index.family;
    if (index.index_uuid == batch.support_uuid && index.unique &&
        visible_support_family == batch.support_family &&
        columns.size() == 1 &&
        columns.front() == parent_column->canonical_name_key) {
      ++exact_support_count;
    }
  }
  if (descriptor_support == parent_relation.indexes.end() ||
      exact_support_count != 1) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_exact_support_changed");
  }
  // D1 admits one immediate single-column FK per child table.  Enforce that
  // bounded generation model in the storage authority as well as the DDL
  // adapter so generation 1 can never silently duplicate.
  if (batch.constraint_metadata_generation != 1) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "bounded_d1_constraint_generation_must_be_one");
  }
  for (const auto& [column_name, descriptor] : current_owner->columns) {
    (void)column_name;
    if (descriptor.find("constraint_mutation_batch_state=sealed") !=
        std::string::npos) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "bounded_d1_prior_constraint_batch_unsupported");
    }
  }
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) return reservation.diagnostic;

  CrudTableRecord table = batch.updated_table;
  table.creator_tx = context.local_transaction_id;
  table.event_sequence = reservation.first;
  MgaConstraintMutationBatch sealed_batch = batch;
  sealed_batch.updated_table = table;
  sealed_batch.batch_hash = ComputeMgaConstraintMutationBatchHash(
      sealed_batch, table.creator_tx, table.event_sequence);
  if (sealed_batch.batch_hash.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "batch_hash_generation_failed");
  }
  // `sealed` is emitted here, after all required catalog and relation fields
  // have passed validation, and is never copied from an SBLR operand.
  const auto line_fields = ConstraintMutationBatchLineFields(
      sealed_batch, table.creator_tx, table.event_sequence);
  if (line_fields.size() != ConstraintMutationBatchFieldCount()) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "batch_codec_field_count_invalid");
  }
  const std::string line = JoinLine(line_fields);
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "sealed_batch_append_failed");
  }
  return OkDiagnostic();
}

MgaBigintIdentityMigrationResult AppendMgaBigintIdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaBigintIdentityMigrationRequest& request) {
  constexpr const char* kOperation = "mga.bigint_identity_migration";
  MgaBigintIdentityMigrationResult result;
  auto refuse = [&](std::string code, std::string key, std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        std::move(code), std::move(key), std::move(detail));
    return result;
  };
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_context_invalid",
                  "active MGA transaction and database path required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthorityForStoreModule(
      context, kOperation);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  if (request.migration_id != kBigintMigrationId || request.rows.empty() ||
      request.prior_catalog_snapshot_uuid.empty() ||
      request.new_catalog_snapshot_uuid.empty() ||
      request.prior_catalog_snapshot_uuid == request.new_catalog_snapshot_uuid ||
      request.prior_catalog_generation == 0 ||
      request.new_catalog_generation != request.prior_catalog_generation + 1 ||
      (!context.statement_metadata_snapshot_uuid.canonical.empty() &&
       context.statement_metadata_snapshot_uuid.canonical !=
           request.prior_catalog_snapshot_uuid)) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_snapshot_stale",
                  "exact prior snapshot and consecutive catalog generation required");
  }

  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) {
    result.diagnostic = current.diagnostic;
    return result;
  }
  std::set<std::pair<std::string, std::string>> identities;
  std::set<std::string> objects;
  std::vector<CrudTableRecord> tables;
  tables.reserve(request.rows.size());
  for (const auto& requested : request.rows) {
    if (requested.object_uuid.empty() || requested.column_uuid.empty() ||
        requested.old_row_generation == 0 ||
        !identities.emplace(requested.object_uuid,
                            requested.column_uuid).second ||
        !objects.emplace(requested.object_uuid).second) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "bigint_identity_migration_multiple_mapping",
                    "each object and column identity must occur exactly once");
    }
    const CrudTableRecord* exact = nullptr;
    std::uint64_t newest_visible_generation = 0;
    for (const auto& table : current.state.relation_metadata.tables) {
      if (table.table_uuid != requested.object_uuid ||
          !CrudCreatorVisible(current.state.relation_metadata,
                              table.creator_tx,
                              table.event_sequence,
                              context.local_transaction_id)) {
        continue;
      }
      newest_visible_generation =
          std::max(newest_visible_generation, table.event_sequence);
      if (table.event_sequence != requested.old_row_generation) continue;
      if (exact != nullptr) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "bigint_identity_migration_multiple_mapping",
                      "multiple visible rows share the expected generation");
      }
      exact = &table;
    }
    if (exact == nullptr ||
        newest_visible_generation != requested.old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "bigint_identity_migration_row_generation_stale",
                    requested.object_uuid);
    }
    CrudTableRecord updated = *exact;
    if (updated.temporary || !updated.temporary_scope.empty() ||
        !updated.temporary_session_uuid.empty() ||
        !updated.on_commit_action.empty()) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "bigint_identity_migration_temporary_unsupported",
                    requested.object_uuid);
    }
    std::size_t matched_columns = 0;
    for (auto& [column_name, descriptor] : updated.columns) {
      (void)column_name;
      const auto descriptor_fields = StrictRelationDescriptorFields(descriptor);
      if (!descriptor_fields) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "bigint_identity_migration_descriptor_contradiction",
                      requested.object_uuid);
      }
      const auto column = descriptor_fields->find("column_uuid");
      if (column == descriptor_fields->end() ||
          column->second != requested.column_uuid) {
        continue;
      }
      ++matched_columns;
      const auto type = descriptor_fields->find("type_uuid");
      if (type == descriptor_fields->end() ||
          type->second != kLegacyBigintTypeUuid) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "bigint_identity_migration_legacy_identity_required",
                      requested.column_uuid);
      }
      const auto first = descriptor.find(kLegacyBigintTypeUuid);
      if (first == std::string::npos ||
          descriptor.find(kLegacyBigintTypeUuid,
                          first + kLegacyBigintTypeUuid.size()) !=
              std::string::npos) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "bigint_identity_migration_multiple_mapping",
                      requested.column_uuid);
      }
      descriptor.replace(first, kLegacyBigintTypeUuid.size(),
                         kCanonicalBigintTypeUuid);
    }
    if (matched_columns != 1) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "bigint_identity_migration_column_mapping_conflict",
                    requested.column_uuid);
    }
    tables.push_back(std::move(updated));
  }

  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) {
    result.diagnostic = reservation.diagnostic;
    return result;
  }
  for (std::size_t i = 0; i < tables.size(); ++i) {
    if (reservation.first <= request.rows[i].old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "bigint_identity_migration_generation_not_advanced",
                    request.rows[i].object_uuid);
    }
    tables[i].creator_tx = context.local_transaction_id;
    tables[i].event_sequence = reservation.first;
  }
  std::vector<std::string> decisions;
  decisions.reserve(request.rows.size());
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    decisions.push_back(BigintMigrationDecisionHash(
        request, request.rows[i], tables[i].event_sequence,
        context.transaction_uuid.canonical));
    if (decisions.back().empty()) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "bigint_identity_migration_hash_failed", "sha256");
    }
  }
  const std::string payload = CanonicalBigintMigrationPayload(
      request, context.local_transaction_id, reservation.first,
      context.transaction_uuid.canonical, tables,
      decisions);
  result.decision_sha256 = Sha256Tagged(payload);
  if (result.decision_sha256.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_hash_failed", "batch");
  }
  std::vector<std::string> fields{
      kRowStoreMagic,
      "BIGINT_IDENTITY_MIGRATION_BATCH",
      std::to_string(context.local_transaction_id),
      std::to_string(reservation.first),
      std::string(kBigintMigrationFormat),
      "sealed",
      result.decision_sha256,
      request.migration_id,
      context.transaction_uuid.canonical,
      request.prior_catalog_snapshot_uuid,
      request.new_catalog_snapshot_uuid,
      std::to_string(request.prior_catalog_generation),
      std::to_string(request.new_catalog_generation),
      std::to_string(request.rows.size())};
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    fields.insert(fields.end(), {
        row.object_uuid,
        row.column_uuid,
        std::string(kLegacyBigintTypeUuid),
        std::string(kCanonicalBigintTypeUuid),
        std::to_string(row.old_row_generation),
        std::to_string(table.event_sequence),
        decisions[i],
        EncodeCrudText(table.default_name),
        EncodeCrudPairs(table.columns),
        "0", "", "", ""});
  }
  // Publication is this single append. MGA visibility subsequently admits it
  // only for its creator or after the owning transaction commits.
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "bigint_identity_migration_append_failed",
                  "sealed batch was not published");
  }
  result.ok = true;
  result.migrated_row_count = request.rows.size();
  result.diagnostic = OkDiagnostic();
  result.evidence = {
      {"migration_id", request.migration_id},
      {"transaction_uuid", context.transaction_uuid.canonical},
      {"prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid},
      {"new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid},
      {"prior_catalog_generation", std::to_string(request.prior_catalog_generation)},
      {"new_catalog_generation", std::to_string(request.new_catalog_generation)},
      {"decision_sha256", result.decision_sha256}};
  return result;
}

MgaInt32IdentityMigrationResult AppendMgaInt32IdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaInt32IdentityMigrationRequest& request) {
  constexpr const char* kOperation = "mga.int32_identity_migration";
  MgaInt32IdentityMigrationResult result;
  auto refuse = [&](std::string code, std::string key, std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        std::move(code), std::move(key), std::move(detail));
    return result;
  };
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_context_invalid",
                  "active MGA transaction and database path required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthorityForStoreModule(
      context, kOperation);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  if (request.migration_id != kInt32MigrationId || request.rows.empty() ||
      request.prior_catalog_snapshot_uuid.empty() ||
      request.new_catalog_snapshot_uuid.empty() ||
      request.prior_catalog_snapshot_uuid == request.new_catalog_snapshot_uuid ||
      request.prior_catalog_generation == 0 ||
      request.new_catalog_generation != request.prior_catalog_generation + 1 ||
      (!context.statement_metadata_snapshot_uuid.canonical.empty() &&
       context.statement_metadata_snapshot_uuid.canonical !=
           request.prior_catalog_snapshot_uuid)) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_snapshot_stale",
                  "exact prior snapshot and consecutive catalog generation required");
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "int32_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }

  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) {
    result.diagnostic = current.diagnostic;
    return result;
  }
  std::set<std::pair<std::string, std::string>> identities;
  std::map<std::string, std::uint64_t> object_generations;
  std::map<std::string, CrudTableRecord> updated_by_object;
  for (const auto& requested : request.rows) {
    if (requested.object_uuid.empty() || requested.column_uuid.empty() ||
        requested.old_row_generation == 0 ||
        !identities.emplace(requested.object_uuid,
                            requested.column_uuid).second) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "int32_identity_migration_multiple_mapping",
                    "each object and column identity must occur exactly once");
    }
    const auto object_generation = object_generations.find(
        requested.object_uuid);
    if (object_generation != object_generations.end() &&
        object_generation->second != requested.old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "int32_identity_migration_row_generation_stale",
                    requested.object_uuid);
    }
    object_generations[requested.object_uuid] = requested.old_row_generation;

    auto updated = updated_by_object.find(requested.object_uuid);
    if (updated == updated_by_object.end()) {
      const CrudTableRecord* exact = nullptr;
      std::uint64_t newest_visible_generation = 0;
      for (const auto& table : current.state.relation_metadata.tables) {
        if (table.table_uuid != requested.object_uuid ||
            !CrudCreatorVisible(current.state.relation_metadata,
                                table.creator_tx,
                                table.event_sequence,
                                context.local_transaction_id)) {
          continue;
        }
        newest_visible_generation =
            std::max(newest_visible_generation, table.event_sequence);
        if (table.event_sequence != requested.old_row_generation) continue;
        if (exact != nullptr) {
          return refuse("CORE.AUTHORITY.CONFLICT",
                        "int32_identity_migration_multiple_mapping",
                        "multiple visible rows share the expected generation");
        }
        exact = &table;
      }
      if (exact == nullptr ||
          newest_visible_generation != requested.old_row_generation) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "int32_identity_migration_row_generation_stale",
                      requested.object_uuid);
      }
      if (exact->temporary || !exact->temporary_scope.empty() ||
          !exact->temporary_session_uuid.empty() ||
          !exact->on_commit_action.empty()) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "int32_identity_migration_temporary_unsupported",
                      requested.object_uuid);
      }
      updated = updated_by_object.emplace(requested.object_uuid, *exact).first;
    }

    std::size_t matched_columns = 0;
    for (auto& [column_name, descriptor] : updated->second.columns) {
      (void)column_name;
      const auto descriptor_fields = StrictRelationDescriptorFields(descriptor);
      if (!descriptor_fields) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "int32_identity_migration_descriptor_contradiction",
                      requested.object_uuid);
      }
      const auto column = descriptor_fields->find("column_uuid");
      if (column == descriptor_fields->end() ||
          column->second != requested.column_uuid) {
        continue;
      }
      ++matched_columns;
      const auto descriptor_uuid =
          descriptor_fields->find("datatype_descriptor_uuid");
      const auto type_uuid = descriptor_fields->find("type_uuid");
      if (descriptor_uuid == descriptor_fields->end() ||
          type_uuid == descriptor_fields->end() ||
          descriptor_uuid->second != kLegacyInt32DescriptorUuid ||
          type_uuid->second != kLegacyInt32TypeUuid) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "int32_identity_migration_legacy_identity_required",
                      requested.column_uuid);
      }
      const std::map<std::string,
                     std::pair<std::string_view, std::string_view>> replacements{
          {"datatype_descriptor_uuid",
           {kLegacyInt32DescriptorUuid, kCanonicalInt32DescriptorUuid}},
          {"type_uuid", {kLegacyInt32TypeUuid, kCanonicalInt32TypeUuid}}};
      if (!ReplaceExactRelationDescriptorIdentities(&descriptor,
                                                     replacements)) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "int32_identity_migration_multiple_mapping",
                      requested.column_uuid);
      }
    }
    if (matched_columns != 1) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "int32_identity_migration_column_mapping_conflict",
                    requested.column_uuid);
    }
  }

  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "int32_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }
  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) {
    result.diagnostic = reservation.diagnostic;
    return result;
  }
  for (auto& [object_uuid, table] : updated_by_object) {
    if (reservation.first <= object_generations[object_uuid]) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "int32_identity_migration_generation_not_advanced",
                    object_uuid);
    }
    table.creator_tx = context.local_transaction_id;
    table.event_sequence = reservation.first;
  }
  std::vector<CrudTableRecord> tables;
  tables.reserve(request.rows.size());
  for (const auto& row : request.rows) {
    tables.push_back(updated_by_object.at(row.object_uuid));
  }
  std::vector<std::string> decisions;
  decisions.reserve(request.rows.size());
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    decisions.push_back(Int32MigrationDecisionHash(
        request, request.rows[i], tables[i].event_sequence,
        context.transaction_uuid.canonical));
    if (decisions.back().empty()) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "int32_identity_migration_hash_failed", "sha256");
    }
  }
  const std::string payload = CanonicalInt32MigrationPayload(
      request, context.local_transaction_id, reservation.first,
      context.transaction_uuid.canonical, tables, decisions);
  result.decision_sha256 = Sha256Tagged(payload);
  if (result.decision_sha256.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_hash_failed", "batch");
  }
  std::vector<std::string> fields{
      kRowStoreMagic,
      "INT32_IDENTITY_MIGRATION_BATCH",
      std::to_string(context.local_transaction_id),
      std::to_string(reservation.first),
      std::string(kInt32MigrationFormat),
      "sealed",
      result.decision_sha256,
      request.migration_id,
      context.transaction_uuid.canonical,
      request.prior_catalog_snapshot_uuid,
      request.new_catalog_snapshot_uuid,
      std::to_string(request.prior_catalog_generation),
      std::to_string(request.new_catalog_generation),
      std::to_string(request.rows.size())};
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    fields.insert(fields.end(), {
        row.object_uuid,
        row.column_uuid,
        std::string(kLegacyInt32DescriptorUuid),
        std::string(kCanonicalInt32DescriptorUuid),
        std::string(kLegacyInt32TypeUuid),
        std::string(kCanonicalInt32TypeUuid),
        std::to_string(row.old_row_generation),
        std::to_string(table.event_sequence),
        decisions[i],
        EncodeCrudText(table.default_name),
        EncodeCrudPairs(table.columns),
        "0", "", "", ""});
  }
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "int32_identity_migration_append_failed",
                  "sealed batch was not published");
  }
  result.ok = true;
  result.migrated_row_count = request.rows.size();
  result.diagnostic = OkDiagnostic();
  result.evidence = {
      {"migration_id", request.migration_id},
      {"transaction_uuid", context.transaction_uuid.canonical},
      {"prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid},
      {"new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid},
      {"prior_catalog_generation",
       std::to_string(request.prior_catalog_generation)},
      {"new_catalog_generation",
       std::to_string(request.new_catalog_generation)},
      {"old_descriptor_uuid", std::string(kLegacyInt32DescriptorUuid)},
      {"new_descriptor_uuid", std::string(kCanonicalInt32DescriptorUuid)},
      {"old_type_uuid", std::string(kLegacyInt32TypeUuid)},
      {"new_type_uuid", std::string(kCanonicalInt32TypeUuid)},
      {"decision_sha256", result.decision_sha256}};
  return result;
}

MgaTextIdentityMigrationResult AppendMgaTextIdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaTextIdentityMigrationRequest& request) {
  constexpr const char* kOperation = "mga.text_identity_migration";
  MgaTextIdentityMigrationResult result;
  auto refuse = [&](std::string code, std::string key, std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(
        std::move(code), std::move(key), std::move(detail));
    return result;
  };
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_context_invalid",
                  "active MGA transaction and database path required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthorityForStoreModule(
      context, kOperation);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  if (!ExactCanonicalTextIdentityAuthorityAvailable(context) ||
      request.migration_id != kTextMigrationId || request.rows.empty() ||
      !CanonicalNonNilMigrationUuid(request.prior_catalog_snapshot_uuid) ||
      !CanonicalNonNilMigrationUuid(request.new_catalog_snapshot_uuid) ||
      request.prior_catalog_snapshot_uuid == request.new_catalog_snapshot_uuid ||
      request.prior_catalog_generation == 0 ||
      request.prior_catalog_generation ==
          std::numeric_limits<std::uint64_t>::max() ||
      request.new_catalog_generation != request.prior_catalog_generation + 1 ||
      context.catalog_generation_id != request.prior_catalog_generation ||
      context.statement_metadata_snapshot_uuid.canonical !=
          request.prior_catalog_snapshot_uuid) {
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_snapshot_stale",
                  "exact prior snapshot, registry row, and consecutive catalog generation required");
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "text_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }

  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) {
    result.diagnostic = current.diagnostic;
    return result;
  }
  std::set<std::pair<std::string, std::string>> identities;
  std::map<std::string, std::uint64_t> object_generations;
  std::map<std::string, CrudTableRecord> updated_by_object;
  std::map<std::string, MgaRelationStorageDescriptor>
      updated_descriptors_by_object;
  std::map<std::string, std::set<std::string>> changed_columns_by_object;
  // Validate the complete mapping before transforming any descriptor.  This
  // keeps duplicate/stale authority precedence independent of row order and
  // prevents a malformed first row from masking a later duplicate identity.
  for (const auto& requested : request.rows) {
    const auto identity =
        std::make_pair(requested.object_uuid, requested.column_uuid);
    if (!CanonicalNonNilMigrationUuid(requested.object_uuid) ||
        !CanonicalNonNilMigrationUuid(requested.column_uuid) ||
        requested.old_row_generation == 0 ||
        identities.contains(identity)) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "text_identity_migration_multiple_mapping",
                    "each object and column identity must occur exactly once");
    }
    identities.insert(identity);
    const auto object_generation = object_generations.find(
        requested.object_uuid);
    if (object_generation != object_generations.end() &&
        object_generation->second != requested.old_row_generation) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_row_generation_stale",
                    requested.object_uuid);
    }
    object_generations[requested.object_uuid] = requested.old_row_generation;
  }

  for (const auto& requested : request.rows) {
    auto updated = updated_by_object.find(requested.object_uuid);
    if (updated == updated_by_object.end()) {
      const CrudTableRecord* exact = nullptr;
      std::uint64_t newest_visible_generation = 0;
      for (const auto& table : current.state.relation_metadata.tables) {
        if (table.table_uuid != requested.object_uuid ||
            !CrudCreatorVisible(current.state.relation_metadata,
                                table.creator_tx,
                                table.event_sequence,
                                context.local_transaction_id)) {
          continue;
        }
        newest_visible_generation =
            std::max(newest_visible_generation, table.event_sequence);
        if (table.event_sequence != requested.old_row_generation) continue;
        if (exact != nullptr) {
          return refuse("CORE.AUTHORITY.CONFLICT",
                        "text_identity_migration_multiple_mapping",
                        "multiple visible rows share the expected generation");
        }
        exact = &table;
      }
      if (exact == nullptr ||
          newest_visible_generation != requested.old_row_generation) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "text_identity_migration_row_generation_stale",
                      requested.object_uuid);
      }
      if (exact->temporary || !exact->temporary_scope.empty() ||
          !exact->temporary_session_uuid.empty() ||
          !exact->on_commit_action.empty()) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "text_identity_migration_temporary_unsupported",
                      requested.object_uuid);
      }
      const auto loaded_descriptor = LoadMgaRelationStorageDescriptor(
          context, requested.object_uuid);
      if (!loaded_descriptor.ok ||
          loaded_descriptor.descriptor.database_uuid.canonical !=
              context.database_uuid.canonical ||
          loaded_descriptor.descriptor.relation_uuid.canonical !=
              requested.object_uuid ||
          loaded_descriptor.descriptor.relation_generation !=
              requested.old_row_generation) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "text_identity_migration_relation_snapshot_stale",
                      requested.object_uuid);
      }
      updated = updated_by_object.emplace(requested.object_uuid, *exact).first;
      updated_descriptors_by_object.emplace(
          requested.object_uuid, loaded_descriptor.descriptor);
    }

    auto& storage = updated_descriptors_by_object.at(requested.object_uuid);
    auto storage_column = storage.columns.end();
    std::size_t storage_matches = 0;
    for (auto candidate = storage.columns.begin();
         candidate != storage.columns.end(); ++candidate) {
      if (candidate->column_uuid.canonical != requested.column_uuid) continue;
      ++storage_matches;
      storage_column = candidate;
    }
    if (storage_matches != 1 || storage_column == storage.columns.end() ||
        storage_column->canonical_name_key.empty() ||
        storage_column->column_generation != requested.old_row_generation ||
        storage_column->value_descriptor.descriptor_uuid.canonical !=
            requested.column_uuid) {
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_relation_column_stale",
                    requested.column_uuid);
    }

    std::size_t matched_columns = 0;
    for (auto& [column_name, descriptor] : updated->second.columns) {
      if (column_name != storage_column->canonical_name_key) continue;
      ++matched_columns;
      auto migrated_storage_descriptor =
          storage_column->value_descriptor.encoded_descriptor;
      if (!RewriteLegacyTextDescriptor(context, &descriptor,
                                       requested.column_uuid) ||
          !RewriteLegacyTextDescriptor(context, &migrated_storage_descriptor,
                                       requested.column_uuid) ||
          descriptor != migrated_storage_descriptor) {
        return refuse("DATATYPE.DESCRIPTOR_INVALID",
                      "text_identity_migration_legacy_identity_required",
                      requested.column_uuid);
      }
      const auto migrated_fields = StrictRelationDescriptorFields(descriptor);
      if (!migrated_fields ||
          migrated_fields->at("nullable") !=
              (storage_column->nullable ? "true" : "false")) {
        return refuse("CORE.AUTHORITY.CONFLICT",
                      "text_identity_migration_nullability_conflict",
                      requested.column_uuid);
      }
      storage_column->value_descriptor.descriptor_uuid.canonical =
          requested.column_uuid;
      storage_column->value_descriptor.canonical_type_name = "text";
      storage_column->value_descriptor.encoded_descriptor = descriptor;
      changed_columns_by_object[requested.object_uuid].insert(
          requested.column_uuid);
    }
    if (matched_columns != 1) {
      return refuse("CORE.AUTHORITY.CONFLICT",
                    "text_identity_migration_column_mapping_conflict",
                    requested.column_uuid);
    }
  }

  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "text_identity_migration_cancelled_before_publication",
                  "cancellation was observed before the sealed batch append");
  }
  const bool requires_contextual_policy = std::ranges::any_of(
      updated_descriptors_by_object, [&context](const auto& entry) {
        return std::ranges::any_of(entry.second.columns,
                                   [&context](const auto& column) {
          return ExactCanonicalMigratedTextDescriptor(
                     context, column.value_descriptor.encoded_descriptor,
                     column.column_uuid.canonical) &&
                 !column.charset_uuid.empty() &&
                 !column.collation_uuid.empty();
        });
      });
  EngineContextualTextPolicyRowSetV2 policy_rows;
  if (requires_contextual_policy) {
    const auto policy =
        LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
    if (!policy.ok) {
      result.diagnostic = policy.diagnostic;
      return result;
    }
    policy_rows = policy.rows;
  }
  std::vector<std::string> allocator_lines;
  const auto reservation = ReserveEventSequenceRange(
      context, "relation_metadata", MetadataStorePath(context), 1,
      [&context]() { return ScanNextMetadataEventSequence(context); },
      &allocator_lines);
  if (!reservation.ok) {
    result.diagnostic = reservation.diagnostic;
    return result;
  }
  const auto abandon_reservation = [&]() {
    AbandonDeferredEventSequenceReservation(reservation);
    allocator_lines.clear();
  };
  std::map<std::string, CrudSealedRelationDescriptorSnapshot>
      sealed_descriptors_by_object;
  for (auto& [object_uuid, table] : updated_by_object) {
    if (reservation.first <= object_generations[object_uuid]) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_generation_not_advanced",
                    object_uuid);
    }
    table.creator_tx = context.local_transaction_id;
    table.event_sequence = reservation.first;
    auto& descriptor = updated_descriptors_by_object.at(object_uuid);
    descriptor.relation_generation = reservation.first;
    for (auto& column : descriptor.columns) {
      if (changed_columns_by_object[object_uuid].contains(
              column.column_uuid.canonical)) {
        column.column_generation = reservation.first;
      }
    }
    const auto descriptor_validation =
        ValidateMgaRelationStorageDescriptor(descriptor);
    if (descriptor_validation.error) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_relation_descriptor_invalid",
                    descriptor_validation.detail);
    }
    const auto serialized = SerializeMgaRelationStorageDescriptor(descriptor);
    if (DeserializeMgaRelationStorageDescriptor(serialized)
            .relation_generation != reservation.first) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_relation_descriptor_roundtrip_failed",
                    object_uuid);
    }
    MgaSealedContextualTextDescriptorMaterialV2 material;
    EngineApiDiagnostic material_diagnostic;
    if (!BuildMgaSealedContextualTextDescriptorMaterialV2(
            context, table, descriptor, policy_rows, &material,
            &material_diagnostic)) {
      abandon_reservation();
      return refuse("CTB.TEXT.DESCRIPTOR_INVALID",
                    "text_identity_migration_sidecar_set_invalid",
                    material_diagnostic.detail);
    }
    CrudSealedRelationDescriptorSnapshot snapshot;
    snapshot.creator_tx = table.creator_tx;
    snapshot.event_sequence = table.event_sequence;
    snapshot.relation_uuid = object_uuid;
    snapshot.relation_descriptor_uuid =
        material.relation_descriptor.descriptor_uuid.canonical;
    snapshot.relation_descriptor_generation =
        material.relation_descriptor.descriptor_generation;
    snapshot.descriptor_field_count =
        material.sealed_set.descriptor_field_count;
    snapshot.descriptor_field_bytes =
        material.sealed_set.descriptor_field_bytes;
    snapshot.contextual_sidecar_count =
        material.sealed_set.contextual_sidecar_count;
    snapshot.descriptor_fields.reserve(
        material.sealed_set.descriptor_fields.size());
    for (const auto& field : material.sealed_set.descriptor_fields) {
      snapshot.descriptor_fields.emplace_back(
          std::string(field.key_raw_bytes.begin(),
                      field.key_raw_bytes.end()),
          std::string(field.value_raw_bytes.begin(),
                      field.value_raw_bytes.end()));
    }
    sealed_descriptors_by_object.emplace(object_uuid, std::move(snapshot));
  }

  std::vector<CrudTableRecord> tables;
  std::vector<CrudSealedRelationDescriptorSnapshot>
      relation_descriptor_snapshots;
  tables.reserve(request.rows.size());
  relation_descriptor_snapshots.reserve(request.rows.size());
  for (const auto& row : request.rows) {
    tables.push_back(updated_by_object.at(row.object_uuid));
    relation_descriptor_snapshots.push_back(
        sealed_descriptors_by_object.at(row.object_uuid));
  }
  std::vector<std::string> decisions;
  decisions.reserve(request.rows.size());
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    decisions.push_back(TextMigrationDecisionHash(
        request, request.rows[i], tables[i].event_sequence,
        context.transaction_uuid.canonical,
        context.datatype_catalog_snapshot_uuid.canonical,
        context.datatype_catalog_generation,
        context.datatype_registry_generation,
        relation_descriptor_snapshots[i]));
    if (decisions.back().empty()) {
      abandon_reservation();
      return refuse("DATATYPE.DESCRIPTOR_INVALID",
                    "text_identity_migration_hash_failed", "sha256");
    }
  }
  const std::string payload = CanonicalTextMigrationPayload(
      request, context.local_transaction_id, reservation.first,
      context.transaction_uuid.canonical,
      context.datatype_catalog_snapshot_uuid.canonical,
      context.datatype_catalog_generation,
      context.datatype_registry_generation,
      tables, relation_descriptor_snapshots, decisions);
  result.decision_sha256 = Sha256Tagged(payload);
  if (result.decision_sha256.empty()) {
    abandon_reservation();
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_hash_failed", "batch");
  }
  std::vector<std::string> fields{
      kRowStoreMagic,
      "TEXT_IDENTITY_MIGRATION_BATCH",
      std::to_string(context.local_transaction_id),
      std::to_string(reservation.first),
      std::string(kTextMigrationFormat),
      "sealed",
      result.decision_sha256,
      request.migration_id,
      context.transaction_uuid.canonical,
      request.prior_catalog_snapshot_uuid,
      request.new_catalog_snapshot_uuid,
      std::to_string(request.prior_catalog_generation),
      std::to_string(request.new_catalog_generation),
      context.datatype_catalog_snapshot_uuid.canonical,
      std::to_string(context.datatype_catalog_generation),
      std::to_string(context.datatype_registry_generation),
      std::to_string(request.rows.size())};
  for (std::size_t i = 0; i < request.rows.size(); ++i) {
    const auto& row = request.rows[i];
    const auto& table = tables[i];
    const auto& snapshot = relation_descriptor_snapshots[i];
    fields.insert(fields.end(), {
        row.object_uuid,
        row.column_uuid,
        std::string(kLegacyTextDescriptorUuid),
        std::string(kCanonicalTextDescriptorUuid),
        std::string(kLegacyTextTypeUuid),
        std::string(kCanonicalTextTypeUuid),
        std::string(kCanonicalTextCodecUuid),
        std::string(kCanonicalTextCodecId),
        "1",
        "1",
        std::to_string(row.old_row_generation),
        std::to_string(table.event_sequence),
        decisions[i],
        EncodeCrudText(table.default_name),
        EncodeCrudPairs(table.columns),
        EncodeCrudPairs(snapshot.descriptor_fields),
        snapshot.relation_descriptor_uuid,
        std::to_string(snapshot.relation_descriptor_generation),
        std::to_string(snapshot.descriptor_field_count),
        std::to_string(snapshot.descriptor_field_bytes),
        std::to_string(snapshot.contextual_sidecar_count),
        "0", "", "", ""});
  }
  if (!AppendLine(MetadataStorePath(context), JoinLine(fields))) {
    abandon_reservation();
    return refuse("DATATYPE.DESCRIPTOR_INVALID",
                  "text_identity_migration_append_failed",
                  "sealed batch and relation descriptor were not published");
  }
  // The sealed metadata line is authoritative and can bootstrap the allocator
  // on restart. Publish allocator acceleration only after the one-line seal;
  // failure here cannot turn an already-visible migration into a refusal.
  (void)AppendDeferredEventSequenceAllocatorLines(
      context, &allocator_lines, nullptr);
  result.ok = true;
  result.migrated_row_count = request.rows.size();
  result.diagnostic = OkDiagnostic();
  result.evidence = {
      {"migration_id", request.migration_id},
      {"transaction_uuid", context.transaction_uuid.canonical},
      {"datatype_catalog_snapshot_uuid",
       context.datatype_catalog_snapshot_uuid.canonical},
      {"datatype_catalog_generation",
       std::to_string(context.datatype_catalog_generation)},
      {"datatype_registry_generation",
       std::to_string(context.datatype_registry_generation)},
      {"prior_catalog_snapshot_uuid", request.prior_catalog_snapshot_uuid},
      {"new_catalog_snapshot_uuid", request.new_catalog_snapshot_uuid},
      {"prior_catalog_generation",
       std::to_string(request.prior_catalog_generation)},
      {"new_catalog_generation",
       std::to_string(request.new_catalog_generation)},
      {"old_descriptor_uuid", std::string(kLegacyTextDescriptorUuid)},
      {"new_descriptor_uuid", std::string(kCanonicalTextDescriptorUuid)},
      {"old_type_uuid", std::string(kLegacyTextTypeUuid)},
      {"new_type_uuid", std::string(kCanonicalTextTypeUuid)},
      {"new_codec_uuid", std::string(kCanonicalTextCodecUuid)},
      {"new_codec_id", std::string(kCanonicalTextCodecId)},
      {"new_codec_version", "1"},
      {"new_codec_generation", "1"},
      {"decision_sha256", result.decision_sha256}};
  return result;
}

EngineApiDiagnostic AppendMgaIndexMetadata(const EngineRequestContext& context,
                                           const CrudIndexRecord& index) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthorityForStoreModule(
      context, "mga.relation_metadata.index_create");
  if (authority.error) { return authority; }
  CrudIndexRecord writable = index;
  writable.creator_tx = context.local_transaction_id;
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  writable.event_sequence = reservation.first;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "INDEX_METADATA",
                                     std::to_string(writable.creator_tx),
                                     std::to_string(writable.event_sequence),
                                     writable.index_uuid,
                                     writable.table_uuid,
                                     NormalizeCrudIndexProfile(writable.profile),
                                     writable.family.empty() ? CrudIndexFamilyForProfile(writable.profile) : writable.family,
                                     EncodeCrudText(writable.default_name),
                                     EncodeCrudText(writable.column_name),
                                     EncodeStringListAsCrudPairs(writable.key_envelopes),
                                     EncodeStringListAsCrudPairs(writable.include_columns),
                                     writable.predicate_kind,
                                     EncodeCrudText(writable.predicate_column),
                                     EncodeCrudText(writable.predicate_value),
                                     writable.unique ? "1" : "0",
                                     writable.exact_fallback ? "1" : "0"});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "index_metadata_append_failed");
  }
  return OkDiagnostic();
}


}  // namespace scratchbird::engine::internal_api
