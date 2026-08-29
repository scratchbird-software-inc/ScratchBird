// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "dml/import_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "storage/database/local_transaction_store.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dl_test = scratchbird::tests::database_lifecycle;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kPlanOperationId = "dml.plan_import_rows";
constexpr std::string_view kPlanBinderTag =
    "private_dml_plan_import_rows_binder";
constexpr std::string_view kPlanConsumerTag =
    "private_dml_plan_import_rows_consumer";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 MillisSeed() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, MillisSeed() + salt);
  Require(generated.ok(), "CDP-011 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string table_uuid;
  std::string index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::size_t EvidenceCount(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  std::size_t count = 0;
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      ++count;
    }
  }
  return count;
}

std::size_t EvidenceCount(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view id) {
  std::size_t count = 0;
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      ++count;
    }
  }
  return count;
}

std::size_t ScopedRelationAccessEvidenceCount(
    const std::vector<api::EngineEvidenceReference>& evidence) {
  return EvidenceCount(evidence, "relation_state_scoped_loads", "1") +
         EvidenceCount(evidence,
                       "direct_physical_bulk_append_context_cache",
                       "hit");
}

bool HasScopedRelationAccessEvidence(
    const std::vector<api::EngineEvidenceReference>& evidence) {
  return ScopedRelationAccessEvidenceCount(evidence) != 0;
}

std::string FieldValue(const api::EngineResultShape& result,
                       std::size_t row_index,
                       std::string_view field_name) {
  Require(row_index < result.rows.size(), "CDP-011 result row index out of range");
  for (const auto& [name, value] : result.rows[row_index].fields) {
    if (name == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

std::string FirstNonEmptyFieldValue(const api::EngineResultShape& result,
                                    std::string_view field_name) {
  for (std::size_t row_index = 0; row_index < result.rows.size(); ++row_index) {
    const std::string value = FieldValue(result, row_index, field_name);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.default_root_uuid.canonical = fixture.filespace_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "CDP-011 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), "CDP-011 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "CDP-011 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_copy_append_batching";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord UniqueIdIndex(const Fixture& fixture,
                                   const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_cdp011_" + name + "_" + std::to_string(MillisSeed() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "cdp011.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = MillisSeed() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CDP-011 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.filespace_uuid = uuid::UuidToString(create.filespace_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "cdp011-metadata");
  const auto table_record = Table(fixture, metadata);
  const auto table = api::AppendMgaTableMetadata(metadata, table_record);
  Require(!table.error, "CDP-011 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(metadata, UniqueIdIndex(fixture, metadata));
  Require(!index.error, "CDP-011 unique index metadata append failed");
  api::MgaRelationStorageDescriptor relation_descriptor;
  Require(!api::EnsureMgaRelationStorageDescriptor(
               metadata, table_record, {}, &relation_descriptor)
               .error,
          "CDP-011 relation descriptor persistence failed");
  Commit(metadata);
  return fixture;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options,
    std::string reject_mode = "fail_fast",
    api::EngineApiU64 reject_limit_rows = 10) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = std::move(reject_mode);
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  if (request.import_policy.reject_mode != "fail_fast") {
    request.import_policy.reject_limit_rows = reject_limit_rows;
  }
  request.canonical_rows = std::move(rows);
  request.estimated_row_count = static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineRequestContext AttachPlanStatementReceipt(
    const Fixture& fixture,
    api::EngineRequestContext context) {
  const platform::u64 identity_salt =
      fixture.salt + context.local_transaction_id * 32;
  context.statement_uuid.canonical =
      NewUuidText(platform::UuidKind::object, identity_salt + 1);
  context.statement_snapshot_uuid.canonical.clear();
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  RequireOk(snapshot, "CDP-011 plan statement snapshot publication failed");
  context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context.statement_snapshot_generation =
      snapshot.snapshot_vector
          .publication_inventory_next_local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_receipt_uuid.canonical =
      NewUuidText(platform::UuidKind::object, identity_salt + 2);
  context.statement_metadata_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object, identity_salt + 3);
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_active_excluded_local_transaction_ids =
      snapshot.snapshot_vector.active_excluded_local_transaction_ids;
  context.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      snapshot.snapshot_vector.in_doubt_excluded_local_transaction_ids;
  context.transaction_policy_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object, identity_salt + 4);
  context.transaction_policy_snapshot_generation = 1;
  context.resource_admission_uuid.canonical =
      NewUuidText(platform::UuidKind::object, identity_salt + 5);

  auto& authorization = context.authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      NewUuidText(platform::UuidKind::object, identity_salt + 6);
  authorization.security_context_generation = 1;
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = 1;
  authorization.catalog_generation_id = context.catalog_generation_id;
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = context.principal_uuid;
  subject.subject_kind = "principal";
  authorization.effective_subjects.push_back(subject);
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      NewUuidText(platform::UuidKind::object, identity_salt + 7);
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = fixture.table_uuid;
  grant.right = "INSERT";
  grant.security_epoch = context.security_epoch;
  authorization.grants.push_back(std::move(grant));
  return context;
}

api::SblrExecutorAvailabilityRowIdentity PlanAvailabilityIdentity() {
  api::SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = api::kSblrDmlPlanImportRowsExecutorId;
  identity.opcode_code = api::kSblrDmlPlanImportRowsOpcodeCode;
  identity.opcode_version = api::kSblrDmlPlanImportRowsOpcodeVersion;
  identity.operand_descriptor_id =
      api::kSblrDmlPlanImportRowsOperandDescriptorId;
  identity.result_descriptor_id =
      api::kSblrDmlPlanImportRowsResultDescriptorId;
  identity.result_descriptor_version =
      api::kSblrDmlPlanImportRowsResultDescriptorVersion;
  return identity;
}

void RequirePlanAvailability(const api::EngineRequestContext& context,
                             bool bootstrap) {
  if (bootstrap) {
    const auto installed = api::LoadSblrExecutorAvailabilitySnapshot(
        context, PlanAvailabilityIdentity());
    Require(installed.ok && installed.snapshot.installed &&
                installed.snapshot.generation != 0,
            "CDP-011 plan executor availability bootstrap failed");
  }
  const auto current = api::LoadCurrentSblrExecutorAvailabilitySnapshot(
      context, PlanAvailabilityIdentity());
  Require(current.ok && current.snapshot.installed &&
              current.snapshot.generation != 0,
          "CDP-011 current plan executor availability missing");
}

api::EngineRequestContext WithPlanTraceTag(api::EngineRequestContext context,
                                           std::string_view tag) {
  context.trace_tags.clear();
  context.trace_tags.emplace_back(tag);
  return context;
}

api::EngineCreateImportRowsPlanDescriptorRequestV1 PlanFactoryRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& binder_context,
    api::EngineApiU64 structural_occurrence_id = 1) {
  api::EngineCreateImportRowsPlanDescriptorRequestV1 request;
  request.context = binder_context;
  request.structural_occurrence_id = structural_occurrence_id;
  request.target_table_uuid.canonical = fixture.table_uuid;
  request.source_kind = sblr::PlanImportRowsSourceKindV1::csv_stream;
  request.format_family = sblr::PlanImportRowsFormatFamilyV1::csv;
  request.reject_mode = sblr::PlanImportRowsRejectModeV1::fail_fast;
  request.reject_payload_policy =
      sblr::PlanImportRowsRejectPayloadPolicyV1::diagnostic_only;
  request.resume_policy = sblr::PlanImportRowsResumePolicyV1::fail_closed;
  return request;
}

api::EnginePlanImportRowsRequest PlanRequest(
    const api::EngineRequestContext& consumer_context,
    const sblr::PlanImportRowsDescriptorRefV1& descriptor_ref) {
  api::EnginePlanImportRowsRequest request;
  request.context = consumer_context;
  request.operation_id = std::string(kPlanOperationId);
  request.descriptor_ref = descriptor_ref;
  return request;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index + 1),
                       prefix + "-note-" + std::to_string(index + 1)));
  }
  return rows;
}

