// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000075001";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view object_kind;
  bool needs_uuid;
};

const CaseRow kCases[] = {
    {"SBSQL-02482A768886", "create_materialized_view_stmt", "CREATE MATERIALIZED VIEW mv AS SELECT 1;", "catalog.mutation.create_materialized_view", "SBLR_CATALOG_MUTATION_CREATE_MATERIALIZED_VIEW", "materialized_view", false},
    {"SBSQL-0D79A271D250", "create_cast_stmt", "CREATE CAST cast_one;", "catalog.mutation.create_cast", "SBLR_CATALOG_MUTATION_CREATE_CAST", "cast", false},
    {"SBSQL-0E2166CCB037", "create_server_stmt", "CREATE SERVER srv;", "catalog.mutation.create_server", "SBLR_CATALOG_MUTATION_CREATE_SERVER", "server", false},
    {"SBSQL-1C0806816BB8", "show_storage_buffer_io_index", "SHOW STORAGE BUFFER IO INDEX;", "catalog.mutation.show_storage_buffer_io_index", "SBLR_CATALOG_MUTATION_SHOW_STORAGE_BUFFER_IO_INDEX", "storage_buffer_io_index", false},
    {"SBSQL-1EAD79587BB9", "alter_ts_action", "ALTER TIME SERIES ts SET RETENTION 60;", "catalog.mutation.alter_time_series", "SBLR_CATALOG_MUTATION_ALTER_TIME_SERIES", "time_series", true},
    {"SBSQL-1FEC46C4DA02", "create_key_value_store", "CREATE KEY VALUE STORE kvs;", "catalog.mutation.create_key_value_store", "SBLR_CATALOG_MUTATION_CREATE_KEY_VALUE_STORE", "key_value_store", false},
    {"SBSQL-2AB5BB905FA2", "cypher_create_clause", "CYPHER CREATE node;", "catalog.mutation.cypher_create", "SBLR_CATALOG_MUTATION_CYPHER_CREATE", "graph_cypher_pattern", false},
    {"SBSQL-332194BDCF54", "graph_create_node_stmt", "CREATE GRAPH NODE person;", "catalog.mutation.graph_create_node", "SBLR_CATALOG_MUTATION_GRAPH_CREATE_NODE", "graph_node", false},
    {"SBSQL-35F30501F9AB", "create_bucket_stmt", "CREATE BUCKET bucket_one;", "catalog.mutation.create_bucket", "SBLR_CATALOG_MUTATION_CREATE_BUCKET", "bucket", false},
    {"SBSQL-433D89379149", "alter_filespace_stmt", "ALTER FILESPACE fs SET QUOTA 1;", "catalog.mutation.alter_filespace", "SBLR_CATALOG_MUTATION_ALTER_FILESPACE", "filespace", true},
    {"SBSQL-43B8DD8E30F4", "create_operator_stmt", "CREATE OPERATOR op_add;", "catalog.mutation.create_operator", "SBLR_CATALOG_MUTATION_CREATE_OPERATOR", "operator", false},
    {"SBSQL-4AEBD06003D0", "create_event_trigger_stmt", "CREATE EVENT TRIGGER trig;", "catalog.mutation.create_event_trigger", "SBLR_CATALOG_MUTATION_CREATE_EVENT_TRIGGER", "event_trigger", false},
    {"SBSQL-51DE71CC37A3", "create_package_body_stmt", "CREATE PACKAGE BODY pkg;", "catalog.mutation.create_package_body", "SBLR_CATALOG_MUTATION_CREATE_PACKAGE_BODY", "package_body", false},
    {"SBSQL-54DA8D47DADA", "create_aggregate_stmt", "CREATE AGGREGATE agg_sum;", "catalog.mutation.create_aggregate", "SBLR_CATALOG_MUTATION_CREATE_AGGREGATE", "aggregate", false},
    {"SBSQL-57BDE9C872EF", "alter_routine_stmt", "ALTER ROUTINE r SET OWNER owner;", "catalog.mutation.alter_routine", "SBLR_CATALOG_MUTATION_ALTER_ROUTINE", "routine", true},
    {"SBSQL-5878BC8439F1", "create_graph", "CREATE GRAPH social;", "catalog.mutation.create_graph", "SBLR_CATALOG_MUTATION_CREATE_GRAPH", "graph", false},
    {"SBSQL-68FEEBE11534", "create_dictionary_stmt", "CREATE DICTIONARY dict;", "catalog.mutation.create_dictionary", "SBLR_CATALOG_MUTATION_CREATE_DICTIONARY", "dictionary", false},
    {"SBSQL-730E005B2620", "create_package_stmt", "CREATE PACKAGE pkg;", "catalog.mutation.create_package", "SBLR_CATALOG_MUTATION_CREATE_PACKAGE", "package", false},
    {"SBSQL-78DE35FE41D9", "alter_udr_action", "ALTER UDR u SET TRUSTED;", "catalog.mutation.alter_udr", "SBLR_CATALOG_MUTATION_ALTER_UDR", "udr", true},
    {"SBSQL-792BE5FDF5F8", "graph_create_edge_stmt", "CREATE GRAPH EDGE knows;", "catalog.mutation.graph_create_edge", "SBLR_CATALOG_MUTATION_GRAPH_CREATE_EDGE", "graph_edge", false},
    {"SBSQL-799E1967776C", "create_filespace_stmt", "CREATE FILESPACE fs;", "catalog.mutation.create_filespace", "SBLR_CATALOG_MUTATION_CREATE_FILESPACE", "filespace", false},
    {"SBSQL-82DBA8E8E31B", "create_filespace_agent_stmt", "CREATE FILESPACE AGENT fsa;", "catalog.mutation.create_filespace_agent", "SBLR_CATALOG_MUTATION_CREATE_FILESPACE_AGENT", "filespace_agent", false},
    {"SBSQL-8CAA57A856BE", "create_quota_stmt", "CREATE QUOTA q;", "catalog.mutation.create_quota", "SBLR_CATALOG_MUTATION_CREATE_QUOTA", "quota", false},
    {"SBSQL-8D20EF30CB0D", "alter_kv_action", "ALTER KEY VALUE STORE kvs SET TTL 60;", "catalog.mutation.alter_key_value_store", "SBLR_CATALOG_MUTATION_ALTER_KEY_VALUE_STORE", "key_value_store", true},
    {"SBSQL-9518445D0CD9", "alter_subject_action", "ALTER SUBJECT s SET POLICY p;", "catalog.mutation.alter_subject", "SBLR_CATALOG_MUTATION_ALTER_SUBJECT", "security_subject", true},
    {"SBSQL-9B606FD6EF79", "graph_index_stmt", "CREATE GRAPH INDEX gidx;", "catalog.mutation.graph_create_index", "SBLR_CATALOG_MUTATION_GRAPH_CREATE_INDEX", "graph_index", false},
    {"SBSQL-9DAFD9A7D944", "create_binding_stmt", "CREATE BINDING bind_one;", "catalog.mutation.create_binding", "SBLR_CATALOG_MUTATION_CREATE_BINDING", "binding", false},
    {"SBSQL-AFAFF306E5CC", "alter_routine_action", "ALTER ROUTINE r SET OWNER owner;", "catalog.mutation.alter_routine", "SBLR_CATALOG_MUTATION_ALTER_ROUTINE", "routine", true},
    {"SBSQL-BC5F731E5171", "create_monitor_stmt", "CREATE MONITOR mon;", "catalog.mutation.create_monitor", "SBLR_CATALOG_MUTATION_CREATE_MONITOR", "monitor", false},
    {"SBSQL-C128B0E7BACF", "refresh_materialized_view_stmt", "REFRESH MATERIALIZED VIEW mv;", "catalog.mutation.refresh_materialized_view", "SBLR_CATALOG_MUTATION_REFRESH_MATERIALIZED_VIEW", "materialized_view", true},
    {"SBSQL-C16916312DA6", "create_filespace_stmt_full", "CREATE FILESPACE fs LOCATION 'tmp';", "catalog.mutation.create_filespace", "SBLR_CATALOG_MUTATION_CREATE_FILESPACE", "filespace", false},
    {"SBSQL-C1D72E3D9A5B", "create_transform_stmt", "CREATE TRANSFORM tr;", "catalog.mutation.create_transform", "SBLR_CATALOG_MUTATION_CREATE_TRANSFORM", "transform", false},
    {"SBSQL-C22117F1754C", "create_secret_stmt", "CREATE SECRET sec;", "catalog.mutation.create_secret", "SBLR_CATALOG_MUTATION_CREATE_SECRET", "secret", false},
    {"SBSQL-C75FEC2CAA69", "alter_reference_action", "ALTER REFERENCE d SET PROFILE p;", "catalog.mutation.alter_reference", "SBLR_CATALOG_MUTATION_ALTER_REFERENCE", "reference", true},
    {"SBSQL-CAB8A126ED9E", "create_pipeline_stmt", "CREATE PIPELINE pipe;", "catalog.mutation.create_pipeline", "SBLR_CATALOG_MUTATION_CREATE_PIPELINE", "pipeline", false},
    {"SBSQL-CDF9CAB99BFE", "create_collation_stmt", "CREATE COLLATION coll;", "catalog.mutation.create_collation", "SBLR_CATALOG_MUTATION_CREATE_COLLATION", "collation", false},
    {"SBSQL-D13498FA0EF4", "create_type_stmt", "CREATE TYPE typ;", "catalog.mutation.create_type", "SBLR_CATALOG_MUTATION_CREATE_TYPE", "type", false},
    {"SBSQL-DDC745405168", "alter_view_action", "ALTER VIEW v SET CHECK OPTION;", "catalog.mutation.alter_view", "SBLR_CATALOG_MUTATION_ALTER_VIEW", "view", true},
    {"SBSQL-E56EBC2407A0", "create_udr_stmt", "CREATE UDR udr_one;", "catalog.mutation.create_udr", "SBLR_CATALOG_MUTATION_CREATE_UDR", "udr", false},
    {"SBSQL-E589270E0A27", "create_tenant_stmt", "CREATE TENANT tenant_one;", "catalog.mutation.create_tenant", "SBLR_CATALOG_MUTATION_CREATE_TENANT", "tenant", false},
    {"SBSQL-EFE478DCAABE", "create_time_series", "CREATE TIME SERIES ts;", "catalog.mutation.create_time_series", "SBLR_CATALOG_MUTATION_CREATE_TIME_SERIES", "time_series", false},
    {"SBSQL-FA4F3D029E2C", "create_document_collection", "CREATE DOCUMENT COLLECTION docs;", "catalog.mutation.create_document_collection", "SBLR_CATALOG_MUTATION_CREATE_DOCUMENT_COLLECTION", "document_collection", false},
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_sbsfc_075_catalog_descriptor_mutation_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_sbsfc_075_catalog_descriptor_mutation_conformance");
  Require(configured.ok(), "SBSFC-075 memory fixture configuration failed");
  Require(configured.fixture_mode, "SBSFC-075 memory fixture mode was not active");
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000075101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000075102";
  session.database_uuid = "019f0000-0000-7000-8000-000000075103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 75;
  session.security_policy_epoch = 76;
  session.descriptor_epoch = 77;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_075_catalog_descriptor_mutation";
  config.parser_uuid = "019f0000-0000-7000-8000-000000075104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-075-catalog-descriptor-mutation";
  config.build_id = "sbsql-sbsfc-075-catalog-descriptor-mutation";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

std::string TargetUuidFor(std::size_t index) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer),
                "019f0000-0000-7000-8000-%012zu", static_cast<std::size_t>(750000 + index));
  return buffer;
}

