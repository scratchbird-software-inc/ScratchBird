// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/ddl_support_service.hpp"
#include "catalog/pinned_descriptor_cache.hpp"
#include "ddl/create_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

constexpr const char* kSchemaUuid = "019f4000-0000-7000-8000-000000000001";
constexpr const char* kTableUuid = "019f4000-0000-7000-8000-000000000101";
constexpr const char* kDomainUuid = "019f4000-0000-7000-8000-000000000102";
constexpr const char* kViewUuid = "019f4000-0000-7000-8000-000000000201";
constexpr const char* kTriggerUuid = "019f4000-0000-7000-8000-000000000202";
constexpr const char* kConstraintUuid = "019f4000-0000-7000-8000-000000000203";
constexpr const char* kPolicyUuid = "019f4000-0000-7000-8000-000000000204";
constexpr const char* kUnrelatedUuid = "019f4000-0000-7000-8000-000000000301";

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
  if (result.ok) {
    return;
  }
  std::cerr << message << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "  " << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  std::exit(EXIT_FAILURE);
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TempPath() {
  return std::filesystem::temp_directory_path() /
         ("sb_ipar_catalog_ddl_support_" + std::to_string(NowMillis()) + "_" +
          std::to_string(static_cast<long long>(getpid())) + ".sbdb");
}

void RemoveSidecars(const std::filesystem::path& path) {
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".sb.catalog_object_events");
  std::filesystem::remove(path.string() + ".sb.api_events");
  std::filesystem::remove(path.string() + ".sb.schema_tree");
  std::filesystem::remove(path.string() + ".sb.schema_tree_events");
  std::filesystem::remove(path.string() + ".sb.name_registry_events");
}