api::EngineApiU64 SelectCount(const Fixture& fixture,
                              const api::EngineRequestContext& context) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "CDP-011 select failed");
  return selected.visible_count;
}

template <std::size_t N>
bool NonzeroBytes(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

std::string PlanUuidText(const sblr::PlanImportRowsUuidV1& value) {
  platform::Uuid uuid_value;
  uuid_value.bytes = value;
  return uuid::UuidToString(uuid_value);
}

void RequirePlanSuccess(
    const api::EnginePlanImportRowsResult& result,
    const sblr::PlanImportRowsDescriptorRefV1& descriptor_ref) {
  RequireOk(result, "CDP-011 exact bound import plan failed");
  Require(result.surface_accepted && result.planning_only &&
              result.execution_requires_execute_import_rows &&
              !result.row_execution_completed &&
              !result.row_persistence_claimed,
          "CDP-011 bound plan validation-only result contract drifted");
  Require(result.normalized_insert_mode_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsInsertModeV1::copy_import) &&
              result.normalized_source_kind_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsSourceKindV1::csv_stream) &&
              result.normalized_format_family_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsFormatFamilyV1::csv) &&
              result.mapped_column_count == 0 &&
              result.validated_request_descriptor_uuid.canonical ==
                  PlanUuidText(descriptor_ref.descriptor_uuid) &&
              result.validated_request_descriptor_generation ==
                  descriptor_ref.descriptor_generation &&
              NonzeroBytes(result.validated_request_projection_sha256),
          "CDP-011 bound plan normalized descriptor result drifted");
  Require(result.accepted_executor_evidence.exact_bytes.size() ==
                  sblr::kPlanImportRowsExecutorEvidenceBytesV1 &&
              result.accepted_executor_evidence.completed_validation_bits ==
                  sblr::kPlanImportRowsAcceptedValidationBitsV1,
          "CDP-011 bound plan accepted IPEV missing");
}

