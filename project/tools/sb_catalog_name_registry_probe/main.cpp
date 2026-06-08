// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/name_resolution_api.hpp"
#include "ddl/create_api.hpp"
#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace sb = scratchbird::engine::internal_api;

namespace {

struct Probe {
  int failures = 0;

  void Check(bool condition, const std::string& message) {
    if (!condition) {
      ++failures;
      std::cerr << "FAIL: " << message << '\n';
    }
  }
};

std::string TempDatabasePath() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() /
          ("sb_catalog_name_registry_probe_" + std::to_string(stamp) + ".sbdb"))
      .string();
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

scratchbird::core::platform::TypedUuid GenerateTypedUuid(scratchbird::core::platform::UuidKind kind,
                                                         std::uint64_t unix_millis) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, unix_millis);
  if (!generated.ok()) { return {}; }
  return generated.value;
}

bool CreateProbeDatabase(Probe* probe,
                         const std::string& database_path,
                         scratchbird::core::platform::TypedUuid database_uuid,
                         scratchbird::core::platform::TypedUuid filespace_uuid,
                         std::uint64_t creation_millis) {
  std::error_code ignored;
  std::filesystem::remove(database_path, ignored);
  std::filesystem::remove(database_path + ".sb.owner.lock", ignored);

  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = database_path;
  create.database_uuid = database_uuid;
  create.filespace_uuid = filespace_uuid;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = creation_millis;
  const char* seed_root = std::getenv("SB_RESOURCE_SEED_PACK_ROOT");
  create.resource_seed_pack_root = seed_root == nullptr
      ? "project/resources/seed-packs/initial-resource-pack"
      : seed_root;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  const auto result = scratchbird::storage::database::CreateDatabaseFile(create);
  probe->Check(result.ok(), "create real probe database");
  return result.ok();
}

sb::EngineRequestContext BaseContext(const std::string& database_path, const std::string& database_uuid) {
  sb::EngineRequestContext context;
  context.database_path = database_path;
  context.trust_mode = sb::EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-000000000001";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-000000000002";
  context.database_uuid.canonical = database_uuid;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

sb::EngineLocalizedName Name(std::string text,
                             std::string profile,
                             bool quoted = false,
                             std::string language = "en") {
  sb::EngineLocalizedName name;
  name.language_tag = std::move(language);
  name.name_class = "primary";
  name.path = text;
  name.name = text;
  name.default_name = true;
  name.identifier_profile_uuid = std::move(profile);
  name.raw_name_text = text;
  name.display_name = text;
  name.was_quoted = quoted;
  name.quote_style = quoted ? "double_quote" : "";
  name.requires_exact_match = quoted;
  return name;
}

sb::EngineDescriptor ScalarDescriptor(std::string name) {
  sb::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "018f0000-0000-7000-8000-00000000d001";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(name);
  descriptor.encoded_descriptor = "base_type=" + descriptor.canonical_type_name;
  return descriptor;
}

std::size_t CountCacheInvalidations(const std::string& database_path) {
  std::ifstream in(database_path, std::ios::binary);
  std::size_t count = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("SBNAME1\tCACHE_INVALIDATE\t", 0) == 0) { ++count; }
  }
  return count;
}

bool CreateSchema(Probe* probe,
                  const sb::EngineRequestContext& context,
                  const std::string& schema_uuid,
                  const std::string& schema_name) {
  sb::EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object.uuid.canonical = schema_uuid;
  request.target_object.object_kind = "schema";
  request.localized_names.push_back(Name(schema_name, "sbsql_v3"));
  const auto result = sb::EngineCreateSchema(request);
  probe->Check(result.ok, "create schema " + schema_name);
  return result.ok;
}

bool CreateDomain(Probe* probe,
                  const sb::EngineRequestContext& context,
                  const std::string& schema_uuid,
                  const std::string& domain_uuid,
                  const sb::EngineLocalizedName& domain_name) {
  sb::EngineCreateDomainRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = domain_uuid;
  request.target_object.object_kind = "domain";
  request.localized_names.push_back(domain_name);
  request.descriptors.push_back(ScalarDescriptor("int32"));
  const auto result = sb::EngineCreateDomain(request);
  probe->Check(result.ok, "create domain " + domain_uuid);
  return result.ok;
}