api::EngineRequestContext Context(const std::filesystem::path& path) {
  api::EngineRequestContext context;
  context.request_id = "ipar-catalog-ddl-support-service-gate";
  context.database_path = path.string();
  context.database_uuid.canonical = "019f4000-0000-7000-8000-00000000db01";
  context.principal_uuid.canonical = "019f4000-0000-7000-8000-00000000aa01";
  context.session_uuid.canonical = "019f4000-0000-7000-8000-00000000bb01";
  context.transaction_uuid.canonical = "019f4000-0000-7000-8000-00000000cc01";
  context.current_schema_uuid.canonical = kSchemaUuid;
  context.local_transaction_id = 42;
  context.snapshot_visible_through_local_transaction_id = 42;
  context.catalog_generation_id = 7;
  context.security_epoch = 11;
  context.resource_epoch = 13;
  context.name_resolution_epoch = 17;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
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

api::EngineObjectReference Object(std::string uuid, std::string kind) {
  api::EngineObjectReference object;
  object.uuid.canonical = std::move(uuid);
  object.object_kind = std::move(kind);
  return object;
}

api::EngineColumnDefinition Column(std::string name, std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical =
      "019f4000-0000-7000-8000-00000000c" + std::to_string(ordinal + 10);
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
  constraint.requested_constraint_uuid.canonical =
      "019f4000-0000-7000-8000-000000000901";
  constraint.names.push_back(Name("ipar_support_constraint_stage"));
  constraint.constraint_kind = "unique_key";
  constraint.canonical_constraint_envelope =
      "constraint_hash=stage-hash;support_uuid=019f4000-0000-7000-8000-000000000902;"
      "support_family=btree";
  return constraint;
}

api::EngineIndexDefinition IndexDefinition() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical =
      "019f4000-0000-7000-8000-000000000903";
  index.names.push_back(Name("ipar_support_index_stage"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("id");
  return index;
}

void CreateCatalogObject(const api::EngineRequestContext& context,
                         std::string uuid,
                         std::string kind,
                         std::string schema_uuid,
                         std::string name,
                         std::vector<api::EngineObjectReference> related = {}) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object = Object(std::move(uuid), std::move(kind));
  request.target_schema.uuid.canonical = std::move(schema_uuid);
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
                                              std::vector<std::string> objects) {
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
                           std::string object_uuid,
                           std::string object_kind) {
  api::CatalogPinnedDescriptorSnapshot snapshot;
  snapshot.key = key;
  snapshot.descriptor.descriptor_uuid.canonical = std::move(object_uuid);
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

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool ContainsCacheInvalidation(const std::vector<api::CatalogDdlCacheInvalidation>& values,
                               std::string_view family,
                               std::string_view object_uuid) {
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

std::string FieldValue(const api::EngineRowValue& row, std::string_view field_name) {
  for (const auto& field : row.fields) {
    if (field.first == field_name) {
      return field.second.encoded_value;
    }
  }
  return {};
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) {
      return true;
    }
  }
  return false;
}

void ValidateDdlPublicationOptimizationEvidence(const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object = Object("019f4000-0000-7000-8000-000000000777", "schema");
  request.localized_names.push_back(Name("ipar_support_publication_schema"));
  const auto result = api::EngineCreateSchema(request);
  RequireOk(result, "IPAR DDL support create schema failed");
  Require(HasEvidence(result, "ddl_validation_before_publish", "true"),
          "IPAR DDL validation-before-publish evidence missing");
  Require(HasEvidence(result, "ddl_prebuild_before_final_lock", "true"),
          "IPAR DDL prebuild-before-lock evidence missing");
  Require(HasEvidence(result, "ddl_epoch_invalidation", "catalog:schema_tree"),
          "IPAR DDL epoch invalidation evidence missing");
  Require(HasEvidence(result, "ddl_rollback_recovery_authority", "durable_transaction_inventory"),
          "IPAR DDL rollback recovery authority evidence missing");
  Require(HasEvidence(result, "ddl_uuid_result_shape", "uuid_first"),
          "IPAR DDL UUID result shape evidence missing");
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "ddl_result_object_uuid") !=
        request.target_object.uuid.canonical) {
      continue;
    }
    Require(FieldValue(row, "ddl_validation_before_publish") == "true",
            "IPAR DDL publication row validation-before-publish mismatch");
    Require(FieldValue(row, "ddl_prebuild_before_final_lock") == "true",
            "IPAR DDL publication row prebuild-before-lock mismatch");
    Require(FieldValue(row, "ddl_final_publish_lock_scope") ==
                "catalog_epoch_publish_only",
            "IPAR DDL publication row final lock scope mismatch");
    Require(FieldValue(row, "ddl_epoch_invalidation") == "catalog:schema_tree",
            "IPAR DDL publication row epoch invalidation mismatch");
    Require(FieldValue(row, "ddl_dependency_proof") == "schema_tree",
            "IPAR DDL publication row dependency proof mismatch");
    Require(FieldValue(row, "ddl_rollback_recovery_authority") ==
                "durable_transaction_inventory",
            "IPAR DDL publication row rollback authority mismatch");
    return;
  }
  Fail("IPAR DDL publication optimization row missing");
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
      Object("019f4000-0000-7000-8000-000000000401", "index"),
      Object("019f4000-0000-7000-8000-000000000402", "view"),
      Object("019f4000-0000-7000-8000-000000000403", "procedure"),
      Object("019f4000-0000-7000-8000-000000000404", "package"),
      Object("019f4000-0000-7000-8000-000000000405", "trigger"),
      Object("019f4000-0000-7000-8000-000000000406", "constraint"),
      Object("019f4000-0000-7000-8000-000000000407", "filespace"),
      Object("019f4000-0000-7000-8000-000000000408", "policy"),
  };
  request.columns.push_back(Column("id", 0));
  request.indexes.push_back(IndexDefinition());
  request.constraints.push_back(ConstraintDefinition());

  api::CatalogDdlPreparedContextProof dependent;
  dependent.prepared_context_uuid = "prepared-context-dependent";
  dependent.dependent_object_uuids = {kTableUuid, kViewUuid};
  dependent.catalog_epoch = 100;
  dependent.security_epoch = context.security_epoch;
  dependent.policy_epoch = context.resource_epoch;
  request.prepared_contexts.push_back(std::move(dependent));

  api::CatalogDdlPreparedContextProof unrelated;
  unrelated.prepared_context_uuid = "prepared-context-unrelated";
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
              "prepared-context-dependent",
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
  const auto path = TempPath();
  RemoveSidecars(path);
  const auto context = Context(path);

  CreateDependencyFixture(context);
  ValidateSupportService(context);
  ValidateRcuPublisher();
  ValidateDdlPublicationOptimizationEvidence(context);

  RemoveSidecars(path);
  std::cout << "ipar_catalog_ddl_support_service_gate=passed\n";
  return EXIT_SUCCESS;
}