void RequirePlanRefusal(const api::EnginePlanImportRowsResult& result,
                        std::string_view diagnostic_code,
                        std::string_view message) {
  if (result.ok || result.diagnostics.size() != 1 ||
      result.diagnostics.front().code != diagnostic_code) {
    if (!result.diagnostics.empty()) {
      std::cerr << "expected=" << diagnostic_code << ";actual="
                << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
  Require(!result.surface_accepted && !result.planning_only &&
              !result.execution_requires_execute_import_rows &&
              !result.row_execution_completed &&
              !result.row_persistence_claimed &&
              result.mapped_column_count == 0 &&
              result.validated_request_descriptor_uuid.canonical.empty() &&
              result.validated_request_descriptor_generation == 0 &&
              result.accepted_executor_evidence.exact_bytes.empty() &&
              result.evidence.empty(),
          "CDP-011 plan refusal published success extensions or evidence");
}

void RequireInventoryEqual(
    const mga::LocalTransactionInventory& before,
    const mga::LocalTransactionInventory& after) {
  Require(before.next_local_transaction_id == after.next_local_transaction_id &&
              before.entries.size() == after.entries.size(),
          "CDP-011 planning changed transaction inventory shape");
  for (std::size_t index = 0; index < before.entries.size(); ++index) {
    const auto& left = before.entries[index];
    const auto& right = after.entries[index];
    Require(left.identity.local_id.value == right.identity.local_id.value &&
                left.identity.transaction_uuid.kind ==
                    right.identity.transaction_uuid.kind &&
                left.identity.transaction_uuid.value ==
                    right.identity.transaction_uuid.value &&
                left.identity.scope == right.identity.scope &&
                left.state == right.state &&
                left.begin_unix_epoch_millis ==
                    right.begin_unix_epoch_millis &&
                left.final_unix_epoch_millis ==
                    right.final_unix_epoch_millis &&
                left.begin_visible_through_local_transaction_id ==
                    right.begin_visible_through_local_transaction_id &&
                left.evidence_record_required ==
                    right.evidence_record_required &&
                left.evidence_record_written ==
                    right.evidence_record_written &&
                left.rollback_only == right.rollback_only,
            "CDP-011 planning changed a transaction inventory row");
  }
}

void RequireRelationStateEqual(const api::MgaRelationStoreState& before,
                               const api::MgaRelationStoreState& after) {
  const auto& left = before.crud_metadata;
  const auto& right = after.crud_metadata;
  Require(before.row_versions.size() == after.row_versions.size() &&
              before.index_entries.size() == after.index_entries.size() &&
              before.max_row_event_sequence == after.max_row_event_sequence &&
              before.max_index_event_sequence ==
                  after.max_index_event_sequence &&
              left.transactions == right.transactions &&
              left.tables.size() == right.tables.size() &&
              left.indexes.size() == right.indexes.size() &&
              left.large_values.size() == right.large_values.size() &&
              left.sealed_relation_descriptor_snapshots.size() ==
                  right.sealed_relation_descriptor_snapshots.size() &&
              left.max_transaction_id == right.max_transaction_id &&
              left.max_sequence == right.max_sequence &&
              left.max_index_sequence == right.max_index_sequence &&
              left.max_event_sequence == right.max_event_sequence &&
              left.savepoints == right.savepoints,
          "CDP-011 planning changed relation/page/index/catalog state");
}

sblr::SblrOperand Operand(std::string type,
                          std::string name,
                          std::string value,
                          std::uint32_t ordinal,
                          const platform::Uuid& descriptor_uuid) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.ordinal = ordinal;
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body.assign(descriptor_uuid.bytes.begin(),
                            descriptor_uuid.bytes.end());
  const auto value_size = static_cast<std::uint64_t>(value.size());
  for (unsigned byte = 0; byte < 8; ++byte) {
    operand.value_body.push_back(
        static_cast<std::uint8_t>((value_size >> (byte * 8)) & 0xffu));
  }
  operand.value_body.insert(operand.value_body.end(), value.begin(), value.end());
  return operand;
}

sblr::SblrOperationEnvelope ExecuteImportEnvelope(const Fixture& fixture) {
  auto envelope = sblr::MakeSblrEnvelope("dml.execute_import_rows",
                                         "SBLR_DML_EXECUTE_IMPORT_ROWS",
                                         "CDP-011-SBLR-EXECUTE-IMPORT");
  envelope.opcode_code = 0x0316u;
  envelope.parser_package_uuid = NewUuidText(platform::UuidKind::object, 7000);
  envelope.registry_snapshot_uuid = NewUuidText(platform::UuidKind::object, 7001);
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.result_shape = "engine_api_result";
  envelope.diagnostic_shape = "engine_api_diagnostic_vector";
  const auto text_descriptor_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 7002).value;
  const auto append_text = [&](std::string name, std::string value) {
    envelope.operands.push_back(
        Operand("text", std::move(name), std::move(value),
                static_cast<std::uint32_t>(envelope.operands.size() + 1),
                text_descriptor_uuid));
  };
  append_text("target_object_uuid", fixture.table_uuid);
  append_text("target_object_kind", "table");
  append_text("source_kind", "csv_stream");
  append_text("format_family", "csv");
  append_text("reject_mode", "fail_fast");
  append_text("checkpoint_mode", "disabled");
  append_text("estimated_row_count", "2");
  return envelope;
}

