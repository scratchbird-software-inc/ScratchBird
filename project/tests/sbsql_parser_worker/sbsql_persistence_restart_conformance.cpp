// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "catalog/catalog_lookup_api.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/select_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "management/config_api.hpp"
#include "memory.hpp"
#include "notification/notification_api.hpp"
#include "security/identity_api.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include "canonical_sblr_admission_test_helper.hpp"

#include "../database_lifecycle/credentialed_database_fixture.hpp"
#include "../release/public_release_authz_fixture.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;

#ifndef SB_FSP011D_SEED_PACK_ROOT
#define SB_FSP011D_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_persistence_restart_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_persistence_restart_conformance");
  Require(configured.ok(), "FSPE-011D memory fixture configuration failed");
  Require(configured.fixture_mode, "FSPE-011D memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbsql_persistence_restart.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field_name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name) { return field.second.encoded_value; }
    }
  }
  return {};
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind, std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool HasFieldValue(const api::EngineApiResult& result,
                   std::string_view field_name,
                   std::string_view expected_value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name && field.second.encoded_value == expected_value) {
        return true;
      }
    }
  }
  return false;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "fspe011d-persistence-restart";
  context.database_path = database_path.string();
  context.database_uuid.canonical = "019e078d-f11d-7000-8000-000000000001";
  context.principal_uuid.canonical = "019e078d-f11d-7000-8000-000000000002";
  context.session_uuid.canonical = "019e078d-f11d-7000-8000-000000000003";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.trace_tags.push_back("FSPE-011D");
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:OBS_AGENT_STATE_READ");
  context.trace_tags.push_back("right:OBS_AGENT_CONTROL");
  context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  return context;
}

api::EngineRequestContext GrantAdminContext(api::EngineRequestContext context) {
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019e078d-f11d-7000-8000-00000000a001";
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = context.resource_epoch;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back(
      {context.principal_uuid, "principal"});
  api::EngineMaterializedAuthorizationGrant grant_admin;
  grant_admin.grant_uuid.canonical = "019e078d-f11d-7000-8000-00000000a102";
  grant_admin.subject_uuid = context.principal_uuid;
  grant_admin.subject_kind = "principal";
  grant_admin.right = "SEC_GRANT_ADMIN";
  grant_admin.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(grant_admin));
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  auto envelope =
      scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
          operation_id, opcode, "FSPE-011D");
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

std::string ServerAdmissionEnvelope(std::string_view operation_id) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=FSPE-011D\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

void RequireServerAdmitted(std::string_view operation_id,
                           std::string_view opcode) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(
          operation_id, opcode));
  if (!admission.admitted) {
    std::cerr << "server admission rejected " << operation_id << '\n';
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message_key << '\n';
    }
  }
  Require(admission.admitted, "server SBLR admission rejected operation");
}

void VerifyServerRejectsSqlText() {
  std::string encoded = ServerAdmissionEnvelope("dml.select_rows");
  const auto marker = encoded.find("contains_sql_text=false");
  Require(marker != std::string::npos, "server admission envelope missing SQL text marker");
  encoded.replace(marker, std::string("contains_sql_text=false").size(), "contains_sql_text=true");
  const auto rejected = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{encoded, false});
  Require(!rejected.admitted, "server admission accepted SQL-text-marked SBLR");
}

