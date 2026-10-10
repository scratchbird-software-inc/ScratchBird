#include "catalog/constraint_metadata_codec.hpp"
#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/catalog_column_binding_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "transaction/transaction_api.hpp"
#include "core/memory/memory.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/ddl_support_service.hpp"
#include "catalog/pinned_descriptor_cache.hpp"
#include "catalog/schema_tree_api.hpp"
#include "catalog/name_registry.hpp"
#include "ddl/create_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

constexpr auto kSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000001");
constexpr auto kTableUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000101");
constexpr auto kDomainUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000102");
constexpr auto kViewUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000201");
constexpr auto kTriggerUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000202");
constexpr auto kConstraintUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000203");
constexpr auto kPolicyUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000204");
constexpr auto kUnrelatedUuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000301");

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (result.ok) {
    return;
  }
  std::cerr << message << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "  " << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Fail(message);
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

api::EngineRequestContext Context(const std::filesystem::path& path) {
  namespace db = scratchbird::storage::database;
  namespace uuid = scratchbird::core::uuid;
  using scratchbird::core::platform::UuidKind;
  db::DatabaseCreateConfig create;
  create.path = path.string();
  const auto database = uuid::GenerateEngineIdentityV7(UuidKind::database, NowMillis());
  const auto filespace = uuid::GenerateEngineIdentityV7(UuidKind::filespace, NowMillis());
  Require(database.ok() && filespace.ok(), "catalog fixture identity generation");
  create.database_uuid = database.value;
  create.filespace_uuid = filespace.value;
  create.creation_unix_epoch_millis = NowMillis();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "catalog fixture native database creation");
  auto context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  context.request_id = "ipar-catalog-ddl-support-service-gate";
  context.current_schema_uuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000001");
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "catalog fixture real MGA begin");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  return context;
}