PipelineArtifacts RunPipeline(const CaseRow& row, std::size_t index) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(row.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  std::vector<std::string> resolved;
  if (row.needs_uuid) resolved.push_back(TargetUuidFor(index));
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, resolved);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-075 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-075 generated registry canonical name drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-075 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-075 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == kFamily,
          "SBSFC-075 generated registry SBLR family drifted");
}

void RequireExactLowering(const CaseRow& row,
                          const PipelineArtifacts& artifacts,
                          std::size_t index) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) {
    std::cerr << "SBSFC-075 rejected surface=" << row.surface_id
              << " canonical=" << row.canonical_name
              << " sql=" << row.sql
              << " operation_id=" << artifacts.envelope.operation_id
              << " opcode=" << artifacts.envelope.sblr_opcode
              << " payload=" << artifacts.envelope.payload << '\n';
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-075 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-075 AST failed");
  Require(artifacts.bound.bound, "SBSFC-075 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-075 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "SBSFC-075 operation family mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "SBSFC-075 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "SBSFC-075 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          "SBSFC-075 SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.catalog_descriptor_mutation_api_required"),
          "SBSFC-075 catalog descriptor mutation API authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.catalog_uuid_assignment_or_resolution_required"),
          "SBSFC-075 UUID authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.name_registry_required"),
          "SBSFC-075 name registry authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "SBSFC-075 MGA catalog authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-075 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-075 parser no-SQL-execution authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-075 lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "SBSFC-075 lowering allowed file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"catalog_descriptor_mutation\""),
          "SBSFC-075 payload missing descriptor mutation envelope");
  Require(Contains(artifacts.envelope.payload, row.operation_id),
          "SBSFC-075 payload missing operation id");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-075 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload,
                   "\"descriptor_ref\":\"sys.catalog."),
          "SBSFC-075 payload missing descriptor ref");
  Require(Contains(artifacts.envelope.payload,
                   "\"name_text_authority\":\"metadata_only_engine_name_registry\""),
          "SBSFC-075 payload missing metadata-only name authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-075 payload exposed SQL text");
  if (row.needs_uuid) {
    Require(Contains(artifacts.envelope.payload, TargetUuidFor(index)),
            "SBSFC-075 payload missing UUID-bound target");
  }
  if (!row.needs_uuid && std::string_view(row.sql).rfind("CREATE ", 0) == 0) {
    Require(Contains(artifacts.envelope.payload, "\"name\":\""),
            "SBSFC-075 create payload missing requested object name");
  }
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-075 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal"),
          "SBSFC-075 payload carried WAL authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-075 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-075");
  Require(admission.operation_id == row.operation_id,
          "server admission SBSFC-075 operation id mismatch");

  const auto* opcode_entry = sblr::LookupSblrOperation(std::string(row.operation_id));
  Require(opcode_entry != nullptr, "SBSFC-075 opcode registry row missing");
  Require(opcode_entry->opcode == row.opcode, "SBSFC-075 opcode registry drifted");
  Require(opcode_entry->requires_security_context,
          "SBSFC-075 opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "SBSFC-075 opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_075_catalog_descriptor_mutation_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810750000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810750001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810750002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "SBSFC-075 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-075-catalog-descriptor-mutation";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000075201";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000075202";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc075.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-075 transaction begin envelope rejected");
  Require(result.accepted, "SBSFC-075 transaction begin not accepted");
  Require(result.api_result.ok, "SBSFC-075 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "trace.sbsfc075.catalog_descriptor_mutation");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

api::EngineApiRequest EngineMutationRequest(const CaseRow& row, std::size_t index) {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = TargetUuidFor(index + 200);
  request.target_object.object_kind = std::string(row.object_kind);
  request.localized_names.push_back({"en", "primary", "", "catalog_descriptor_target", true});
  request.option_envelopes.push_back(std::string("catalog_authority:sys.catalog.") + std::string(row.canonical_name));
  request.option_envelopes.push_back(std::string("descriptor_ref:sys.catalog.") + std::string(row.object_kind));
  request.option_envelopes.push_back("mga_catalog_commit_required:true");
  request.option_envelopes.push_back("parser_executes_sql:false");
  return request;
}

void RequireEngineDispatch(const std::filesystem::path& path,
                           const std::string& database_uuid) {
  auto context = BeginEngineTransaction(path, database_uuid);
  for (std::size_t index = 0; index < std::size(kCases); ++index) {
    const auto& row = kCases[index];
    const sblr::SblrDispatchRequest request{
        context,
        EngineEnvelope(row),
        EngineMutationRequest(row, index)};
    const auto result = sblr::DispatchSblrOperation(request);
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Require(result.envelope_validated, "SBSFC-075 engine envelope rejected");
    Require(result.accepted, "SBSFC-075 engine dispatch did not accept route");
    Require(result.dispatched_to_api, "SBSFC-075 engine did not dispatch to API");
    Require(result.api_result.ok, "EngineCatalogDescriptorMutation failed");
    Require(result.api_result.operation_id == row.operation_id,
            "EngineCatalogDescriptorMutation operation id drifted");
    Require(result.api_result.primary_object.object_kind == row.object_kind,
            "EngineCatalogDescriptorMutation object kind drifted");
    Require(HasEvidence(result.api_result, "api_behavior_event", row.operation_id),
            "EngineCatalogDescriptorMutation missing behavior event evidence");
    Require(HasEvidence(result.api_result, "catalog_descriptor_mutation", row.operation_id),
            "EngineCatalogDescriptorMutation missing descriptor mutation evidence");
    Require(HasEvidence(result.api_result, "name_registry",
                        result.api_result.primary_object.uuid.canonical),
            "EngineCatalogDescriptorMutation missing name registry evidence");
    Require(HasEvidence(result.api_result, "mga_catalog_commit",
                        std::to_string(context.local_transaction_id)),
            "EngineCatalogDescriptorMutation missing MGA commit evidence");
  }
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  for (std::size_t index = 0; index < std::size(kCases); ++index) {
    const auto& row = kCases[index];
    RequireRegistryEvidence(row);
    const auto artifacts = RunPipeline(row, index);
    RequireExactLowering(row, artifacts, index);
  }
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  RequireEngineDispatch(path, database_uuid);
  RemoveDatabaseArtifacts(path);
  std::cout << "sbsql_sbsfc_075_catalog_descriptor_mutation_conformance=passed\n";
  return EXIT_SUCCESS;
}