sblr::SblrDispatchResult Dispatch(const std::filesystem::path& database_path,
                                  const std::string& operation_id,
                                  const std::string& opcode,
                                  api::EngineRequestContext context,
                                  api::EngineApiRequest request = {},
                                  bool requires_transaction = false) {
  RequireServerAdmitted(operation_id, opcode);
  auto envelope = Envelope(operation_id, opcode);
  envelope.requires_transaction_context = requires_transaction;
  request.context = context;
  request.operation_id = operation_id;
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = std::move(context);
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = std::move(request);
  auto result = sblr::DispatchSblrOperation(dispatch);
  if (!result.accepted || !result.envelope_validated || !result.dispatched_to_api || !result.api_result.ok) {
    std::cerr << "dispatch failed for " << operation_id << " path=" << database_path << '\n'
              << sblr::SerializeSblrDispatchResultToJson(result);
  }
  return result;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name, std::string type) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = "019e078d-f11d-7000-8000-00000000010" + std::to_string(ordinal);
  column.names.push_back({"en", "primary", name, name, true});
  column.descriptor.descriptor_uuid.canonical = "019e078d-f11d-7000-8000-00000000020" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor = "type=" + column.descriptor.canonical_type_name;
  return column;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineDescriptor ScalarDescriptor(std::string uuid, std::string type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = std::move(uuid);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineIndexDefinition BtreeIndex(std::string uuid, std::string name, std::string column) {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = std::move(uuid);
  index.names.push_back(Name(std::move(name)));
  index.index_kind = "btree";
  index.key_envelopes.push_back(std::move(column));
  return index;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = "019e078d-f11d-7000-8000-000000000301";
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

void RequireLookup(const std::filesystem::path& database_path,
                   const api::EngineRequestContext& context,
                   std::string uuid,
                   std::string kind) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = kind;
  const auto lookup = Dispatch(database_path,
                               "catalog.lookup_object",
                               "SBLR_CATALOG_LOOKUP_OBJECT",
                               context,
                               request,
                               true);
  Require(lookup.api_result.primary_object.uuid.canonical == uuid, "catalog lookup did not find persisted object after restart");
  Require(lookup.api_result.primary_object.object_kind == kind, "catalog lookup returned wrong persisted object kind after restart");
}

void RequireDescriptorContains(const std::filesystem::path& database_path,
                               const api::EngineRequestContext& context,
                               std::string uuid,
                               std::string needle) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = uuid;
  const auto descriptor = Dispatch(database_path,
                                   "catalog.get_descriptor",
                                   "SBLR_CATALOG_GET_DESCRIPTOR",
                                   context,
                                   request,
                                   true);
  Require(!descriptor.api_result.result_shape.columns.empty(), "descriptor lookup returned no descriptor after restart");
  if (descriptor.api_result.result_shape.columns.front().encoded_descriptor.find(needle) == std::string::npos) {
    std::cerr << "descriptor payload for " << uuid << ": "
              << descriptor.api_result.result_shape.columns.front().encoded_descriptor
              << " missing=" << needle << '\n';
  }
  Require(descriptor.api_result.result_shape.columns.front().encoded_descriptor.find(needle) != std::string::npos,
          "descriptor lookup did not preserve expected descriptor payload after restart");
}

