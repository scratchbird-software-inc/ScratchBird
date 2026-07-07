// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "ddl/create_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;

namespace {

#ifndef SB_MISS008_SEED_PACK_ROOT
#define SB_MISS008_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2800-0000-7000-8000-000000000001";
constexpr const char* kSchemaUuid = "019f2800-0000-7000-8000-000000000101";
constexpr const char* kTableUuid = "019f2800-0000-7000-8000-000000000102";
constexpr const char* kUniqueIdIndexUuid = "019f2800-0000-7000-8000-000000000103";

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_missing_functionality_dml_upsert_variants_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_missing_functionality_dml_upsert_variants_conformance");
  Require(configured.ok(), "MISS-008 memory fixture configuration failed");
  Require(configured.fixture_mode, "MISS-008 memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_miss008_dml.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical =
      "019f2800-0000-7000-8000-00000000030" + std::to_string(ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019f2800-0000-7000-8000-00000000040" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = kUniqueIdIndexUuid;
  index.names.push_back(Name("miss008_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineRowValue Row(std::string row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EnginePredicateEnvelope IdEquals(std::string id) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = "id";
  predicate.bound_values.push_back(TextValue(std::move(id)));
  return predicate;
}

api::EnginePredicateEnvelope IdIn(std::initializer_list<std::string_view> ids) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_in_list";
  predicate.canonical_predicate_envelope = "id";
  for (const auto id : ids) {
    predicate.bound_values.push_back(TextValue(std::string(id)));
  }
  return predicate;
}

void Grant(api::EngineRequestContext* context, std::string right) {
  api::EngineMaterializedAuthorizationGrant grant;
  if (right == "INSERT") {
    grant.grant_uuid.canonical = "019f2800-0000-7000-8000-000000000601";
  } else if (right == "UPDATE") {
    grant.grant_uuid.canonical = "019f2800-0000-7000-8000-000000000602";
  } else if (right == "DELETE") {
    grant.grant_uuid.canonical = "019f2800-0000-7000-8000-000000000603";
  } else if (right == "SELECT") {
    grant.grant_uuid.canonical = "019f2800-0000-7000-8000-000000000604";
  } else {
    grant.grant_uuid.canonical = "019f2800-0000-7000-8000-000000000605";
  }
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = kTableUuid;
  grant.right = std::move(right);
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

void AddDmlAuthorization(api::EngineRequestContext* context) {
  context->authorization_context.present = true;
  context->authorization_context.authority_uuid = context->database_uuid;
  context->authorization_context.principal_uuid = context->principal_uuid;
  context->authorization_context.security_epoch = context->security_epoch;
  context->authorization_context.policy_epoch = 1;
  context->authorization_context.catalog_generation_id =
      context->catalog_generation_id;
  context->authorization_context.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  Grant(context, "INSERT");
  Grant(context, "UPDATE");
  Grant(context, "DELETE");
  Grant(context, "SELECT");
  Grant(context, "CATALOG_MUTATE");
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      std::string_view session_suffix = "001") {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "miss008-dml-upsert-variants";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDatabaseUuid;
  context.principal_uuid.canonical = "019f2800-0000-7000-8000-000000000002";
  context.session_uuid.canonical =
      std::string("019f2800-0000-7000-8000-000000000") +
      std::string(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("SBSQL-MISS-008");
  return context;
}

std::uint64_t EvidenceU64(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) continue;
    try {
      return static_cast<std::uint64_t>(std::stoull(evidence.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path,
                                           std::string_view session_suffix,
                                           bool dml_authorized = true) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database_path, session_suffix);
  auto begin = api::EngineBeginTransaction(request);
  Require(begin.ok, "MISS-008 transaction begin failed");
  auto context = BaseContext(database_path, session_suffix);
  context.local_transaction_id = begin.local_transaction_id;
  context.transaction_uuid = begin.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begin.snapshot_visible_through_local_transaction_id != 0
          ? begin.snapshot_visible_through_local_transaction_id
          : EvidenceU64(begin, "snapshot_visible_through_local_transaction_id");
  if (dml_authorized) {
    AddDmlAuthorization(&context);
  }
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  auto commit = api::EngineCommitTransaction(request);
  Require(commit.ok, "MISS-008 commit failed");
}

void CreateSchemaAndTable(const std::filesystem::path& database_path) {
  auto context = BeginTransaction(database_path, "101", false);

  api::EngineCreateSchemaRequest schema_request;
  schema_request.context = context;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("miss008_schema"));
  auto schema = api::EngineCreateSchema(schema_request);
  Require(schema.ok, "MISS-008 schema create failed");

  api::EngineCreateTableRequest table_request;
  table_request.context = context;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.requested_table_uuid.canonical = kTableUuid;
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.table_names.push_back(Name("miss008_table"));
  table_request.table_columns.push_back(Column(0, "id"));
  table_request.table_columns.push_back(Column(1, "note"));
  table_request.table_indexes.push_back(UniqueIdIndex());
  auto table = api::EngineCreateTable(table_request);
  Require(table.ok, "MISS-008 table create failed");
  Commit(context);
}

void InsertRows(const api::EngineRequestContext& context,
                std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  auto inserted = api::EngineInsertRows(request);
  Require(inserted.ok, "MISS-008 insert rows failed");
  Require(HasEvidence(inserted, "audit_event", "data.dml_change"),
          "MISS-008 insert audit evidence missing");
  Require(HasEvidence(inserted, "dml_result_shape"),
          "MISS-008 insert result shape evidence missing");
}

void VerifyRights(const api::EngineRequestContext& context) {
  Require(api::SecurityContextHasRight(context, "INSERT", kTableUuid),
          "MISS-008 INSERT right not materialized");
  Require(api::SecurityContextHasRight(context, "UPDATE", kTableUuid),
          "MISS-008 UPDATE right not materialized");
  Require(api::SecurityContextHasRight(context, "DELETE", kTableUuid),
          "MISS-008 DELETE right not materialized");
  auto denied = context;
  denied.authorization_context.grants.clear();
  Require(!api::SecurityContextHasRight(denied, "DELETE", kTableUuid),
          "MISS-008 empty grants unexpectedly authorized DELETE");
}

void VerifyUpsert(const api::EngineRequestContext& context) {
  api::EngineMergeRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.merge_surface_variant = "upsert";
  request.conflict_target_column = "id";
  request.on_conflict_action = "do_update";
  request.match_predicate.predicate_kind = "column_equals";
  request.match_predicate.canonical_predicate_envelope = "id";
  request.input_rows.push_back(Row("019f2800-0000-7000-8000-000000000301",
                                   "1",
                                   "upsert-source"));
  request.input_rows.push_back(Row("019f2800-0000-7000-8000-000000000302",
                                   "2",
                                   "upsert-inserted"));
  request.update_assignments.push_back({"note", TextValue("upsert-updated")});

  auto upserted = api::EngineMergeRows(request);
  Require(upserted.ok, "MISS-008 UPSERT merge failed");
  Require(upserted.matched_count == 1, "MISS-008 UPSERT matched count mismatch");
  Require(upserted.updated_count == 1, "MISS-008 UPSERT updated count mismatch");
  Require(upserted.inserted_count == 1, "MISS-008 UPSERT inserted count mismatch");
  Require(HasEvidence(upserted, "dml_surface_variant", "upsert"),
          "MISS-008 UPSERT surface evidence missing");
  Require(HasEvidence(upserted, "upsert_canonical_route", "dml.merge_rows"),
          "MISS-008 UPSERT canonical route evidence missing");
  Require(HasEvidence(upserted, "excluded_pseudo_relation",
                      "source_row_descriptor_bound"),
          "MISS-008 UPSERT EXCLUDED descriptor evidence missing");
  Require(HasEvidence(upserted, "merge_surface", "upsert_matched_update_or_insert"),
          "MISS-008 UPSERT merge surface evidence missing");
  Require(HasEvidence(upserted, "audit_event", "data.dml_change"),
          "MISS-008 UPSERT audit evidence missing");
}

void VerifyDeleteVariant(const api::EngineRequestContext& context,
                         std::string variant,
                         api::EnginePredicateEnvelope predicate,
                         std::string expected_evidence_kind,
                         std::string expected_evidence_id,
                         api::EngineApiU64 expected_deleted) {
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.delete_surface_variant = std::move(variant);
  request.delete_predicate = std::move(predicate);
  if (request.delete_surface_variant == "batch_delete") {
    request.batch_on_column = "id";
    request.batch_limit_rows = expected_deleted;
  } else if (request.delete_surface_variant == "drop_series") {
    request.series_name = "series";
  }

  auto deleted = api::EngineDeleteRows(request);
  Require(deleted.ok, "MISS-008 delete variant failed");
  Require(deleted.deleted_count == expected_deleted,
          "MISS-008 delete variant deleted count mismatch");
  Require(HasEvidence(deleted, "dml_surface_variant", request.delete_surface_variant),
          "MISS-008 delete surface evidence missing");
  Require(HasEvidence(deleted, expected_evidence_kind, expected_evidence_id),
          "MISS-008 delete variant-specific evidence missing");
  Require(HasEvidence(deleted, "audit_event", "data.dml_change"),
          "MISS-008 delete audit evidence missing");
  Require(HasEvidence(deleted, "mga_row_version", "row_delete_tombstone"),
          "MISS-008 delete tombstone evidence missing");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  const auto work = MakeTempDir();
  Require(!work.empty(), "MISS-008 failed to create temp directory");
  const auto database_path = work / "miss008.sbdb";

  api::EngineCreateLifecycleRequest create;
  create.context = BaseContext(database_path);
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") +
                                    SB_MISS008_SEED_PACK_ROOT);
  auto created = api::EngineCreateLifecycle(create);
  Require(created.ok, "MISS-008 lifecycle create database failed");
  CreateSchemaAndTable(database_path);

  auto context = BeginTransaction(database_path, "201");
  VerifyRights(context);
  InsertRows(context,
             {Row("019f2800-0000-7000-8000-000000000201", "1", "seed"),
              Row("019f2800-0000-7000-8000-000000000211", "b1", "batch-one"),
              Row("019f2800-0000-7000-8000-000000000212", "b2", "batch-two"),
              Row("019f2800-0000-7000-8000-000000000221", "e1", "erase-one"),
              Row("019f2800-0000-7000-8000-000000000231", "s1", "series-one")});
  VerifyUpsert(context);
  VerifyDeleteVariant(context,
                      "batch_delete",
                      IdIn({"b1", "b2"}),
                      "delete_chunked_limit_applied",
                      "true",
                      1);
  VerifyDeleteVariant(context,
                      "erase",
                      IdEquals("e1"),
                      "erase_semantics",
                      "audit_safe_valid_time_close",
                      1);
  VerifyDeleteVariant(context,
                      "drop_series",
                      IdEquals("s1"),
                      "drop_series_semantics",
                      "series_tombstone",
                      1);
  Commit(context);

  std::error_code cleanup_error;
  std::filesystem::remove_all(work, cleanup_error);
  return EXIT_SUCCESS;
}