sb::EngineResolveNameRequest ResolveDomainRequest(const sb::EngineRequestContext& context,
                                                  const std::string& schema_uuid,
                                                  const std::string& raw_name,
                                                  const std::string& profile,
                                                  bool quoted);

sb::EngineResolveNameResult ResolveDomain(const sb::EngineRequestContext& context,
                                          const std::string& schema_uuid,
                                          const std::string& raw_name,
                                          const std::string& profile,
                                          bool quoted = false) {
  return sb::EngineResolveName(ResolveDomainRequest(context, schema_uuid, raw_name, profile, quoted));
}

sb::EngineResolveNameRequest ResolveDomainRequest(const sb::EngineRequestContext& context,
                                                  const std::string& schema_uuid,
                                                  const std::string& raw_name,
                                                  const std::string& profile,
                                                  bool quoted = false) {
  sb::EngineResolveNameRequest request;
  request.context = context;
  request.context.current_schema_uuid.canonical = schema_uuid;
  request.context.identifier_profile_uuid = profile;
  request.target_schema.uuid.canonical = schema_uuid;
  request.target_schema.object_kind = "schema";
  request.sql_object_reference.expected_object_type = "domain";
  request.sql_object_reference.path_type = "current_relative";
  request.sql_object_reference.object_name.raw_text = raw_name;
  request.sql_object_reference.object_name.identifier_profile_uuid = profile;
  request.sql_object_reference.object_name.was_quoted = quoted;
  request.sql_object_reference.object_name.quote_style = quoted ? "double_quote" : "";
  request.sql_object_reference.object_name.requires_exact_match = quoted;
  return request;
}

void ExpectResolved(Probe* probe,
                    const sb::EngineRequestContext& context,
                    const std::string& schema_uuid,
                    const std::string& raw_name,
                    const std::string& profile,
                    const std::string& expected_uuid,
                    bool quoted = false) {
  const auto result = ResolveDomain(context, schema_uuid, raw_name, profile, quoted);
  probe->Check(result.ok, "resolve " + raw_name + " using " + profile);
  if (result.ok) {
    probe->Check(result.primary_object.uuid.canonical == expected_uuid,
                 "resolve " + raw_name + " returned expected object UUID");
    probe->Check(result.primary_object.object_kind == "domain",
                 "resolve " + raw_name + " returned domain object kind");
  }
}

void ExpectNotResolved(Probe* probe,
                       const sb::EngineRequestContext& context,
                       const std::string& schema_uuid,
                       const std::string& raw_name,
                       const std::string& profile,
                       bool quoted = false) {
  const auto result = ResolveDomain(context, schema_uuid, raw_name, profile, quoted);
  probe->Check(!result.ok, "name should not resolve: " + raw_name + " using " + profile);
}

void ExpectMappedName(Probe* probe,
                      const sb::EngineRequestContext& context,
                      const std::string& object_uuid,
                      const std::string& object_kind,
                      const std::string& expected_name) {
  sb::EngineMapUuidToNameRequest request;
  request.context = context;
  request.target_object.uuid.canonical = object_uuid;
  request.target_object.object_kind = object_kind;
  const auto result = sb::EngineMapUuidToName(request);
  probe->Check(result.ok, "map uuid to visible name " + object_uuid);
  if (result.ok) {
    probe->Check(!result.result_shape.rows.empty(), "uuid-to-name result has a row");
    if (!result.result_shape.rows.empty()) {
      bool found_name = false;
      for (const auto& field : result.result_shape.rows.front().fields) {
        if (field.first == "name" && field.second.encoded_value == expected_name) { found_name = true; }
      }
      probe->Check(found_name, "uuid-to-name returned expected display name " + expected_name);
    }
  }
}

}  // namespace