void VerifyOpenedState(const db::DatabaseLifecycleResult& opened) {
  if (!opened.ok()) {
    std::cerr << "database reopen diagnostic: "
              << opened.diagnostic.diagnostic_code << " "
              << opened.diagnostic.message_key << " "
              << opened.diagnostic.remediation_hint << '\n';
  }
  Require(opened.ok(), "database reopen failed");
  Require(db::DatabaseLifecyclePhaseName(opened.state.phase) == std::string("opened"),
          "database did not reopen into opened phase");
  Require(opened.state.startup_recovery_classification == "clean_checkpoint_path",
          "database did not reopen on clean checkpoint path");
  Require(!opened.state.write_admission_fenced, "clean restart left write admission fenced");
  Require(opened.state.typed_catalog_records_present, "typed catalog records did not survive reopen");
  Require(opened.state.typed_catalog_record_count >= 20, "typed catalog record count too low after reopen");
  Require(opened.state.resource_seed_catalog_present, "resource seed catalog did not survive reopen");
  Require(opened.state.resource_seed_catalog.active, "resource seed catalog reopened as inactive");
  Require(!opened.state.resource_seed_catalog.minimal_bootstrap, "restart used minimal bootstrap resource catalog");
  Require(opened.state.resource_seed_catalog.seed_pack_name == "initial-resource-pack",
          "unexpected seed pack survived reopen");
  Require(opened.state.resource_seed_catalog.resource_artifact_records > 0,
          "resource seed artifact records did not survive reopen");
  Require(opened.state.database_uuid.valid(), "database UUID invalid after reopen");
  Require(opened.state.filespace_uuid.valid(), "filespace UUID invalid after reopen");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  const auto work = MakeTempDir();
  Require(!work.empty(), "failed to create temp directory");
  const auto database_path = work / "fspe011d.sbdb";
  VerifyServerRejectsSqlText();

  const auto created =
      scratchbird::tests::database_lifecycle::CreateCredentialedDatabaseFixture(
          database_path, SB_FSP011D_SEED_PACK_ROOT);
  Require(created.ok(), "credentialed lifecycle fixture database create failed");
  Require(std::filesystem::exists(database_path), "lifecycle create did not create database file");
  const std::string created_database_uuid =
      scratchbird::core::uuid::UuidToString(created.state.database_uuid.value);
  const std::string created_filespace_uuid =
      scratchbird::core::uuid::UuidToString(created.state.filespace_uuid.value);
  Require(!created_database_uuid.empty(), "created database UUID was not reported");
  Require(!created_filespace_uuid.empty(), "created filespace UUID was not reported");

  auto open_result = Dispatch(database_path,
                              "lifecycle.open_database",
                              "SBLR_LIFECYCLE_OPEN_DATABASE",
                              BaseContext(database_path));
  Require(open_result.api_result.ok, "SBLR lifecycle open failed");
  Require(FieldValue(open_result.api_result, "phase") == "opened", "SBLR open did not report opened phase");

  auto begin_write = Dispatch(database_path,
                              "transaction.begin",
                              "SBLR_TXN_BEGIN",
                              BaseContext(database_path));
  Require(begin_write.api_result.local_transaction_id != 0, "write transaction did not return local transaction id");

  auto write_context = BaseContext(database_path);
  write_context.local_transaction_id = begin_write.api_result.local_transaction_id;
  write_context.transaction_uuid = begin_write.api_result.transaction_uuid;
  write_context.snapshot_visible_through_local_transaction_id = begin_write.api_result.local_transaction_id;

  constexpr const char* kSchemaUuid = "019e078d-f11d-7000-8000-000000000101";
  constexpr const char* kTableUuid = "019e078d-f11d-7000-8000-000000000102";
  api::EngineApiRequest schema_request;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("fspe011d_schema"));
  auto schema = Dispatch(database_path,
                         "ddl.create_schema",
                         "SBLR_DDL_CREATE_SCHEMA",
                         write_context,
                         schema_request,
                         true);
  Require(schema.api_result.primary_object.uuid.canonical == kSchemaUuid, "schema create did not use server UUID");
  Require(HasEvidence(schema.api_result, "schema", kSchemaUuid), "schema persistence evidence missing");

  api::EngineApiRequest table_request;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.localized_names.push_back(Name("fspe011d_table"));
  table_request.columns.push_back(Column(0, "id", "text"));
  table_request.columns.push_back(Column(1, "note", "text"));
  auto table = Dispatch(database_path,
                        "ddl.create_table",
                        "SBLR_DDL_CREATE_TABLE",
                        write_context,
                        table_request,
                        true);
  Require(table.api_result.primary_object.uuid.canonical == kTableUuid, "table create did not use server UUID");
  Require(HasEvidence(table.api_result, "mga_relation_metadata", "table_create"), "table MGA evidence missing");

  api::EngineApiRequest insert_request;
  insert_request.target_object.uuid.canonical = kTableUuid;
  insert_request.target_object.object_kind = "table";
  insert_request.rows.push_back(Row("1", "persisted-across-restart"));
  auto insert = Dispatch(database_path,
                         "dml.insert_rows",
                         "SBLR_DML_INSERT_ROWS",
                         write_context,
                         insert_request,
                         true);
  Require(insert.api_result.result_shape.rows.size() == 1, "insert did not return one row");
  Require(HasEvidence(insert.api_result, "mga_row_store", "row_insert"), "insert row-store evidence missing");

  constexpr const char* kIndexUuid = "019e078d-f11d-7000-8000-000000000103";
  api::EngineApiRequest index_request;
  index_request.target_object.uuid.canonical = kTableUuid;
  index_request.target_object.object_kind = "table";
  index_request.indexes.push_back(BtreeIndex(kIndexUuid, "fspe011d_table_id_idx", "id"));
  api::EngineCreateIndexRequest create_index_request;
  static_cast<api::EngineApiRequest&>(create_index_request) = index_request;
  create_index_request.context = write_context;
  const auto index = api::EngineCreateIndex(create_index_request);
  Require(index.ok && index.primary_object.uuid.canonical == kIndexUuid,
          "index create did not use server UUID");
  Require(HasEvidence(index, "mga_relation_metadata", "index_create"),
          "index MGA evidence missing");

  api::EngineApiRequest indexed_insert_request;
  indexed_insert_request.target_object.uuid.canonical = kTableUuid;
  indexed_insert_request.target_object.object_kind = "table";
  indexed_insert_request.rows.push_back(Row("2", "persisted-through-index"));
  auto indexed_insert = Dispatch(database_path,
                                 "dml.insert_rows",
                                 "SBLR_DML_INSERT_ROWS",
                                 write_context,
                                 indexed_insert_request,
                                 true);
  Require(indexed_insert.api_result.result_shape.rows.size() == 1, "post-index insert did not return one row");
  Require(HasEvidence(indexed_insert.api_result, "mga_row_store", "row_insert"), "post-index insert row-store evidence missing");

  constexpr const char* kDomainUuid = "019e078d-f11d-7000-8000-000000000104";
  api::EngineApiRequest domain_request;
  domain_request.target_schema.uuid.canonical = kSchemaUuid;
  domain_request.target_schema.object_kind = "schema";
  domain_request.target_object.uuid.canonical = kDomainUuid;
  domain_request.target_object.object_kind = "domain";
  domain_request.localized_names.push_back(Name("fspe011d_domain"));
  domain_request.descriptors.push_back(ScalarDescriptor("019e078d-f11d-7000-8000-000000000204", "text"));
  domain_request.policy_profile.encoded_profiles.push_back("domain_visibility_policy:fspe011d_visible");
  domain_request.option_envelopes.push_back("check_constraint:not_empty");
  api::EngineCreateDomainRequest create_domain_request;
  static_cast<api::EngineApiRequest&>(create_domain_request) = domain_request;
  create_domain_request.context = write_context;
  const auto domain = api::EngineCreateDomain(create_domain_request);
  Require(domain.ok && domain.primary_object.uuid.canonical == kDomainUuid,
          "domain create did not use server UUID");
  Require(HasEvidence(domain, "domain_event", "domain_create"),
          "domain persistence evidence missing");

  constexpr const char* kFunctionUuid = "019e078d-f11d-7000-8000-000000000105";
  api::EngineApiRequest function_request;
  function_request.target_schema.uuid.canonical = kSchemaUuid;
  function_request.target_schema.object_kind = "schema";
  function_request.target_object.uuid.canonical = kFunctionUuid;
  function_request.target_object.object_kind = "function";
  function_request.localized_names.push_back(Name("fspe011d_function"));
  function_request.option_envelopes.push_back("permission:manage_executable");
  api::EngineCreateFunctionRequest create_function_request;
  static_cast<api::EngineApiRequest&>(create_function_request) =
      function_request;
  auto function_context = write_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &function_context, {"CATALOG_MUTATE"});
  create_function_request.context = std::move(function_context);
  const auto function = api::EngineCreateFunction(create_function_request);
  for (const auto& diagnostic : function.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(function.ok && function.primary_object.uuid.canonical == kFunctionUuid,
          "function create did not use server UUID");
  Require(HasEvidence(function, "function", kFunctionUuid),
          "function persistence evidence missing");

  constexpr const char* kIdentityUuid = "019e078d-f11d-7000-8000-000000000106";
  api::EngineApiRequest identity_request;
  identity_request.target_object.uuid.canonical = kIdentityUuid;
  identity_request.target_object.object_kind = "security_identity";
  identity_request.localized_names.push_back(Name("fspe011d_user"));
  identity_request.option_envelopes.push_back("identity_kind:user");
  api::EngineCreateIdentityRequest create_identity_request;
  static_cast<api::EngineApiRequest&>(create_identity_request) =
      identity_request;
  auto identity_context = write_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &identity_context, {"SEC_IDENTITY_ADMIN"});
  create_identity_request.context = std::move(identity_context);
  const auto identity = api::EngineCreateIdentity(create_identity_request);
  for (const auto& diagnostic : identity.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(identity.ok && identity.primary_object.uuid.canonical == kIdentityUuid,
          "security identity create did not use server UUID");
  Require(HasEvidence(identity, "security_user", kIdentityUuid),
          "security identity persistence evidence missing");

  constexpr const char* kGrantUuid = "019e078d-f11d-7000-8000-000000000107";
  api::EngineApiRequest grant_request;
  grant_request.target_object.uuid.canonical = kGrantUuid;
  grant_request.target_object.object_kind = "grant";
  grant_request.related_objects.push_back({{kIdentityUuid}, "security_identity"});
  grant_request.related_objects.push_back({{kTableUuid}, "table"});
  grant_request.option_envelopes.push_back("right:SELECT");
  auto grant = Dispatch(database_path,
                        "security.grant_right",
                        "SBLR_SECURITY_GRANT_RIGHT",
                        GrantAdminContext(write_context),
                        grant_request,
                        true);
  Require(grant.api_result.primary_object.uuid.canonical == kGrantUuid, "grant create did not use server UUID");
  Require(HasEvidence(grant.api_result, "grant", kGrantUuid), "grant persistence evidence missing");

  constexpr const char* kConfigUuid = "019e078d-f11d-7000-8000-000000000108";
  api::EngineApiRequest config_request;
  config_request.target_object.uuid.canonical = kConfigUuid;
  config_request.target_object.object_kind = "config";
  config_request.localized_names.push_back(Name("fspe011d.metrics.enabled"));
  config_request.option_envelopes.push_back("value:true");
  api::EngineSetConfigRequest set_config_request;
  static_cast<api::EngineApiRequest&>(set_config_request) = config_request;
  set_config_request.context = write_context;
  const auto config = api::EngineSetConfig(set_config_request);
  Require(config.ok && config.primary_object.uuid.canonical == kConfigUuid,
          "config set did not use server UUID");
  Require(HasEvidence(config, "config", kConfigUuid),
          "config persistence evidence missing");

  constexpr const char* kParserPackageUuid = "019e078d-f11d-7000-8000-000000000109";
  api::EngineApiRequest parser_package_request;
  parser_package_request.target_object.uuid.canonical = kParserPackageUuid;
  parser_package_request.target_object.object_kind = "parser_package";
  parser_package_request.localized_names.push_back(Name("fspe011d_sbsql_parser_package"));
  parser_package_request.option_envelopes.push_back("package_kind:sbsql_parser_support");
  auto parser_package = Dispatch(database_path,
                                 "extensibility.register_parser_package",
                                 "SBLR_EXTENSIBILITY_REGISTER_PARSER_PACKAGE",
                                 write_context,
                                 parser_package_request,
                                 true);
  Require(parser_package.api_result.primary_object.uuid.canonical == kParserPackageUuid,
          "parser package register did not use server UUID");
  Require(HasEvidence(parser_package.api_result, "parser_package", kParserPackageUuid),
          "parser package persistence evidence missing");

  constexpr const char* kEventChannelUuid = "019e078d-f11d-7000-8000-00000000010a";
  constexpr const char* kSubscriptionUuid = "019e078d-f11d-7000-8000-00000000010b";
  api::EngineApiRequest channel_request;
  channel_request.target_object.uuid.canonical = kEventChannelUuid;
  channel_request.target_object.object_kind = "event_channel";
  channel_request.localized_names.push_back(Name("fspe011d_channel"));
  channel_request.option_envelopes.push_back("queue_policy_uuid:event.queue.durable_local.fspe011d");
  api::EngineCreateEventChannelRequest create_channel_request;
  static_cast<api::EngineApiRequest&>(create_channel_request) = channel_request;
  auto event_create_context = write_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &event_create_context, {"EVENT_CREATE"});
  create_channel_request.context = std::move(event_create_context);
  const auto channel = api::EngineCreateEventChannel(create_channel_request);
  Require(channel.ok && channel.primary_object.uuid.canonical == kEventChannelUuid,
          "event channel create did not use server UUID");
  Require(HasEvidence(channel, "event_channel", kEventChannelUuid),
          "event channel persistence evidence missing");

  api::EngineApiRequest listen_request;
  listen_request.target_object.uuid.canonical = kEventChannelUuid;
  listen_request.target_object.object_kind = "event_channel";
  listen_request.option_envelopes.push_back(std::string("subscription_uuid:") + kSubscriptionUuid);
  listen_request.option_envelopes.push_back("delivery_profile:durable_local");
  api::EngineListenNotificationRequest listen_notification_request;
  static_cast<api::EngineApiRequest&>(listen_notification_request) =
      listen_request;
  auto event_listen_context = write_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &event_listen_context, {"EVENT_SUBSCRIBE"});
  listen_notification_request.context = std::move(event_listen_context);
  const auto listen = api::EngineListenNotification(listen_notification_request);
  Require(listen.ok && listen.primary_object.uuid.canonical == kSubscriptionUuid,
          "event listen did not use subscription UUID");
  Require(HasEvidence(listen, "event_subscription", kSubscriptionUuid),
          "event subscription persistence evidence missing");

  api::EngineApiRequest notify_request;
  notify_request.target_object.uuid.canonical = kEventChannelUuid;
  notify_request.target_object.object_kind = "event_channel";
  notify_request.rows.push_back(Row("event", "persisted-event"));
  notify_request.option_envelopes.push_back("payload:persisted-event");
  api::EngineNotifyEventChannelRequest notify_channel_request;
  static_cast<api::EngineApiRequest&>(notify_channel_request) = notify_request;
  auto event_notify_context = write_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &event_notify_context, {"EVENT_PUBLISH"});
  notify_channel_request.context = std::move(event_notify_context);
  const auto notify = api::EngineNotifyEventChannel(notify_channel_request);
  Require(notify.ok && HasEvidence(notify, "event_publication"),
          "event publication persistence evidence missing");

  api::EngineApiRequest page_agent_request;
  page_agent_request.related_objects.push_back({{created_filespace_uuid}, "filespace"});
  page_agent_request.option_envelopes.push_back("page_family:data");
  page_agent_request.option_envelopes.push_back("page_type:relation");
  page_agent_request.option_envelopes.push_back("requested_pages:2");
  page_agent_request.option_envelopes.push_back("policy_authorized:true");
  page_agent_request.option_envelopes.push_back("evidence_sink_available:true");
  page_agent_request.option_envelopes.push_back("metrics_fresh:true");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_observed:true");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_trust_provenance:test_metric_registry");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_source_id:fspe011d:source");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:fspe011d:key");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:fspe011d:attestation");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_protected_material_present:false");
  page_agent_request.option_envelopes.push_back("agent_metric_snapshot_provenance_record:fspe011d:provenance");
  page_agent_request.option_envelopes.push_back(
      "agent_metric_snapshot_scope_uuid:019e078d-f11d-7000-8000-000000000001");
  page_agent_request.option_envelopes.push_back(
      "agent_metric_snapshot_digest:sha256:fspe011d:page_allocation_manager");
  page_agent_request.option_envelopes.push_back(
      "agent_metric_snapshot_value_digest:sha256:fspe011d:page_allocation_manager:value");
  page_agent_request.option_envelopes.push_back(
      "agent_metric_snapshot_schema_digest:sha256:fspe011d:page_allocation_manager:schema");
  page_agent_request.option_envelopes.push_back(
      "agent_metric_snapshot_id:fspe011d:page_allocation_manager");
  page_agent_request.option_envelopes.push_back(
      "agent_metric_snapshot_evidence_uuid:019e078d-f11d-7000-8000-00000000a201");
  page_agent_request.option_envelopes.push_back("safety_fence_result:passed");
  page_agent_request.option_envelopes.push_back("wall_now_us:1");
  page_agent_request.option_envelopes.push_back("monotonic_now_us:1");
  auto page_agent_context = write_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &page_agent_context, {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  auto page_agent = Dispatch(database_path,
                             "agents.request_page_preallocation",
                             "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION",
                             std::move(page_agent_context),
                             page_agent_request,
                             true);
  Require(HasEvidence(page_agent.api_result, "agent_hook", "agents.request_page_preallocation"),
          "page preallocation agent evidence missing");

  auto commit_write = Dispatch(database_path,
                               "transaction.commit",
                               "SBLR_TXN_COMMIT",
                               write_context,
                               {},
                               true);
  Require(commit_write.api_result.ok, "write transaction commit failed");
  Require(HasEvidence(commit_write.api_result, "transaction_state", "committed"), "commit evidence missing");

  auto shutdown = Dispatch(database_path,
                           "lifecycle.shutdown_database",
                           "SBLR_LIFECYCLE_SHUTDOWN_DATABASE",
                           BaseContext(database_path));
  Require(shutdown.api_result.ok, "SBLR lifecycle clean shutdown failed");
  Require(FieldValue(shutdown.api_result, "clean_shutdown") == "true", "clean shutdown row missing");

  auto reopened = db::OpenDatabaseFile({database_path.string(), false, false, false});
  VerifyOpenedState(reopened);
  const auto clean_after_direct_open = db::MarkDatabaseCleanShutdown(database_path.string());
  Require(clean_after_direct_open.ok(), "clean shutdown after direct reopen failed");

  auto reopen_dispatch = Dispatch(database_path,
                                  "lifecycle.open_database",
                                  "SBLR_LIFECYCLE_OPEN_DATABASE",
                                  BaseContext(database_path));
  Require(reopen_dispatch.api_result.ok, "SBLR lifecycle reopen failed");
  Require(FieldValue(reopen_dispatch.api_result, "startup_recovery_classification") == "clean_checkpoint_path",
          "SBLR reopen did not report clean checkpoint path");

  auto begin_read = Dispatch(database_path,
                             "transaction.begin",
                             "SBLR_TXN_BEGIN",
                             BaseContext(database_path));
  Require(begin_read.api_result.local_transaction_id != 0, "read transaction did not return local transaction id");
  auto read_context = BaseContext(database_path);
  read_context.local_transaction_id = begin_read.api_result.local_transaction_id;
  read_context.transaction_uuid = begin_read.api_result.transaction_uuid;
  read_context.snapshot_visible_through_local_transaction_id = begin_write.api_result.local_transaction_id;

  api::EngineApiRequest lookup_request;
  lookup_request.target_object.uuid.canonical = kTableUuid;
  lookup_request.target_object.object_kind = "table";
  auto lookup = Dispatch(database_path,
                         "catalog.lookup_object",
                         "SBLR_CATALOG_LOOKUP_OBJECT",
                         read_context,
                         lookup_request,
                         true);
  Require(lookup.api_result.primary_object.uuid.canonical == kTableUuid, "catalog lookup did not find table after restart");
  RequireLookup(database_path, read_context, kSchemaUuid, "schema");
  RequireLookup(database_path, read_context, kIndexUuid, "index");
  RequireLookup(database_path, read_context, kDomainUuid, "domain");
  RequireLookup(database_path, read_context, kFunctionUuid, "function");
  RequireLookup(database_path, read_context, kIdentityUuid, "security_user");
  RequireLookup(database_path, read_context, kGrantUuid, "grant");
  RequireLookup(database_path, read_context, kConfigUuid, "config");
  RequireLookup(database_path, read_context, kParserPackageUuid, "parser_package");
  RequireDescriptorContains(database_path, read_context, kTableUuid, "columns=");
  RequireDescriptorContains(database_path, read_context, kDomainUuid, "visibility_policy=66737065303131645f76697369626c65");

  api::EngineApiRequest select_request;
  select_request.target_object.uuid.canonical = kTableUuid;
  select_request.target_object.object_kind = "table";
  select_request.predicate.predicate_kind = "column_equals";
  select_request.predicate.canonical_predicate_envelope = "id";
  select_request.predicate.bound_values.push_back(TextValue("2"));
  select_request.projection.canonical_projection_envelopes.push_back("id");
  select_request.projection.canonical_projection_envelopes.push_back("note");
  api::EngineSelectRowsRequest select_rows_request;
  static_cast<api::EngineApiRequest&>(select_rows_request) = select_request;
  select_rows_request.context = read_context;
  const auto selected = api::EngineSelectRows(select_rows_request);
  Require(selected.ok && selected.result_shape.rows.size() == 1,
          "select did not see persisted row after restart");
  Require(FieldValue(selected, "id") == "2", "persisted row id mismatch after restart");
  Require(FieldValue(selected, "note") == "persisted-through-index",
          "persisted row payload mismatch after restart");
  Require(HasEvidence(selected, "index_lookup"),
          "post-restart select did not use persisted index");

  api::EnginePollEventDeliveryRequest poll_request;
  auto event_poll_context = read_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &event_poll_context, {"EVENT_DELIVERY_READ"});
  poll_request.context = std::move(event_poll_context);
  const auto polled = api::EnginePollEventDelivery(poll_request);
  Require(polled.ok && HasFieldValue(polled, "payload", "persisted-event"),
          "durable event publication did not survive restart");

  api::EngineInspectConfigRequest inspect_config_request;
  inspect_config_request.context = read_context;
  const auto inspected_config = api::EngineInspectConfig(inspect_config_request);
  Require(inspected_config.ok &&
              HasFieldValue(inspected_config, "config_uuid", kConfigUuid),
          "metrics/config record did not survive restart");

  api::EngineApiRequest agent_show_request;
  agent_show_request.option_envelopes.push_back("agent_type:job_control_manager");
  auto agent_show_context = read_context;
  scratchbird::tests::release::GrantMaterializedRights(
      &agent_show_context, {"OBS_AGENT_STATE_READ"});
  auto job_agent = Dispatch(database_path,
                            "agents.show",
                            "SBLR_AGENTS_SHOW",
                            std::move(agent_show_context),
                            agent_show_request,
                            false);
  Require(HasFieldValue(job_agent.api_result, "agent_type", "job_control_manager"),
          "job control manager registry surface unavailable after restart");

  auto commit_read = Dispatch(database_path,
                              "transaction.commit",
                              "SBLR_TXN_COMMIT",
                              read_context,
                              {},
                              true);
  Require(commit_read.api_result.ok, "read transaction commit failed");

  std::filesystem::remove_all(work);
  std::cout << "sbsql_persistence_restart_conformance=passed database_uuid="
            << created_database_uuid << " rows=2 table_uuid=" << kTableUuid << '\n';
  return EXIT_SUCCESS;
}