api::EngineApiRequest SblrApiRequest(const Fixture& fixture,
                                     std::vector<api::EngineRowValue> rows) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.rows = std::move(rows);
  return request;
}

void SeedCommittedRow(const Fixture& fixture, std::string id, std::string note) {
  auto context = Begin(fixture, "cdp011-seed");
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(note)));
  request.estimated_row_count = 1;
  const auto inserted = api::EngineInsertRows(request);
  RequireOk(inserted, "CDP-011 seed insert failed");
  Commit(context);
}

void TestEnabledAndDisabledBatchingProduceSameRows() {
  auto enabled_fixture = MakeFixture("enabled", 1000);
  auto enabled_context = Begin(enabled_fixture, "cdp011-enabled");
  auto enabled = api::EngineExecuteImportRows(ImportRequest(
      enabled_fixture,
      enabled_context,
      Rows("enabled", 6),
      {"copy_append_batching=enabled", "copy_append_batch_rows=2"}));
  RequireOk(enabled, "CDP-011 enabled COPY append batch failed");
  Require(enabled.inserted_rows == 6 && enabled.accepted_rows == 6,
          "CDP-011 enabled COPY row count mismatch");
  Require(HasEvidence(enabled.evidence, "copy_append_batching", "enabled"),
          "CDP-011 enabled evidence missing");
  Require(HasEvidence(enabled.evidence, "copy_append_batch_count", "1"),
          "CDP-011 enabled path did not execute as one engine append batch");
  Require(HasEvidence(enabled.evidence, "copy_append_batch_rows", "6"),
          "CDP-011 enabled path did not report actual batch rows");
  Require(SelectCount(enabled_fixture, enabled_context) == 6,
          "CDP-011 enabled rows not visible inside writer transaction");
  Rollback(enabled_context);

  auto disabled_fixture = MakeFixture("disabled", 2000);
  auto disabled_context = Begin(disabled_fixture, "cdp011-disabled");
  auto disabled = api::EngineExecuteImportRows(ImportRequest(
      disabled_fixture,
      disabled_context,
      Rows("disabled", 6),
      {"copy_append_batching=disabled"}));
  RequireOk(disabled, "CDP-011 disabled COPY singleton baseline failed");
  Require(disabled.inserted_rows == enabled.inserted_rows &&
              disabled.accepted_rows == enabled.accepted_rows,
          "CDP-011 disabled baseline changed row counts");
  Require(HasEvidence(disabled.evidence, "copy_append_batching", "disabled"),
          "CDP-011 disabled evidence missing");
  Require(HasEvidence(disabled.evidence, "copy_append_batch_count", "6"),
          "CDP-011 disabled path did not execute singleton baseline");
  Require(HasEvidence(disabled.evidence, "copy_append_batch_rows", "1"),
          "CDP-011 disabled path did not report singleton batch rows");
  Require(SelectCount(disabled_fixture, disabled_context) == 6,
          "CDP-011 disabled rows not visible inside writer transaction");
  Rollback(disabled_context);
}