int main() {
  Probe probe;
  const std::string database_path = TempDatabasePath();
  const std::uint64_t creation_millis = CurrentUnixMillis();
  const auto database_uuid =
      GenerateTypedUuid(scratchbird::core::platform::UuidKind::database, creation_millis);
  const auto filespace_uuid =
      GenerateTypedUuid(scratchbird::core::platform::UuidKind::filespace, creation_millis + 1);
  probe.Check(database_uuid.valid(), "generate database uuid");
  probe.Check(filespace_uuid.valid(), "generate filespace uuid");
  if (!database_uuid.valid() || !filespace_uuid.valid() ||
      !CreateProbeDatabase(&probe, database_path, database_uuid, filespace_uuid, creation_millis)) {
    std::filesystem::remove(database_path);
    std::filesystem::remove(database_path + ".sb.owner.lock");
    return 1;
  }

  auto context = BaseContext(database_path, scratchbird::core::uuid::UuidToString(database_uuid.value));
  sb::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "snapshot";
  const auto begin_result = sb::EngineBeginTransaction(begin);
  probe.Check(begin_result.ok, "begin transaction");
  if (!begin_result.ok) {
    std::filesystem::remove(database_path);
    return 1;
  }
  context.local_transaction_id = begin_result.local_transaction_id;
  context.transaction_uuid = begin_result.transaction_uuid;

  const std::string schema_uuid = "018f0000-0000-7000-8000-00000000a001";
  const std::string sbsql_domain_uuid = "018f0000-0000-7000-8000-00000000b001";
  const std::string postgresql_domain_uuid = "018f0000-0000-7000-8000-00000000b002";
  const std::string firebird_domain_uuid = "018f0000-0000-7000-8000-00000000b003";
  const std::string mysql_domain_uuid = "018f0000-0000-7000-8000-00000000b004";
  const std::string quoted_domain_uuid = "018f0000-0000-7000-8000-00000000b005";

  CreateSchema(&probe, context, schema_uuid, "app");
  context.current_schema_uuid.canonical = schema_uuid;

  CreateDomain(&probe, context, schema_uuid, sbsql_domain_uuid, Name("FooBar", "sbsql_v3"));
  CreateDomain(&probe, context, schema_uuid, postgresql_domain_uuid, Name("MiXeD_pg", "postgresql_family"));
  CreateDomain(&probe, context, schema_uuid, firebird_domain_uuid, Name("FireMix", "firebird_dialect_3"));
  CreateDomain(&probe, context, schema_uuid, mysql_domain_uuid, Name("CamelThing", "mysql_case_insensitive"));
  CreateDomain(&probe, context, schema_uuid, quoted_domain_uuid, Name("ExactCase", "sbsql_v3", true));
  const std::size_t initial_invalidations = CountCacheInvalidations(database_path);
  probe.Check(initial_invalidations >= 6, "name creation emits parser-cache invalidation evidence");

  ExpectResolved(&probe, context, schema_uuid, "foobar", "sbsql_v3", sbsql_domain_uuid);
  ExpectResolved(&probe, context, schema_uuid, "MIXED_PG", "postgresql_family", postgresql_domain_uuid);
  ExpectResolved(&probe, context, schema_uuid, "firemix", "firebird_dialect_3", firebird_domain_uuid);
  ExpectResolved(&probe, context, schema_uuid, "camelthing", "mysql_case_insensitive", mysql_domain_uuid);
  ExpectResolved(&probe, context, schema_uuid, "ExactCase", "sbsql_v3", quoted_domain_uuid, true);
  ExpectNotResolved(&probe, context, schema_uuid, "exactcase", "sbsql_v3");
  ExpectMappedName(&probe, context, postgresql_domain_uuid, "domain", "MiXeD_pg");

  auto private_request = ResolveDomainRequest(context, schema_uuid, "firemix", "firebird_dialect_3");
  private_request.policy_profile.encoded_profiles.push_back("metadata_visibility:hide_object:" + firebird_domain_uuid);
  const auto hidden_public_result = sb::EngineResolveName(private_request);
  probe.Check(!hidden_public_result.ok, "public resolver hides policy-hidden object");
  const auto private_result = sb::ResolveNameRegistryPrivate(private_request, "domain");
  probe.Check(private_result.ok, "private resolver still sees policy-hidden engine candidate");
  if (private_result.ok) {
    probe.Check(private_result.matches.front().object_uuid == firebird_domain_uuid,
                "private resolver returned hidden object UUID");
  }
  sb::EngineMapUuidToNameRequest hidden_map_request;
  hidden_map_request.context = context;
  hidden_map_request.target_object.uuid.canonical = firebird_domain_uuid;
  hidden_map_request.target_object.object_kind = "domain";
  hidden_map_request.policy_profile.encoded_profiles.push_back("metadata_visibility:hide_object:" + firebird_domain_uuid);
  const auto hidden_map_public = sb::EngineMapUuidToName(hidden_map_request);
  probe.Check(!hidden_map_public.ok, "public uuid-to-name mapper hides policy-hidden object");
  const auto hidden_map_private =
      sb::MapNameRegistryUuidToNamePrivate(hidden_map_request, firebird_domain_uuid, "domain");
  probe.Check(hidden_map_private.ok, "private uuid-to-name mapper still sees policy-hidden object name");

  auto de_context = context;
  de_context.language_context.language_tag = "de";
  de_context.language_context.default_language_tag = "en";
  ExpectResolved(&probe, de_context, schema_uuid, "foobar", "sbsql_v3", sbsql_domain_uuid);

  const auto retire_for_rename =
      sb::RetireNameRegistryEntriesForObject(context, "probe.rename_domain_name", sbsql_domain_uuid);
  probe.Check(!retire_for_rename.error, "retire old name entries for rename");
  const auto persist_renamed = sb::PersistNameRegistryEntriesForObject(
      context,
      "probe.rename_domain_name",
      sbsql_domain_uuid,
      "domain",
      schema_uuid,
      std::vector<sb::EngineLocalizedName>{Name("RenamedDomain", "sbsql_v3")},
      "RenamedDomain");
  probe.Check(!persist_renamed.error, "persist renamed name entry");
  const std::size_t rename_invalidations = CountCacheInvalidations(database_path);
  probe.Check(rename_invalidations > initial_invalidations, "rename emits parser-cache invalidation evidence");
  ExpectNotResolved(&probe, context, schema_uuid, "foobar", "sbsql_v3");
  ExpectResolved(&probe, context, schema_uuid, "renameddomain", "sbsql_v3", sbsql_domain_uuid);

  const auto retire_for_drop =
      sb::RetireNameRegistryEntriesForObject(context, "probe.drop_domain_name", mysql_domain_uuid);
  probe.Check(!retire_for_drop.error, "retire name entries for drop");
  const std::size_t drop_invalidations = CountCacheInvalidations(database_path);
  probe.Check(drop_invalidations > rename_invalidations, "drop emits parser-cache invalidation evidence");
  ExpectNotResolved(&probe, context, schema_uuid, "camelthing", "mysql_case_insensitive");

  const std::size_t scale_base_invalidations = CountCacheInvalidations(database_path);
  for (int i = 0; i < 96; ++i) {
    const std::string object_uuid = "018f0000-0000-7000-8000-00000001" + std::to_string(1000 + i);
    const std::string object_name = "ScaleDomain_" + std::to_string(i);
    CreateDomain(&probe, context, schema_uuid, object_uuid, Name(object_name, "sbsql_v3"));
  }
  ExpectResolved(&probe,
                 context,
                 schema_uuid,
                 "scaledomain_95",
                 "sbsql_v3",
                 "018f0000-0000-7000-8000-000000011095");
  const std::size_t scale_after_invalidations = CountCacheInvalidations(database_path);
  probe.Check(scale_after_invalidations >= scale_base_invalidations + 96,
              "scale case emits invalidation evidence for each created name");

  std::filesystem::remove(database_path);
  if (probe.failures != 0) {
    std::cerr << probe.failures << " catalog name registry probe checks failed\n";
    return 1;
  }
  return 0;
}