api::EngineLocalizedName Name(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

api::EngineObjectReference Object(api::EngineUuid uuid, std::string kind) {
  api::EngineObjectReference object;
  object.uuid = std::move(uuid);
  object.object_kind = std::move(kind);
  return object;
}

api::EngineColumnDefinition Column(std::string name, std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid =
      scratchbird::tests::FixtureUuid(1557, ordinal + 10);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int64";
  column.descriptor.encoded_descriptor = "canonical=int64";
  column.ordinal = ordinal;
  column.nullable = false;
  return column;
}

api::EngineConstraintDefinition ConstraintDefinition() {
  api::EngineConstraintDefinition constraint;
  constraint.requested_constraint_uuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000901");
  constraint.names.push_back(Name("ipar_support_constraint_stage"));
  constraint.constraint_kind = "unique_key";
  api::CatalogConstraintMetadata metadata;
  metadata.text = {{"constraint_hash", "stage-hash"}, {"support_family", "btree"}};
  metadata.identities = {{"support_uuid", scratchbird::tests::FixtureUuidLiteral(
      "019f4000-0000-7000-8000-000000000902")}};
  Require(api::EncodeCatalogConstraintMetadata(metadata, &constraint.canonical_constraint_envelope),
          "constraint fixture metadata encoding failed");
  return constraint;
}

api::EngineIndexDefinition IndexDefinition() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid = scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000903");
  index.names.push_back(Name("ipar_support_index_stage"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("id");
  return index;
}

void CreateCatalogObject(const api::EngineRequestContext& context,
                         api::EngineUuid uuid,
                         std::string kind,
                         api::EngineUuid schema_uuid,
                         std::string name,
                         std::vector<api::EngineObjectReference> related = {}) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object = Object(std::move(uuid), std::move(kind));
  request.target_schema.uuid = std::move(schema_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  request.related_objects = std::move(related);
  RequireOk(api::EngineCatalogCreateObject(request),
            "IPAR catalog support fixture create failed");
}

void CreateDependencyFixture(const api::EngineRequestContext& context) {
  CreateCatalogObject(context, kSchemaUuid, "schema", {}, "ipar_support_schema");
  CreateCatalogObject(context, kDomainUuid, "domain", kSchemaUuid, "ipar_support_domain");
  CreateCatalogObject(context,
                      kTableUuid,
                      "table",
                      kSchemaUuid,
                      "ipar_support_table",
                      {Object(kDomainUuid, "domain")});
  CreateCatalogObject(context,
                      kViewUuid,
                      "view",
                      kSchemaUuid,
                      "ipar_support_view",
                      {Object(kTableUuid, "table")});
  CreateCatalogObject(context,
                      kTriggerUuid,
                      "trigger",
                      kTableUuid,
                      "ipar_support_trigger",
                      {Object(kTableUuid, "table")});
  CreateCatalogObject(context,
                      kConstraintUuid,
                      "constraint",
                      kTableUuid,
                      "ipar_support_constraint",
                      {Object(kTableUuid, "table")});
  CreateCatalogObject(context,
                      kPolicyUuid,
                      "policy",
                      kTableUuid,
                      "ipar_support_policy",
                      {Object(kTableUuid, "table")});
  CreateCatalogObject(context, kUnrelatedUuid, "table", kSchemaUuid, "ipar_unrelated_table");
}

api::CatalogPinnedDescriptorCacheKey CacheKey(const api::EngineRequestContext& context,
                                              std::string family,
                                              std::vector<api::EngineUuid> objects) {
  api::CatalogPinnedDescriptorCacheKey key;
  key.descriptor_family = std::move(family);
  key.catalog_epoch = context.catalog_generation_id;
  key.security_epoch = context.security_epoch;
  key.resource_policy_epoch = context.resource_epoch;
  key.name_resolution_epoch = context.name_resolution_epoch;
  key.descriptor_set_digest = key.descriptor_family + ":digest";
  key.object_uuids = std::move(objects);
  key.security_policy_identity = "security:default";
  key.redaction_policy_identity = "redaction:default";
  key.resource_policy_identity = "resource:default";
  return key;
}

void PutDescriptorSnapshot(const api::CatalogPinnedDescriptorCacheKey& key,
                           api::EngineUuid object_uuid,
                           std::string object_kind) {
  api::CatalogPinnedDescriptorSnapshot snapshot;
  snapshot.key = key;
  snapshot.descriptor.descriptor_uuid = std::move(object_uuid);
  snapshot.descriptor.descriptor_kind = std::move(object_kind);
  snapshot.descriptor.canonical_type_name = snapshot.descriptor.descriptor_kind;
  snapshot.descriptor.encoded_descriptor = "immutable_descriptor=true";
  snapshot.primary_object.uuid = snapshot.descriptor.descriptor_uuid;
  snapshot.primary_object.object_kind = snapshot.descriptor.descriptor_kind;
  snapshot.descriptor_owner = snapshot.primary_object;
  snapshot.result_shape.result_kind = "descriptor";
  snapshot.read_only_snapshot = true;
  snapshot.security_recheck_required = true;
  snapshot.visibility_recheck_required = true;
  snapshot.finality_authority_cached = false;
  const auto put = api::GlobalCatalogPinnedDescriptorCache().Put(std::move(snapshot));
  Require(put.ok, "IPAR support descriptor cache put failed");
}

template <typename T, typename U>
bool Contains(const std::vector<T>& values, const U& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool ContainsCacheInvalidation(const std::vector<api::CatalogDdlCacheInvalidation>& values,
                               std::string_view family,
                               const api::EngineUuid& object_uuid) {
  for (const auto& value : values) {
    if (value.cache_family == family && value.object_uuid == object_uuid) {
      return true;
    }
  }
  return false;
}

std::set<std::string> StagedKinds(
    const std::vector<api::CatalogDdlStagedDescriptor>& staged) {
  std::set<std::string> kinds;
  for (const auto& descriptor : staged) {
    kinds.insert(descriptor.object.object_kind);
    Require(descriptor.validation_state == "validated",
            "IPAR staged descriptor was not validated");
    Require(descriptor.built_before_final_publish_lock,
            "IPAR staged descriptor was not prebuilt before final lock");
    Require(!descriptor.final_publish_lock_held,
            "IPAR staged descriptor held final publish lock during prebuild");
    Require(!descriptor.parser_sql_authority,
            "IPAR staged descriptor leaked parser SQL authority");
  }
  return kinds;
}

bool IdentityFieldEquals(const api::EngineRowValue& row, std::string_view name,
                         const api::EngineUuid& identity) {
  for (const auto& [field, value] : row.fields) {
    if (field != name) continue;
    return !value.isSqlNull() && value.encoded_value.empty() &&
           value.binary_value.size() == identity.bytes.size() &&
           std::equal(value.binary_value.begin(), value.binary_value.end(), identity.bytes.begin());
  }
  return false;
}

api::EngineRequestContext BeginFresh(const api::EngineRequestContext& base) {
  api::EngineBeginTransactionRequest begin;
  begin.context = base;
  begin.context.local_transaction_id = 0;
  begin.context.transaction_uuid = {};
  begin.context.snapshot_visible_through_local_transaction_id = 0;
  begin.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(begin);
  RequireOk(result, "catalog observer MGA begin");
  auto context = begin.context;
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  return context;
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  RequireOk(api::EngineRollbackTransaction(rollback), "catalog fixture durable rollback");
}

void CheckSchemaVisibility(const api::EngineRequestContext& context,
                           const api::EngineUuid& schema,
                           const api::EngineUuid& row,
                           std::string_view name, bool visible) {
  api::EngineApiDiagnostic diagnostic;
  const auto record = api::FindVisibleSchemaTreeRecord(
      context, schema, context.local_transaction_id, diagnostic);
  Require(!diagnostic.error, "schema readback refused");
  Require(record.has_value() == visible, "schema transaction visibility mismatch");
  if (record) {
    Require(record->state == "active" && record->default_name == name,
            "schema stored definition mismatch");
    api::BinaryCatalogMetadata metadata;
    std::vector<api::EngineLocalizedName> names;
    std::vector<std::pair<std::string, std::string>> comments;
    Require(api::DecodeSchemaTreeMetadata(record->payload, &names, &comments, &metadata),
            "schema persisted metadata decode");
    Require(api::BinaryCatalogUuid(metadata, "catalog_ddl_result_row_uuid") == row,
            "returned schema catalog row identity was not persisted");
  }
  const auto names = api::LoadNameRegistryState(context, context.local_transaction_id);
  Require(names.ok, "schema name readback refused");
  std::size_t count = 0;
  for (const auto& entry : names.state.entries) {
    if (entry.object_uuid != schema || entry.deleted) continue;
    ++count;
    Require(!entry.derived_from_legacy_name && entry.object_class == "schema" &&
                entry.raw_name_text == name &&
                scratchbird::core::uuid::IsEngineIdentityUuid(entry.name_entry_uuid),
            "schema must have actual binary-bound authoritative name record");
  }
  Require(count == (visible ? 1U : 0U), "schema name visibility/duplicate mismatch");
}

void ValidateDdlPublicationEffects(const api::EngineRequestContext& context) {
  // A result identity is not an executed publication receipt. Assert real
  // catalog/name reads and MGA finality instead of historical constant flags.
  const auto observer = BeginFresh(context);
  api::EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object = Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000777"), "schema");
  request.localized_names.push_back(Name("ipar_support_publication_schema"));
  const auto result = api::EngineCreateSchema(request);
  RequireOk(result, "IPAR DDL support create schema failed");
  Require(result.primary_object.uuid == request.target_object.uuid &&
              result.primary_object.object_kind == "schema" &&
              scratchbird::core::uuid::IsEngineIdentityUuid(result.catalog_row_uuid) &&
              result.catalog_row_uuid != request.target_object.uuid,
          "schema result identity binding");
  Require(std::any_of(result.result_shape.rows.begin(), result.result_shape.rows.end(),
      [&](const auto& row) { return IdentityFieldEquals(row, "object_uuid", request.target_object.uuid); }),
      "schema result object identity is not binary16");
  CheckSchemaVisibility(context, request.target_object.uuid, result.catalog_row_uuid,
                        request.localized_names.front().name, true);
  CheckSchemaVisibility(observer, request.target_object.uuid, result.catalog_row_uuid,
                        request.localized_names.front().name, false);
  const auto journal_sizes = [&] {
    return std::pair{
      std::filesystem::file_size(context.database_path + ".sb.api_events.v2"),
      std::filesystem::file_size(context.database_path + ".sb.name_events.v2")};
  };
  const auto before_refusals = journal_sizes();
  const auto duplicate = api::EngineCreateSchema(request);
  Require(!duplicate.ok && !duplicate.diagnostics.empty(), "duplicate schema must refuse");
  auto conflict = request;
  conflict.target_object.uuid = scratchbird::tests::FixtureUuid(910, 1);
  const auto name_conflict = api::EngineCreateSchema(conflict);
  Require(!name_conflict.ok && !name_conflict.diagnostics.empty(),
          "duplicate schema name must refuse before publication");
  auto recovery = request;
  recovery.target_object.uuid = scratchbird::tests::FixtureUuid(910, 2);
  recovery.localized_names = {Name("ipar_support_recovery_schema")};
  recovery.recovery_operation_uuid = scratchbird::tests::FixtureUuid(910, 3);
  recovery.requested_catalog_row_uuid = scratchbird::tests::FixtureUuid(910, 4);
  recovery.mutation_uuid = scratchbird::tests::FixtureUuid(910, 5);
  recovery.statement_publication_barrier_uuid = scratchbird::tests::FixtureUuid(910, 6);
  recovery.option_envelopes.push_back(std::string("catalog_ddl_mutation_audit:") +
      std::string(reinterpret_cast<const char*>(recovery.recovery_operation_uuid.bytes.data()), 16));
  for (int member = 0; member != 4; ++member) {
    for (const bool missing : {false, true}) {
      auto bad = recovery;
      auto* identity = member == 0 ? &bad.recovery_operation_uuid :
          member == 1 ? &bad.requested_catalog_row_uuid :
          member == 2 ? &bad.mutation_uuid : &bad.statement_publication_barrier_uuid;
      if (missing) *identity = {};
      else identity->bytes[6] = 0x40;  // UUIDv4 is valid data, not system authority.
      const auto refused = api::EngineCreateSchema(bad);
      Require(!refused.ok && !refused.diagnostics.empty(),
              "malformed or partial recovery tuple must refuse");
    }
  }
  Require(journal_sizes() == before_refusals, "refused schema request wrote catalog effects");
  CheckSchemaVisibility(context, request.target_object.uuid, result.catalog_row_uuid,
                        request.localized_names.front().name, true);
  const auto recovered = api::EngineCreateSchema(recovery);
  RequireOk(recovered, "exact schema recovery identity create");
  Require(recovered.catalog_row_uuid == recovery.requested_catalog_row_uuid,
          "schema recovery changed the requested binary row identity");
  const auto before_replay = journal_sizes();
  const auto replay = api::EngineCreateSchema(recovery);
  RequireOk(replay, "exact schema recovery replay");
  Require(replay.catalog_row_uuid == recovered.catalog_row_uuid &&
              journal_sizes() == before_replay, "schema recovery replay duplicated effects");
  CheckSchemaVisibility(context, recovery.target_object.uuid, recovered.catalog_row_uuid,
                        recovery.localized_names.front().name, true);
  auto conflicting_replay = recovery;
  conflicting_replay.requested_catalog_row_uuid = scratchbird::tests::FixtureUuid(910, 7);
  Require(!api::EngineCreateSchema(conflicting_replay).ok && journal_sizes() == before_replay,
          "conflicting schema replay changed durable identity");
  Rollback(context);
  Rollback(observer);
  const auto reopened = BeginFresh(context);
  CheckSchemaVisibility(reopened, request.target_object.uuid, result.catalog_row_uuid,
                        request.localized_names.front().name, false);
  CheckSchemaVisibility(reopened, recovery.target_object.uuid, recovered.catalog_row_uuid,
                        recovery.localized_names.front().name, false);
  request.context = reopened;
  request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral(
      "019f4000-0000-7000-8000-000000000778");
  const auto committed_schema = api::EngineCreateSchema(request);
  RequireOk(committed_schema, "schema create after rollback");
  Require(committed_schema.catalog_row_uuid != result.catalog_row_uuid,
          "a rolled-back catalog row identity must not be reused");
  api::EngineCommitTransactionRequest commit;
  commit.context = reopened;
  const auto committed = api::EngineCommitTransaction(commit);
  RequireOk(committed, "schema durable commit");
  Require(committed.engine_finality_known, "schema commit finality unknown");
  const auto reader = BeginFresh(context);
  CheckSchemaVisibility(reader, request.target_object.uuid, committed_schema.catalog_row_uuid,
                        request.localized_names.front().name, true);
  Rollback(reader);
}

void ValidateRcuPublisher() {
  auto& publisher = api::GlobalCatalogDdlImmutableSnapshotPublisher();
  publisher.Clear();

  api::CatalogDdlImmutableSnapshot first;
  first.catalog_epoch = 10;
  first.snapshot_digest = "first";
  first.object_uuids.push_back(kTableUuid);
  const auto published_first = publisher.Publish(first);
  const auto old_reader = publisher.AcquireLatest();
  Require(old_reader.snapshot && old_reader.generation == published_first.generation,
          "IPAR RCU reader did not acquire first snapshot");
  Require(publisher.ActiveReaders(published_first.generation) == 1,
          "IPAR RCU active reader count mismatch");

  api::CatalogDdlImmutableSnapshot second;
  second.catalog_epoch = 11;
  second.snapshot_digest = "second";
  second.object_uuids.push_back(kTableUuid);
  const auto published_second = publisher.Publish(second);
  const auto new_reader = publisher.AcquireLatest();
  Require(new_reader.snapshot && new_reader.generation == published_second.generation,
          "IPAR RCU reader did not acquire latest snapshot");
  publisher.Release(new_reader);

  Require(publisher.RetireUpTo(published_first.generation) == 0,
          "IPAR RCU retired snapshot with active reader");
  publisher.Release(old_reader);
  Require(publisher.RetireUpTo(published_first.generation) == 1,
          "IPAR RCU did not retire drained snapshot");
  Require(publisher.RetainedSnapshotCount() == 1,
          "IPAR RCU retained snapshot count mismatch");
}

api::EngineCatalogDdlSupportRequest SupportRequest(
    const api::EngineRequestContext& context,
    bool apply_invalidation) {
  api::EngineCatalogDdlSupportRequest request;
  request.context = context;
  request.operation_id = "catalog.ddl_support.ipar_gate";
  request.target_object = Object(kTableUuid, "table");
  request.mutation_source = "catalog_object_alter";
  request.apply_descriptor_cache_invalidation = apply_invalidation;
  request.stage_objects = {
      Object(kTableUuid, "table"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000401"), "index"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000402"), "view"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000403"), "procedure"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000404"), "package"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000405"), "trigger"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000406"), "constraint"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000407"), "filespace"),
      Object(scratchbird::tests::FixtureUuidLiteral("019f4000-0000-7000-8000-000000000408"), "policy"),
  };
  request.columns.push_back(Column("id", 0));
  scratchbird::tests::BindFixtureColumnDatatype(context,
      scratchbird::core::datatypes::CanonicalTypeId::int64, request.columns.back());
  request.indexes.push_back(IndexDefinition());
  request.constraints.push_back(ConstraintDefinition());

  api::CatalogDdlPreparedContextProof dependent;
  dependent.prepared_context_uuid = scratchbird::tests::FixtureUuid(1557, 1);
  dependent.dependent_object_uuids = {kTableUuid, kViewUuid};
  dependent.catalog_epoch = 100;
  dependent.security_epoch = context.security_epoch;
  dependent.policy_epoch = context.resource_epoch;
  request.prepared_contexts.push_back(std::move(dependent));

  api::CatalogDdlPreparedContextProof unrelated;
  unrelated.prepared_context_uuid = scratchbird::tests::FixtureUuid(1557, 2);
  unrelated.dependent_object_uuids = {kUnrelatedUuid};
  unrelated.catalog_epoch = 100;
  unrelated.security_epoch = context.security_epoch;
  unrelated.policy_epoch = context.resource_epoch;
  request.prepared_contexts.push_back(std::move(unrelated));
  return request;
}

void ValidateSupportService(const api::EngineRequestContext& context) {
  api::GlobalCatalogPinnedDescriptorCache().Clear();
  api::GlobalCatalogDdlDependencyClosureCache().Clear();
  api::GlobalCatalogDdlImmutableSnapshotPublisher().Clear();

  const auto table_key = CacheKey(context, "catalog_descriptor", {kTableUuid});
  const auto prepared_key =
      CacheKey(context, "prepared_authority_proof", {kViewUuid, kTableUuid});
  const auto unrelated_key =
      CacheKey(context, "catalog_descriptor", {kUnrelatedUuid});
  PutDescriptorSnapshot(table_key, kTableUuid, "table");
  PutDescriptorSnapshot(prepared_key, kViewUuid, "view");
  PutDescriptorSnapshot(unrelated_key, kUnrelatedUuid, "table");

  const auto result = api::EngineCatalogDdlSupportService(
      SupportRequest(context, true));
  RequireOk(result, "IPAR catalog DDL support service failed");
  Require(!result.dependency_closure.cache_hit,
          "IPAR closure cache unexpectedly hit on first build");
  Require(result.descriptor_cache_invalidated_entries == 2,
          "IPAR descriptor invalidation did not invalidate exact dependent entries");
  Require(result.immutable_snapshot_generation != 0,
          "IPAR immutable snapshot was not published by support service");

  const auto& affected = result.dependency_closure.affected_object_uuids;
  Require(Contains(affected, kTableUuid), "IPAR closure missed target table");
  Require(Contains(affected, kViewUuid), "IPAR closure missed dependent view");
  Require(Contains(affected, kTriggerUuid), "IPAR closure missed dependent trigger");
  Require(Contains(affected, kConstraintUuid), "IPAR closure missed dependent constraint");
  Require(Contains(affected, kPolicyUuid), "IPAR closure missed dependent policy");
  Require(!Contains(affected, kDomainUuid),
          "IPAR closure over-invalidated upstream dependency");
  Require(!Contains(affected, kUnrelatedUuid),
          "IPAR closure over-invalidated unrelated object");

  Require(ContainsCacheInvalidation(result.cache_invalidations,
                                    "descriptor_cache",
                                    kTableUuid),
          "IPAR impact predictor missed table descriptor cache");
  Require(ContainsCacheInvalidation(result.cache_invalidations,
                                    "plan_cache",
                                    kViewUuid),
          "IPAR impact predictor missed dependent view plan cache");
  Require(ContainsCacheInvalidation(result.cache_invalidations,
                                    "compiled_routine_cache",
                                    kTriggerUuid),
          "IPAR impact predictor missed trigger compiled routine cache");
  Require(!ContainsCacheInvalidation(result.cache_invalidations,
                                     "descriptor_cache",
                                     kUnrelatedUuid),
          "IPAR impact predictor over-invalidated unrelated descriptor cache");

  Require(result.prepared_context_invalidations.size() == 1,
          "IPAR prepared context invalidation count mismatch");
  Require(result.prepared_context_invalidations.front().prepared_context_uuid ==
              scratchbird::tests::FixtureUuid(1557, 1),
          "IPAR prepared context invalidation targeted wrong context");

  const auto staged_kinds = StagedKinds(result.staged_descriptors);
  for (const std::string expected : {"table",
                                     "index",
                                     "view",
                                     "procedure",
                                     "package",
                                     "trigger",
                                     "constraint",
                                     "filespace",
                                     "policy"}) {
    Require(staged_kinds.count(expected) != 0,
            "IPAR staged descriptor family missing");
  }

  Require(result.publish_plan.validation_before_publish,
          "IPAR publish plan skipped pre-publish validation");
  Require(result.publish_plan.prebuild_before_final_lock,
          "IPAR publish plan skipped descriptor prebuild before final lock");
  Require(result.publish_plan.final_publish_short_section,
          "IPAR publish plan did not minimize final publish section");
  Require(!result.publish_plan.partial_state_visible,
          "IPAR publish plan allowed partial state visibility");
  Require(result.publish_plan.uuid_returning_result,
          "IPAR publish plan did not reserve UUID-returning result");
  Require(result.publish_plan.rollback_recovery_authority ==
              "durable_transaction_inventory",
          "IPAR publish plan drifted from MGA finality authority");

  Require(api::GlobalCatalogPinnedDescriptorCache().Lookup(table_key).diagnostic_code ==
              "SB_CATALOG_PINNED_DESCRIPTOR_CACHE_MISS",
          "IPAR target descriptor cache entry survived invalidation");
  Require(api::GlobalCatalogPinnedDescriptorCache().Lookup(prepared_key).diagnostic_code ==
              "SB_CATALOG_PINNED_DESCRIPTOR_CACHE_MISS",
          "IPAR prepared descriptor cache entry survived invalidation");
  Require(api::GlobalCatalogPinnedDescriptorCache().Lookup(unrelated_key).cache_hit,
          "IPAR unrelated descriptor cache entry was invalidated");

  const auto second = api::EngineCatalogDdlSupportService(
      SupportRequest(context, false));
  RequireOk(second, "IPAR catalog DDL support service second call failed");
  Require(second.dependency_closure.cache_hit,
          "IPAR transitive closure cache did not reuse same epoch");

  auto changed_security = context;
  changed_security.security_epoch += 1;
  const auto third = api::EngineCatalogDdlSupportService(
      SupportRequest(changed_security, false));
  RequireOk(third, "IPAR catalog DDL support service security epoch call failed");
  Require(!third.dependency_closure.cache_hit,
          "IPAR transitive closure cache ignored security epoch change");
}

}  // namespace

int main() {
  try {
  namespace mem = scratchbird::core::memory;
  Require(mem::ConfigureDefaultMemoryManagerForFixture(mem::DefaultLocalEngineMemoryPolicy(),
      "ipar_catalog_ddl_support_service_gate").ok(), "catalog fixture memory policy");
  scratchbird::tests::OwnedTempDirectory cleanup;
  const auto context = Context(cleanup.path() / "catalog.sbdb");

  CreateDependencyFixture(context);
  ValidateSupportService(context);
  ValidateRcuPublisher();
  ValidateDdlPublicationEffects(context);
  cleanup.Cleanup();
  std::cout << "ipar_catalog_ddl_support_service_gate=passed\n";
  return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