void TestPlanContractIsCompleteAndExecutionBound() {
  auto fixture = MakeFixture("plan_contract", 2500);
  auto context = AttachPlanStatementReceipt(
      fixture, Begin(fixture, "cdp011-plan-contract"));
  RequirePlanAvailability(context, true);
  const auto binder_context = WithPlanTraceTag(context, kPlanBinderTag);
  const auto consumer_context = WithPlanTraceTag(context, kPlanConsumerTag);

  const auto bound = api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
      PlanFactoryRequest(fixture, binder_context));
  if (!bound.ok) {
    std::cerr << bound.diagnostic.code << ':' << bound.diagnostic.detail << '\n';
  }
  Require(bound.ok, "CDP-011 exact import descriptor bind failed");

  auto unsupported_demand = PlanFactoryRequest(fixture, binder_context, 2);
  unsupported_demand.format_family = sblr::PlanImportRowsFormatFamilyV1::jsonl;
  const auto unsupported_bound =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
          unsupported_demand);
  Require(unsupported_bound.ok,
          "CDP-011 recognized unsupported descriptor did not bind");

  const auto relation_before =
      api::LoadMgaRelationStoreState(consumer_context);
  Require(relation_before.ok,
          "CDP-011 plan relation-state baseline load failed");
  const auto inventory_before =
      db::LoadLocalTransactionInventoryFromDatabase(
          fixture.database_path.string());
  Require(inventory_before.ok(),
          "CDP-011 plan transaction-inventory baseline load failed");
  const auto descriptor_before = api::LoadMgaRelationStorageDescriptor(
      consumer_context, fixture.table_uuid);
  Require(descriptor_before.ok,
          "CDP-011 plan relation descriptor baseline load failed");
  Require(SelectCount(fixture, consumer_context) == 0,
          "CDP-011 plan target was not empty before validation");

  const auto request = PlanRequest(consumer_context, bound.descriptor_ref);
  RequirePlanSuccess(api::EnginePlanImportRows(request), bound.descriptor_ref);
  RequirePlanSuccess(api::EnginePlanImportRows(request), bound.descriptor_ref);

  auto missing_uuid = request;
  missing_uuid.descriptor_ref = {};
  RequirePlanRefusal(api::EnginePlanImportRows(missing_uuid),
                     "SBLR.OPERAND_INVALID",
                     "CDP-011 missing plan UUID was not refused");

  auto localized_name = request;
  localized_name.localized_names.emplace_back();
  RequirePlanRefusal(api::EnginePlanImportRows(localized_name),
                     "SBLR.OPERAND_INVALID",
                     "CDP-011 localized plan authority was not refused");

  auto malformed_mapping = request;
  api::EngineImportColumnMapping legacy_mapping;
  legacy_mapping.target_column = "id";
  malformed_mapping.column_mappings.push_back(std::move(legacy_mapping));
  RequirePlanRefusal(api::EnginePlanImportRows(malformed_mapping),
                     "SBLR.OPERAND_INVALID",
                     "CDP-011 malformed mapping authority was not refused");

  auto denied = request;
  denied.context.authorization_context.grants.clear();
  RequirePlanRefusal(api::EnginePlanImportRows(denied),
                     "SECURITY.ACCESS_DENIED",
                     "CDP-011 denied INSERT plan was not refused");

  auto missing_transaction = request;
  missing_transaction.context.local_transaction_id = 0;
  missing_transaction.context.transaction_uuid.canonical.clear();
  RequirePlanRefusal(api::EnginePlanImportRows(missing_transaction),
                     "MGA.TRANSACTION_INVALID",
                     "CDP-011 missing plan transaction was not refused");

  auto stale_transaction = request;
  stale_transaction.context.transaction_uuid.canonical = NewUuidText(
      platform::UuidKind::transaction, fixture.salt + 900);
  RequirePlanRefusal(api::EnginePlanImportRows(stale_transaction),
                     "MGA.TRANSACTION_INVALID",
                     "CDP-011 stale plan transaction was not refused");

  auto stale_snapshot = request;
  ++stale_snapshot.context.statement_snapshot_generation;
  RequirePlanRefusal(api::EnginePlanImportRows(stale_snapshot),
                     "MGA.AUTHORITY_MISMATCH",
                     "CDP-011 stale plan snapshot was not refused");

  auto stale_receipt = request;
  stale_receipt.context.statement_receipt_uuid.canonical = NewUuidText(
      platform::UuidKind::object, fixture.salt + 901);
  RequirePlanRefusal(api::EnginePlanImportRows(stale_receipt),
                     "MGA.AUTHORITY_MISMATCH",
                     "CDP-011 stale plan receipt was not refused");

  RequirePlanRefusal(
      api::EnginePlanImportRows(
          PlanRequest(consumer_context, unsupported_bound.descriptor_ref)),
      "SBLR.OPERATION_UNSUPPORTED",
      "CDP-011 unsupported source/format plan was not refused");

  auto invalid_target = PlanFactoryRequest(fixture, binder_context, 3);
  invalid_target.target_table_uuid.canonical.clear();
  const auto invalid_target_result =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
          invalid_target);
  Require(!invalid_target_result.ok &&
              invalid_target_result.diagnostic.code ==
                  "SBLR.OPERAND_INVALID",
          "CDP-011 missing target UUID binder demand was not refused");

  auto invalid_policy = PlanFactoryRequest(fixture, binder_context, 4);
  invalid_policy.reject_mode =
      static_cast<sblr::PlanImportRowsRejectModeV1>(0);
  const auto invalid_policy_result =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
          invalid_policy);
  Require(!invalid_policy_result.ok &&
              invalid_policy_result.diagnostic.code ==
                  "SBLR.OPERAND_INVALID",
          "CDP-011 invalid plan policy was not refused");

  const auto relation_after =
      api::LoadMgaRelationStoreState(consumer_context);
  Require(relation_after.ok,
          "CDP-011 plan relation-state postcondition load failed");
  RequireRelationStateEqual(relation_before.state, relation_after.state);
  const auto inventory_after =
      db::LoadLocalTransactionInventoryFromDatabase(
          fixture.database_path.string());
  Require(inventory_after.ok(),
          "CDP-011 plan transaction-inventory postcondition load failed");
  RequireInventoryEqual(inventory_before.inventory, inventory_after.inventory);
  const auto descriptor_after = api::LoadMgaRelationStorageDescriptor(
      consumer_context, fixture.table_uuid);
  Require(descriptor_after.ok &&
              api::SerializeMgaRelationStorageDescriptor(
                  descriptor_before.descriptor) ==
                  api::SerializeMgaRelationStorageDescriptor(
                      descriptor_after.descriptor),
          "CDP-011 planning changed the catalog relation descriptor");
  Require(SelectCount(fixture, consumer_context) == 0,
          "CDP-011 plan success or refusal inserted target rows");

  const auto released =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  Require(released.ok && released.released_row_count == 2,
          "CDP-011 plan descriptor release count drifted");
  Rollback(context);

  auto rollback_reader = Begin(fixture, "cdp011-plan-rollback-reader");
  Require(SelectCount(fixture, rollback_reader) == 0,
          "CDP-011 planning followed by rollback mutated rows");
  Rollback(rollback_reader);

  auto commit_context = AttachPlanStatementReceipt(
      fixture, Begin(fixture, "cdp011-plan-commit-without-execute"));
  RequirePlanAvailability(commit_context, false);
  const auto commit_binder =
      WithPlanTraceTag(commit_context, kPlanBinderTag);
  const auto commit_consumer =
      WithPlanTraceTag(commit_context, kPlanConsumerTag);
  const auto commit_bound =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
          PlanFactoryRequest(fixture, commit_binder));
  Require(commit_bound.ok,
          "CDP-011 commit-without-execute descriptor bind failed");
  RequirePlanSuccess(
      api::EnginePlanImportRows(
          PlanRequest(commit_consumer, commit_bound.descriptor_ref)),
      commit_bound.descriptor_ref);
  Require(SelectCount(fixture, commit_consumer) == 0,
          "CDP-011 plan inserted rows before commit-without-execute");
  const auto commit_release =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(commit_binder);
  Require(commit_release.ok && commit_release.released_row_count == 1,
          "CDP-011 commit plan descriptor release failed");
  Commit(commit_context);

  db::DatabaseOpenConfig open;
  open.path = fixture.database_path.string();
  const auto opened = db::OpenDatabaseFile(open);
  Require(opened.ok(),
          "CDP-011 commit-without-execute database did not reopen");
  auto commit_reader = Begin(fixture, "cdp011-plan-commit-reader");
  Require(SelectCount(fixture, commit_reader) == 0,
          "CDP-011 planning followed by commit mutated rows");
  Rollback(commit_reader);
}

