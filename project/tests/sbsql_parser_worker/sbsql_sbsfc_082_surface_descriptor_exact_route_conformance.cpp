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
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000082001";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view sql;
  std::string_view route_kind;
  std::string_view descriptor_role;
  std::string_view descriptor_ref;
};

const CaseRow kCases[] = {
    {"SBSQL-5252559BCC6F", "monitor_input", "grammar_production", "MONITOR INPUT lag;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-54185A6B5CF6", "expiry_clause", "grammar_production", "EXPIRY CLAUSE ttl;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-584EC38FC9E5", "bool_clause", "grammar_production", "BOOL CLAUSE true;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-58BDE3CE0180", "sql_text", "grammar_production", "SQL TEXT normalized;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-58E1427DEE2C", "rebuild_options", "grammar_production", "REBUILD OPTIONS online;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-5E6BBEE42304", "materialization_hint", "grammar_production", "MATERIALIZATION HINT materialized;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-5E6DC360F377", "resolve_name_public", "canonical_surface", "RESOLVE NAME PUBLIC target;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-5EF441751C18", "render_contract_name", "grammar_production", "RENDER CONTRACT NAME canonical;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-61D0B486BF6C", "charset_clause", "grammar_production", "CHARSET CLAUSE utf8;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-63FCCA3AB5E6", "diag_assignment", "grammar_production", "DIAG ASSIGNMENT message;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-64A4C690F1DB", "label_decl", "grammar_production", "LABEL DECL loop_a;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-65AD5E103469", "in_target", "grammar_production", "IN TARGET rowset;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-6916A845F6A3", "range_options", "grammar_production", "RANGE OPTIONS bounded;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-694CD8192572", "dict_lifetime", "grammar_production", "DICT LIFETIME session;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-69A297A83452", "range_op", "grammar_production", "RANGE OP overlaps;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-6B7B90870888", "heterogeneous_form", "grammar_production", "HETEROGENEOUS FORM enabled;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-6C4B82EB48F2", "ttl_set_assign", "grammar_production", "TTL SET ASSIGN ttl;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-6DBD31C25418", "partition_method", "grammar_production", "PARTITION METHOD hash;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-710AB5F3660C", "hint_directive", "grammar_production", "HINT DIRECTIVE index;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-7519AB513F98", "flashback_stmt", "grammar_production", "FLASHBACK STMT timestamp;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-76A71BDF7D24", "ddl_multimodel_stmt", "grammar_production", "DDL MULTIMODEL STMT document;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-7711B88AF159", "qualify_clause", "grammar_production", "QUALIFY CLAUSE rank;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-7B45978FFB2C", "support_stmt", "grammar_production", "SUPPORT STMT inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-7C871FA5CAF6", "digit", "grammar_production", "DIGIT 7;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-7E41DDA3564F", "vacuum_option", "grammar_production", "VACUUM OPTION analyze;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-7EB6F5C24255", "encryption_options", "grammar_production", "ENCRYPTION OPTIONS local;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-7EF725F464B5", "period_lookup", "grammar_production", "PERIOD LOOKUP system_time;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-7F0DD56ED1AA", "dialect_name", "grammar_production", "DIALECT NAME sbsql;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-7F596EB8572C", "coprocessor_hint", "grammar_production", "COPROCESSOR HINT local;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-807095A5AAA6", "ca_name", "grammar_production", "CA NAME root_ca;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-82517404B2BD", "uuid_reference", "grammar_production", "UUID REFERENCE object;", "expression_descriptor_validation", "expression_descriptor", "sys.query.expression_descriptor"},
    {"SBSQL-835D3440C77A", "compression_clause", "grammar_production", "COMPRESSION CLAUSE lz4;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-83C0F94F52A8", "set_quantifier", "grammar_production", "SET QUANTIFIER distinct;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-8528ADDE68DE", "edition_scope", "grammar_production", "EDITION SCOPE current;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-87333795AE09", "sweep_priority", "grammar_production", "SWEEP PRIORITY low;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-895D13E51241", "os_analyzer_def", "grammar_production", "OS ANALYZER DEF standard;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-89E79556C408", "ttl_action", "grammar_production", "TTL ACTION expire;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-8B3D1277AB5C", "compact_options", "grammar_production", "COMPACT OPTIONS online;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-8D6851EE865D", "move_options", "grammar_production", "MOVE OPTIONS online;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-8E26D8C232C3", "import_options", "grammar_production", "IMPORT OPTIONS batch;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-8F1237F65DE3", "filter_clause", "grammar_production", "FILTER CLAUSE active;", "query_descriptor_validation", "query_plan_descriptor", "sys.query.plan_descriptor"},
    {"SBSQL-9226724284C3", "generation_clause", "grammar_production", "GENERATION CLAUSE identity;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-929481915091", "analyze_option", "grammar_production", "ANALYZE OPTION sample;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-93C7D0583A0B", "buffer_pool_stmt", "grammar_production", "BUFFER POOL STMT inspect;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-974EF3FD88C6", "cleanup_action", "grammar_production", "CLEANUP ACTION sweep;", "storage_descriptor_validation", "storage_management_descriptor", "sys.storage.management_profile"},
    {"SBSQL-97A40B10134D", "event_body", "grammar_production", "EVENT BODY notify;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-97FE78656EA1", "branch_op_stmt", "grammar_production", "BRANCH OP STMT inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-9A9A1995FC1E", "admission_stmt", "grammar_production", "ADMISSION STMT inspect;", "management_descriptor_validation", "management_descriptor", "sys.management.runtime"},
    {"SBSQL-9C2FEDE03D1D", "opclass_item", "grammar_production", "OPCLASS ITEM btree;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
    {"SBSQL-9C574516EB61", "enum_value_assign", "grammar_production", "ENUM VALUE ASSIGN red;", "catalog_descriptor_validation", "catalog_descriptor", "sys.catalog.object_descriptor"},
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
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
  session.session_uuid = "019f0000-0000-7000-8000-000000082101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000082102";
  session.database_uuid = "019f0000-0000-7000-8000-000000082103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 82;
  session.security_policy_epoch = 83;
  session.descriptor_epoch = 84;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_082_surface_descriptor";
  config.parser_uuid = "019f0000-0000-7000-8000-000000082104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-082-surface-descriptor";
  config.build_id = "sbsql-sbsfc-082-surface-descriptor";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const CaseRow& row) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(row.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-082 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-082 generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "SBSFC-082 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-082 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-082 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-082 generated registry canonical family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-082 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-082 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          "SBSFC-082 AST row surface id mismatch");
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          "SBSFC-082 AST canonical name mismatch");
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          "SBSFC-082 AST registry family mismatch");
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-082 AST canonical operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-082 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-082 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.values.v3",
          "SBSFC-082 route operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.values.v3",
          "SBSFC-082 route operation key mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "SBSFC-082 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "query.plan_operation",
          "SBSFC-082 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "SBSFC-082 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == "EnginePlanOperation",
          "SBSFC-082 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-082 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-082 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          "SBSFC-082 cluster provider exclusion authority missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, row.descriptor_ref),
          "SBSFC-082 descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-082 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.route_kind),
          "SBSFC-082 payload missing route kind");
  Require(Contains(artifacts.envelope.payload, row.descriptor_role),
          "SBSFC-082 payload missing descriptor role");
  Require(Contains(artifacts.envelope.payload, row.descriptor_ref),
          "SBSFC-082 payload missing descriptor ref");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-082 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-082 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-082 payload used replay, refusal, or cluster-provider error evidence");
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false") &&
              Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          "SBSFC-082 payload did not prove no cluster/private dispatch");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-082 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-082 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-082 server admission did not require public ABI dispatch");
  Require(admission.operation_id == "query.plan_operation",
          "SBSFC-082 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.optimizer.plan.v3",
          "SBSFC-082 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation("query.plan_operation");
  Require(opcode != nullptr, "SBSFC-082 opcode registry row missing");
  Require(opcode->opcode == "SBLR_QUERY_PLAN_OPERATION", "SBSFC-082 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-082 opcode claimed cluster authority");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-082-surface-descriptor";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000082201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000082202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000082203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000082204";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000082205";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000082206";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.current_sqlstate = "00000";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000082207";
  context.trace_tags.push_back("sbsfc082.surface_descriptor");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.sbsfc082.surface_descriptor");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "sbsfc082_surface_descriptor"});
  envelope.operands.push_back({"text", "sbsfc082_surface_id", std::string(row.surface_id)});
  envelope.operands.push_back({"text", "sbsfc082_runtime_evidence_kind", "sbsfc082_surface_descriptor_route"});
  envelope.operands.push_back({"text", "sbsfc082_runtime_evidence_id", std::string(row.canonical_name)});
  envelope.operands.push_back({"text", "sbsfc082_descriptor_role", std::string(row.descriptor_role)});
  envelope.operands.push_back({"text", "sbsfc082_descriptor_ref", std::string(row.descriptor_ref)});
  envelope.operands.push_back({"text", "query_operation", "descriptor_validation"});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "sbsfc082_surface_descriptor";
  request.option_envelopes.push_back(std::string("sbsfc082_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back("sbsfc082_runtime_evidence_kind:sbsfc082_surface_descriptor_route");
  request.option_envelopes.push_back(std::string("sbsfc082_runtime_evidence_id:") + std::string(row.canonical_name));
  request.option_envelopes.push_back(std::string("sbsfc082_descriptor_role:") + std::string(row.descriptor_role));
  request.option_envelopes.push_back(std::string("sbsfc082_descriptor_ref:") + std::string(row.descriptor_ref));
  request.option_envelopes.push_back("query_operation:descriptor_validation");
  return request;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

void RequireEngineDispatch(const api::EngineRequestContext& context, const CaseRow& row) {
  const auto result = sblr::DispatchSblrOperation({context, EngineEnvelope(row), ApiRequestFor(row)});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-082 engine envelope rejected");
  Require(result.accepted, "SBSFC-082 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-082 engine did not dispatch to API");
  Require(result.api_result.operation_id == "query.plan_operation",
          "SBSFC-082 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-082 runtime API did not complete");
  Require(HasEvidence(result.api_result, "sbsfc082_surface_descriptor_route", row.canonical_name),
          "SBSFC-082 runtime evidence missing");
  Require(HasEvidence(result.api_result, "sbsfc082_surface", row.surface_id),
          "SBSFC-082 runtime did not carry row surface evidence");
  Require(HasEvidence(result.api_result, "sbsfc082_descriptor_role", row.descriptor_role),
          "SBSFC-082 runtime did not carry descriptor-role evidence");
  Require(HasEvidence(result.api_result, "sbsfc082_descriptor_ref", row.descriptor_ref),
          "SBSFC-082 runtime did not carry descriptor-ref evidence");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-082 runtime allowed parser SQL execution");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-082 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "private_cluster_execution", "false"),
          "SBSFC-082 runtime claimed private cluster execution");
  Require(HasEvidence(result.api_result, "wal_recovery_authority", "false"),
          "SBSFC-082 runtime carried WAL/recovery authority");
}

}  // namespace

int main() {
  static_assert(sizeof(kCases) / sizeof(kCases[0]) == 50);
  for (const auto& row : kCases) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto context = EngineContext();
  for (const auto& row : kCases) {
    RequireEngineDispatch(context, row);
  }

  std::cout << "sbsql_sbsfc_082_surface_descriptor_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