void TestRollbackInvisibilityAndCommittedReopenVisibility() {
  auto rollback_fixture = MakeFixture("rollback", 3000);
  auto rollback_context = Begin(rollback_fixture, "cdp011-rollback-writer");
  const auto rolled = api::EngineExecuteImportRows(ImportRequest(
      rollback_fixture,
      rollback_context,
      Rows("rollback", 4),
      {"copy_append_batching=enabled"}));
  RequireOk(rolled, "CDP-011 rollback import failed before rollback");
  Rollback(rollback_context);

  auto rollback_reader = Begin(rollback_fixture, "cdp011-rollback-reader");
  Require(SelectCount(rollback_fixture, rollback_reader) == 0,
          "CDP-011 rolled-back COPY rows became visible");
  Rollback(rollback_reader);

  auto commit_fixture = MakeFixture("commit_reopen", 4000);
  auto commit_context = Begin(commit_fixture, "cdp011-commit-writer");
  const auto committed = api::EngineExecuteImportRows(ImportRequest(
      commit_fixture,
      commit_context,
      Rows("commit", 5),
      {"copy_append_batching=enabled"}));
  RequireOk(committed, "CDP-011 committed import failed");
  Commit(commit_context);

  const auto opened = db::OpenDatabaseFile({commit_fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "CDP-011 committed database did not reopen");

  auto reopen_reader = Begin(commit_fixture, "cdp011-reopen-reader");
  Require(SelectCount(commit_fixture, reopen_reader) == 5,
          "CDP-011 committed COPY rows were not visible after reopen");
  Rollback(reopen_reader);
}

void TestSblrExecuteImportRowsDispatchesToExecutor() {
  auto fixture = MakeFixture("sblr_execute", 4500);
  auto context = Begin(fixture, "cdp011-sblr-execute");
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = context;
  dispatch.envelope = ExecuteImportEnvelope(fixture);
  dispatch.api_request = SblrApiRequest(fixture, Rows("sblr", 2));
  const auto result = sblr::DispatchSblrOperation(dispatch);
  if (!(result.accepted && result.envelope_validated && result.dispatched_to_api &&
        result.api_result.ok)) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message << '\n';
    }
    if (!result.api_result.diagnostics.empty()) {
      std::cerr << result.api_result.diagnostics.front().code << ':'
                << result.api_result.diagnostics.front().detail << '\n';
    }
    Fail("CDP-011 SBLR execute import dispatch failed");
  }
  Require(result.api_result.operation_id == "dml.execute_import_rows",
          "CDP-011 SBLR import dispatched to wrong operation");
  Require(HasEvidence(result.api_result.evidence, "import_plan_consumed", "false"),
          "CDP-011 SBLR execute import falsely consumed a planning result");
  Require(HasEvidence(result.api_result.evidence,
                      "import_execution_descriptor_revalidated",
                      "true"),
          "CDP-011 SBLR execute import did not revalidate its descriptor");
  Require(HasEvidence(result.api_result.evidence,
                      "import_execution_row_execution_completed",
                      "true"),
          "CDP-011 SBLR import did not complete execution");
  Require(HasEvidence(result.api_result.evidence, "parser_finality_authority", "false"),
          "CDP-011 SBLR import parser finality evidence missing");
  Require(SelectCount(fixture, context) == 2,
          "CDP-011 SBLR import rows were not visible in writer transaction");
  Rollback(context);
}

void TestRejectModeFallsBackWithoutLosingGoodRows() {
  auto fixture = MakeFixture("reject", 5000);
  SeedCommittedRow(fixture, "duplicate-id", "seed");

  auto context = Begin(fixture, "cdp011-reject");
  std::vector<api::EngineRowValue> rows;
  rows.push_back(Row("new-id-1", "accepted-a"));
  rows.push_back(Row("new-id-2", "accepted-b"));
  rows.push_back(Row("duplicate-id", "rejected-duplicate"));
  rows.push_back(Row("new-id-3", "accepted-c"));
  rows.push_back(Row("new-id-4", "accepted-d"));

  const auto result = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      std::move(rows),
      {"copy_append_batching=enabled", "copy_append_batch_rows=8"},
      "reject_row"));
  RequireOk(result, "CDP-011 reject-row COPY import failed");
  Require(result.accepted_rows == 4 && result.inserted_rows == 4 && result.rejected_rows == 1,
          "CDP-011 reject-row counts mismatch");
  Require(result.delegated_to_insert_rows,
          "CDP-011 reject-row path did not use EngineInsertRows");
  Require(HasEvidence(result.evidence, "copy_append_reject_fallback", "bisection"),
          "CDP-011 reject-row path did not record bisection fallback");
  Require(HasEvidence(result.evidence, "copy_slow_path",
                      "reject_bisection_singleton_fallback"),
          "CDP-011 reject-row path did not record slow-path reason");
  Require(HasEvidence(result.evidence, "copy_append_singleton_fallback_batches", "1"),
          "CDP-011 reject-row path did not record singleton fallback");
  Require(HasEvidence(result.evidence, "copy_append_bisection_split_count", "2"),
          "CDP-011 reject-row split count changed");
  Require(HasEvidence(result.evidence,
                      "copy_append_bisection_terminal_singleton_count",
                      "1"),
          "CDP-011 reject-row terminal singleton count changed");
  Require(HasEvidence(result.evidence,
                      "copy_append_bisection_batch_attempt_count",
                      "5"),
          "CDP-011 reject-row batch attempt count changed");
  Require(EvidenceCount(result.evidence,
                        "copy_append_bisection_batch_outcome",
                        "accepted_sub_batch") >= 2,
          "CDP-011 reject-row did not preserve accepted sub-batches");
  Require(EvidenceCount(result.evidence,
                        "copy_append_bisection_accepted_sub_batch_rows",
                        "2") >= 2,
          "CDP-011 reject-row accepted sub-batches were not batched");
  Require(EvidenceCount(result.evidence, "prepared_insert_descriptor") >= 2,
          "CDP-011 reject-row accepted sub-batches skipped prepared descriptors");
  Require(EvidenceCount(result.evidence, "insert_row_encoder_plan") >= 2,
          "CDP-011 reject-row accepted sub-batches skipped canonical row encoder");
  Require(ScopedRelationAccessEvidenceCount(result.evidence) >= 2,
          "CDP-011 reject-row accepted sub-batches skipped scoped relation access");
  Require(EvidenceCount(result.evidence, "mga_row_store", "row_insert") >= 2,
          "CDP-011 reject-row accepted sub-batches skipped MGA row writer");
  Require(HasEvidence(result.evidence, "dml_summary.rows_changed", "4"),
          "CDP-011 reject-row final DML summary changed");
  Require(HasEvidence(result.evidence, "import_reject_materialization", "result_shape"),
          "CDP-011 reject-row path did not materialize reject diagnostic");
  Require(!HasEvidence(result.evidence, "import_reject_materialization", "reject_target"),
          "CDP-011 diagnostic-only reject-row path wrote a reject target");
  Require(result.result_shape.rows.size() == 5,
          "CDP-011 reject-row result should include four accepted rows and one diagnostic row");
  const std::string duplicate_code =
      FirstNonEmptyFieldValue(result.result_shape, "diagnostic_code");
  Require(duplicate_code == "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION" ||
              duplicate_code == "CLI.CONSTRAINT_UNIQUE_VIOLATION" ||
              duplicate_code == "SB_ENGINE_API_INVALID_REQUEST",
          "CDP-011 reject-row diagnostic code mismatch");
  const std::string duplicate_detail =
      FirstNonEmptyFieldValue(result.result_shape, "diagnostic_detail");
  Require(duplicate_detail.find("duplicate_key") != std::string::npos ||
              duplicate_detail.find("unique_index_duplicate") != std::string::npos,
          "CDP-011 reject-row diagnostic detail did not describe duplicate key");
  Require(SelectCount(fixture, context) == 5,
          "CDP-011 reject-row valid rows were not visible in writer transaction");
  Rollback(context);
}

void TestRejectLimitStopsBisectionAfterBoundedRejects() {
  auto fixture = MakeFixture("reject_limit", 6000);
  SeedCommittedRow(fixture, "duplicate-id", "seed");

  auto context = Begin(fixture, "cdp011-reject-limit");
  std::vector<api::EngineRowValue> rows;
  rows.push_back(Row("duplicate-id", "rejected-first"));
  rows.push_back(Row("limit-valid-id", "accepted-before-limit"));
  rows.push_back(Row("duplicate-id", "rejected-over-limit"));

  const auto result = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      std::move(rows),
      {"copy_append_batching=enabled", "copy_append_batch_rows=8"},
      "reject_row",
      1));
  Require(!result.ok, "CDP-011 reject limit import unexpectedly succeeded");
  Require(!result.diagnostics.empty(),
          "CDP-011 reject limit failure did not return a diagnostic");
  Require(result.diagnostics.front().detail.find("reject_limit_exceeded") !=
              std::string::npos,
          "CDP-011 reject limit diagnostic changed");
  Require(result.accepted_rows == 1 && result.inserted_rows == 1 &&
              result.rejected_rows == 2,
          "CDP-011 reject limit counts mismatch");
  Require(HasEvidence(result.evidence, "import_execution_refused_by", "reject_limit"),
          "CDP-011 reject limit refusal evidence missing");
  Require(HasEvidence(result.evidence, "import_reject_limit_exceeded", "2"),
          "CDP-011 reject limit count evidence missing");
  Require(HasEvidence(result.evidence, "copy_append_reject_fallback", "bisection"),
          "CDP-011 reject limit did not retain bisection evidence");
  Require(HasEvidence(result.evidence, "copy_slow_path_reason", "reject_limit_exceeded"),
          "CDP-011 reject limit slow-path reason missing");
  Require(EvidenceCount(result.evidence,
                        "copy_append_bisection_batch_outcome",
                        "rejected_singleton") >= 2,
          "CDP-011 reject limit did not isolate rejected singleton rows");
  Require(EvidenceCount(result.evidence, "prepared_insert_descriptor") != 0,
          "CDP-011 reject limit accepted row skipped prepared descriptor path");
  Require(EvidenceCount(result.evidence, "insert_row_encoder_plan") != 0,
          "CDP-011 reject limit accepted row skipped canonical row encoder");
  Require(HasScopedRelationAccessEvidence(result.evidence),
          "CDP-011 reject limit accepted row skipped scoped relation access");
  Require(SelectCount(fixture, context) == 2,
          "CDP-011 reject limit accepted row was not visible before rollback");
  Rollback(context);
}

}  // namespace

int main() {
  dl_test::ConfigureLifecycleMemoryFixture("cdp_copy_append_batching_gate");
  TestEnabledAndDisabledBatchingProduceSameRows();
  TestPlanContractIsCompleteAndExecutionBound();
  TestRollbackInvisibilityAndCommittedReopenVisibility();
  TestSblrExecuteImportRowsDispatchesToExecutor();
  TestRejectModeFallsBackWithoutLosingGoodRows();
  TestRejectLimitStopsBisectionAfterBoundedRejects();
  return EXIT_SUCCESS;
}
