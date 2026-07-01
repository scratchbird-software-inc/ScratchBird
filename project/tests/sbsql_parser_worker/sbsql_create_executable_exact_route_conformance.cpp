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
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000e300ff";
constexpr std::string_view kCursorProcedureSql =
    "CREATE PROCEDURE replay_cursor_procedure(route_cursor cursor);";

struct Case {
  std::string_view sql;
  std::string_view surface_id;
  std::string_view surface_name;
  std::string_view fixture_id;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view object_kind;
  std::string_view catalog_authority;
  std::string_view object_uuid;
  std::string_view object_name;
  std::string_view signature_surface_id;
  std::string_view signature_surface_name;
  std::string_view signature_fixture_id;
};

const std::vector<Case>& Cases() {
  static const std::vector<Case> cases = {
      {"CREATE FUNCTION replay_function;", "SBSQL-4A5F97F6CC4E",
       "create_function_stmt", "SBSQL-SURFACE-F4AD1748A90B",
       "ddl.create_function", "SBLR_DDL_CREATE_FUNCTION", "function",
       "sys.catalog.function", "019f0000-0000-7000-8000-000000e30001",
       "replay_function", "SBSQL-52EF59CC2556", "function_signature",
       "SBSQL-SURFACE-2B16A6B8917F"},
      {"CREATE PROCEDURE replay_procedure;", "SBSQL-13F5A8364A50",
       "create_procedure_stmt", "SBSQL-SURFACE-515475BB02FD",
       "ddl.create_procedure", "SBLR_DDL_CREATE_PROCEDURE", "procedure",
       "sys.catalog.procedure", "019f0000-0000-7000-8000-000000e30002",
       "replay_procedure", "SBSQL-B5E9C0943E63", "procedure_signature",
       "SBSQL-SURFACE-1C3307CC7B4E"},
      {"CREATE TRIGGER replay_trigger;", "SBSQL-5127560F8031",
       "create_trigger_stmt", "SBSQL-SURFACE-B1C95C652651",
       "ddl.create_trigger", "SBLR_DDL_CREATE_TRIGGER", "trigger",
       "sys.catalog.trigger", "019f0000-0000-7000-8000-000000e30003",
       "replay_trigger", "", "", ""},
      {"CREATE TRIGGER trig_items_ai AFTER INSERT ON TABLE trig_items FOR EACH ROW AS BEGIN INSERT INTO trig_audit (audit_id, event_kind, item_id, old_price, new_price, audit_note) VALUES (NEXT VALUE FOR trig_audit_seq, 'INSERT', new.item_id, NULL, new.item_price, 'item inserted'); END",
       "SBSQL-5127560F8031",
       "create_trigger_stmt", "SBSQL-SURFACE-B1C95C652651",
       "ddl.create_trigger", "SBLR_DDL_CREATE_TRIGGER", "trigger",
       "sys.catalog.trigger", "019f0000-0000-7000-8000-000000e30004",
       "trig_items_ai", "", "", ""},
  };
  return cases;
}

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

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.name << '=' << field.value << '\n';
    }
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000e30101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000e30102";
  session.database_uuid = "019f0000-0000-7000-8000-000000e30103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 71;
  session.security_policy_epoch = 72;
  session.descriptor_epoch = 73;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000e30104";
  config.bundle_contract_id = "sbp_sbsql@create-executable-route-test";
  config.build_id = "sbsql-create-executable-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql,
                              std::vector<std::string> resolved_object_uuids = {}) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            std::move(resolved_object_uuids));
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

PipelineArtifacts RunPipeline(const Case& route) {
  return RunPipeline(route.sql, {std::string(route.object_uuid)});
}

void RequireRegistryEvidence(const Case& route) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(route.surface_id);
  Require(registry_row != nullptr, "CREATE executable generated registry row missing");
  Require(registry_row->canonical_name == route.surface_name,
          "CREATE executable generated registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "CREATE executable generated registry kind drifted");
  Require(registry_row->family == "ddl_catalog",
          "CREATE executable generated registry family drifted");
  Require(registry_row->source_status == "native_now",
          "CREATE executable generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "CREATE executable generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == kFamily,
          "CREATE executable generated registry SBLR family drifted");
  Require(registry_row->validation_fixture_id == route.fixture_id,
          "CREATE executable generated registry fixture id drifted");

  if (!route.signature_surface_id.empty()) {
    const auto* signature_row =
        FindGeneratedSurfaceRegistryRowById(route.signature_surface_id);
    Require(signature_row != nullptr,
            "CREATE executable signature generated registry row missing");
    Require(signature_row->canonical_name == route.signature_surface_name,
            "CREATE executable signature generated registry canonical name drifted");
    Require(signature_row->surface_kind == "grammar_production",
            "CREATE executable signature generated registry kind drifted");
    Require(signature_row->family == "general",
            "CREATE executable signature generated registry family drifted");
    Require(signature_row->source_status == "native_now",
            "CREATE executable signature generated registry status drifted");
    Require(signature_row->cluster_scope == "noncluster_or_profile_scoped",
            "CREATE executable signature generated registry cluster scope drifted");
    Require(signature_row->sblr_operation_family == "sblr.general.operation.v3",
            "CREATE executable signature generated registry SBLR family drifted");
    Require(signature_row->validation_fixture_id == route.signature_fixture_id,
            "CREATE executable signature generated registry fixture id drifted");
  }
}

void RequireExactLowering(const Case& route, const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "CREATE executable CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE executable AST failed");
  Require(artifacts.bound.bound, "CREATE executable bind failed");
  Require(artifacts.verifier.admitted, "CREATE executable verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE executable operation family mismatch");
  Require(artifacts.envelope.operation_id == route.operation_id,
          "CREATE executable operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == route.operation_id,
          "CREATE executable engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == route.opcode,
          "CREATE executable SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "CREATE executable catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_executable_object_api_required"),
          "CREATE executable engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "CREATE executable MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "CREATE executable parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CREATE executable parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "CREATE executable lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "CREATE executable lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"create_executable_object_ddl\""),
          "CREATE executable payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, route.catalog_authority),
          "CREATE executable payload missing catalog authority");
  Require(Contains(artifacts.envelope.payload, route.object_kind),
          "CREATE executable payload missing object kind evidence");
  Require(Contains(artifacts.envelope.payload, "\"signature_descriptor_embedded\":false"),
          "CREATE executable payload overclaimed signature descriptor");
  Require(Contains(artifacts.envelope.payload, "\"body_text_included\":false"),
          "CREATE executable payload embedded body text");
  const bool expects_body_compilation =
      Contains(route.sql, " AS BEGIN ") || Contains(route.sql, "\nAS\nBEGIN");
  if (expects_body_compilation) {
    Require(Contains(artifacts.envelope.payload, "\"body_compilation_included\":true"),
            "CREATE executable payload missing compiled body descriptor");
    Require(Contains(artifacts.envelope.payload,
                     "\"compiled_body_provenance\":\"sbsql_udr_lowering\""),
            "CREATE executable payload missing SBSQL UDR lowering provenance");
    Require(Contains(artifacts.envelope.payload, "\"executor\":\"internal_procedure\""),
            "CREATE executable payload missing internal procedure executor descriptor");
  } else {
    Require(Contains(artifacts.envelope.payload, "\"body_compilation_included\":false"),
            "CREATE executable payload overclaimed body compilation");
  }
  Require(Contains(artifacts.envelope.payload, "\"runtime_invocation_included\":false"),
          "CREATE executable payload overclaimed runtime invocation");
  Require(Contains(artifacts.envelope.payload, route.surface_id),
          "CREATE executable payload missing row-identifiable surface evidence");
  if (!route.signature_surface_id.empty()) {
    Require(Contains(artifacts.envelope.payload, route.signature_surface_id),
            "CREATE executable payload missing signature row evidence");
  }
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":true") &&
              Contains(artifacts.envelope.payload,
                       "\"name_text_authority\":\"metadata_only_engine_name_registry\""),
          "CREATE executable payload did not confine name text to name-registry metadata");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "CREATE executable payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CREATE executable payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, route.sql),
          "CREATE executable payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "CREATE executable payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CREATE executable payload carried WAL/recovery authority");
}

void RequireServerAdmission(const Case& route, const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected CREATE executable exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for CREATE executable");
  Require(admission.operation_id == route.operation_id,
          "server admission CREATE executable operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission CREATE executable operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(route.operation_id);
  Require(opcode_entry != nullptr, "CREATE executable opcode registry row missing");
  Require(opcode_entry->opcode == route.opcode,
          "CREATE executable opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "CREATE executable opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "CREATE executable opcode registry transaction context drifted");
}

void RequireCursorRoutineArgumentRoute() {
  const Case route{kCursorProcedureSql,
                   "SBSQL-13F5A8364A50",
                   "create_procedure_stmt",
                   "SBSQL-SURFACE-515475BB02FD",
                   "ddl.create_procedure",
                   "SBLR_DDL_CREATE_PROCEDURE",
                   "procedure",
                   "sys.catalog.procedure",
                   "019f0000-0000-7000-8000-000000e30022",
                   "replay_cursor_procedure",
                   "SBSQL-B5E9C0943E63",
                   "procedure_signature",
                   "SBSQL-SURFACE-1C3307CC7B4E"};
  const auto artifacts = RunPipeline(route);
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "cursor routine CST failed");
  Require(!artifacts.ast.messages.has_errors(), "cursor routine AST failed");
  Require(artifacts.bound.bound, "cursor routine bind failed");
  Require(artifacts.verifier.admitted, "cursor routine verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "cursor routine operation family mismatch");
  Require(artifacts.envelope.operation_id == route.operation_id,
          "cursor routine operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == route.operation_id,
          "cursor routine engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == route.opcode,
          "cursor routine SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_executable_object_api_required"),
          "cursor routine engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "cursor routine parser no-SQL-execution authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.routine.parameter_descriptor"),
          "cursor routine parameter descriptor ref missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.server.cursor_descriptor"),
          "cursor routine cursor descriptor ref missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.routine.cursor_parameter_descriptor"),
          "cursor routine cursor parameter descriptor ref missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "cursor routine lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "cursor routine lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"signature_descriptor_embedded\":true"),
          "cursor routine payload did not embed signature descriptor");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_parameter_descriptor_present\":true"),
          "cursor routine payload missing parameter descriptor");
  Require(Contains(artifacts.envelope.payload, "\"routine_parameter_count\":1"),
          "cursor routine payload missing parameter count");
  Require(Contains(artifacts.envelope.payload, "\"routine_parameter_0_type\":\"cursor\""),
          "cursor routine payload missing cursor parameter type");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_parameter_0_descriptor_kind\":\"cursor_handle\""),
          "cursor routine payload missing cursor handle descriptor kind");
  Require(Contains(artifacts.envelope.payload, "\"routine_cursor_argument\":true"),
          "cursor routine payload missing cursor argument flag");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_cursor_argument_binding\":\"descriptor.cursor_handle.session_registry\""),
          "cursor routine payload missing cursor binding authority");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_cursor_argument_parser_executes_cursor\":false"),
          "cursor routine payload overclaimed parser cursor execution");
  Require(Contains(artifacts.envelope.payload, "SBSQL-0B00DEA678E2") &&
              Contains(artifacts.envelope.payload, "SBSQL-C5D151D17944") &&
              Contains(artifacts.envelope.payload, "SBSQL-1D1D3395F617") &&
              Contains(artifacts.envelope.payload, "SBSQL-02CE40320417"),
          "cursor routine payload missing parameter/cursor surface evidence");
  Require(!Contains(artifacts.envelope.payload, "route_cursor") &&
              !Contains(artifacts.envelope.payload, route.sql),
          "cursor routine payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "cursor routine payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "cursor routine payload carried WAL/recovery authority");
  RequireServerAdmission(route, artifacts.envelope);
}

void RequireRoutineInvocationRoute() {
  constexpr std::string_view kProcedureUuid = "019f0000-0000-7000-8000-000000e30042";
  constexpr std::string_view kProcedureSchemaUuid = "019f0000-0000-7000-8000-000000e300ff";
  const auto artifacts = RunPipeline(
      "EXECUTE PROCEDURE replay_procedure(1);",
      {std::string(kProcedureUuid), std::string(kProcedureSchemaUuid)});
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "routine invocation CST failed");
  Require(!artifacts.ast.messages.has_errors(), "routine invocation AST failed");
  Require(artifacts.bound.bound, "routine invocation bind failed");
  Require(artifacts.verifier.admitted, "routine invocation verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.routine.execute.v3",
          "routine invocation operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.routine.execute.v3",
          "routine invocation operation key mismatch");
  Require(artifacts.envelope.operation_id == "routine.procedure_invoke",
          "routine invocation operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "routine.procedure_invoke",
          "routine invocation engine API operation id mismatch");
  Require(artifacts.envelope.engine_api_function == "EngineInvokeExecutableObject",
          "routine invocation engine API function mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_PROCEDURE_INVOKE",
          "routine invocation SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.execute"),
          "routine invocation execute right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.sblr_uuid_admission_required"),
          "routine invocation server UUID admission step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.executable_object_invoke_api_required"),
          "routine invocation engine executable-object authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "routine invocation parser no-SQL-execution authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.catalog.procedure"),
          "routine invocation procedure catalog descriptor ref missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.routine_descriptor"),
          "routine invocation routine descriptor ref missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "routine invocation lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "routine invocation lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload, "\"routine_invocation\":true"),
          "routine invocation payload missing invocation marker");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_invocation_kind\":\"procedure\""),
          "routine invocation payload missing procedure marker");
  Require(Contains(artifacts.envelope.payload,
                   "\"runtime_invocation_included\":true"),
          "routine invocation payload missing runtime invocation marker");
  Require(Contains(artifacts.envelope.payload,
                   "\"server_revalidates_sblr_uuid\":true"),
          "routine invocation payload missing server revalidation marker");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_argument_count\":1"),
          "routine invocation payload missing argument count");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_argument_0_type\":\"bigint\""),
          "routine invocation payload missing literal argument type");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_object_uuid\":\"019f0000-0000-7000-8000-000000e30042\""),
          "routine invocation payload missing target UUID");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "routine invocation payload did not prove parser_executes_sql=false");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_routine\":false"),
          "routine invocation payload did not prove parser_executes_routine=false");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "routine invocation payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "EXECUTE PROCEDURE") &&
              !Contains(artifacts.envelope.payload, "replay_procedure(1)") &&
              !Contains(artifacts.envelope.payload, "reference"),
          "routine invocation payload embedded source SQL text or reference authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected routine invocation route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for routine invocation");
  Require(admission.operation_id == "routine.procedure_invoke",
          "server admission routine invocation operation id mismatch");
  Require(admission.operation_family == "sblr.routine.execute.v3",
          "server admission routine invocation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation("routine.procedure_invoke");
  Require(opcode_entry != nullptr, "routine invocation opcode registry row missing");
  Require(opcode_entry->opcode == "SBLR_PROCEDURE_INVOKE",
          "routine invocation opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "routine invocation opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "routine invocation opcode registry transaction context drifted");
}

void RequireCompiledProcedureBodyRoute() {
  const std::string sql =
      "CREATE PROCEDURE users.public.proc_process_tasks(\n"
      "    p_min_priority INTEGER\n"
      ")\n"
      "AS\n"
      "DECLARE VARIABLE v_task_id    INTEGER DEFAULT 0;\n"
      "DECLARE VARIABLE v_task_label VARCHAR(128) DEFAULT '';\n"
      "DECLARE VARIABLE v_priority   INTEGER DEFAULT 0;\n"
      "DECLARE VARIABLE v_class      VARCHAR(32) DEFAULT 'normal';\n"
      "BEGIN\n"
      "  FOR SELECT task_id, task_label, priority\n"
      "        FROM users.public.proc_tasks\n"
      "       WHERE priority >= p_min_priority\n"
      "       ORDER BY priority DESC\n"
      "        INTO v_task_id, v_task_label, v_priority\n"
      "  DO\n"
      "  BEGIN\n"
      "    INSERT INTO users.public.proc_results\n"
      "      (result_id, task_id, result_text)\n"
      "    VALUES\n"
      "      (NEXT VALUE FOR users.public.proc_result_seq,\n"
      "       v_task_id,\n"
      "       v_class || ':' || v_task_label);\n"
      "  END\n"
      "END";
  const auto artifacts = RunPipeline(
      sql,
      {"019f0000-0000-7000-8000-000000e33001",
       "019f0000-0000-7000-8000-000000e33002",
       "019f0000-0000-7000-8000-000000e33003",
       "019f0000-0000-7000-8000-000000e33004"});
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "compiled procedure CST failed");
  Require(!artifacts.ast.messages.has_errors(), "compiled procedure AST failed");
  Require(artifacts.bound.bound, "compiled procedure bind failed");
  Require(artifacts.verifier.admitted, "compiled procedure verifier rejected exact route");
  Require(Contains(artifacts.envelope.payload,
                   "\"body_compilation_included\":true"),
          "compiled procedure body was not represented as compiled descriptor");
  Require(Contains(artifacts.envelope.payload,
                   "\"compiled_body_descriptor\":\"sbsql.compiled.procedural.process_tasks.v1\""),
          "compiled procedure descriptor mismatch");
  Require(!Contains(artifacts.envelope.payload, "FOR SELECT") &&
              !Contains(artifacts.envelope.payload, "INSERT INTO"),
          "compiled procedure payload embedded source SQL text");
}

void RequireCompiledScalarFunctionBodyRoute(std::string sql,
                                            std::string_view expected_descriptor,
                                            std::string_view failure_label,
                                            std::vector<std::string> resolved_object_uuids) {
  const auto artifacts = RunPipeline(
      sql,
      std::move(resolved_object_uuids));
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(),
          std::string(failure_label) + " CST failed");
  Require(!artifacts.ast.messages.has_errors(),
          std::string(failure_label) + " AST failed");
  Require(artifacts.bound.bound, std::string(failure_label) + " bind failed");
  Require(artifacts.verifier.admitted,
          std::string(failure_label) + " verifier rejected exact route");
  const std::string expected =
      "\"compiled_body_descriptor\":\"" + std::string(expected_descriptor) + "\"";
  if (!Contains(artifacts.envelope.payload, expected)) {
    std::cerr << artifacts.envelope.payload << '\n';
  }
  Require(Contains(artifacts.envelope.payload, expected),
          std::string(failure_label) + " descriptor mismatch");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_return_descriptor_present\":true"),
          std::string(failure_label) + " missing return descriptor");
  Require(!Contains(artifacts.envelope.payload, "DECLARE VARIABLE") &&
              !Contains(artifacts.envelope.payload, "RETURN ") &&
              !Contains(artifacts.envelope.payload, "WHILE ") &&
              !Contains(artifacts.envelope.payload, "IF "),
          std::string(failure_label) + " payload embedded source SQL text");
}

void RequireCompiledScalarFunctionBodyRoutes() {
  RequireCompiledScalarFunctionBodyRoute(
      "CREATE FUNCTION users.public.fsce_exact_probe.fn_classify_amount(\n"
      "    p_amount DECIMAL(18,2)\n"
      ")\n"
      "RETURNS VARCHAR(32)\n"
      "DETERMINISTIC\n"
      "RETURNS NULL ON NULL INPUT\n"
      "AS\n"
      "DECLARE VARIABLE v_label VARCHAR(32) DEFAULT 'standard';\n"
      "BEGIN\n"
      "  IF p_amount >= 1000 THEN\n"
      "  BEGIN\n"
      "    v_label = 'large';\n"
      "  END\n"
      "  ELSE\n"
      "  BEGIN\n"
      "    IF p_amount <= 0 THEN\n"
      "    BEGIN\n"
      "      v_label = 'zero_or_negative';\n"
      "    END\n"
      "    ELSE\n"
      "    BEGIN\n"
      "      v_label = 'standard';\n"
      "    END\n"
      "  END\n"
      "  RETURN v_label;\n"
      "END",
      "sbsql.compiled.procedural.classify_amount.v1",
      "compiled classify function",
      {"019f0000-0000-7000-8000-000000e33201"});
  RequireCompiledScalarFunctionBodyRoute(
      "CREATE FUNCTION users.public.fsce_exact_probe.fn_factorial(\n"
      "    p_n INTEGER\n"
      ")\n"
      "RETURNS BIGINT\n"
      "DETERMINISTIC\n"
      "CALLED ON NULL INPUT\n"
      "AS\n"
      "DECLARE VARIABLE v_result BIGINT DEFAULT 1;\n"
      "DECLARE VARIABLE v_i      INTEGER DEFAULT 1;\n"
      "BEGIN\n"
      "  IF p_n IS NULL THEN\n"
      "  BEGIN\n"
      "    RETURN NULL;\n"
      "  END\n"
      "  IF p_n < 0 THEN\n"
      "  BEGIN\n"
      "    RETURN NULL;\n"
      "  END\n"
      "  WHILE v_i <= p_n DO\n"
      "  BEGIN\n"
      "    v_result = v_result * v_i;\n"
      "    v_i = v_i + 1;\n"
      "  END\n"
      "  RETURN v_result;\n"
      "END",
      "sbsql.compiled.procedural.factorial.v1",
      "compiled factorial function",
      {"019f0000-0000-7000-8000-000000e33202"});
  RequireCompiledScalarFunctionBodyRoute(
      "CREATE FUNCTION users.public.fsce_exact_probe.fn_safe_divide(\n"
      "    p_numerator   DECIMAL(18,6),\n"
      "    p_denominator DECIMAL(18,6)\n"
      ")\n"
      "RETURNS DECIMAL(18,6)\n"
      "DETERMINISTIC\n"
      "CALLED ON NULL INPUT\n"
      "AS\n"
      "DECLARE VARIABLE v_result DECIMAL(18,6) DEFAULT NULL;\n"
      "BEGIN\n"
      "  IF p_numerator IS NULL OR p_denominator IS NULL THEN\n"
      "  BEGIN\n"
      "    RETURN NULL;\n"
      "  END\n"
      "  IF p_denominator = 0 THEN\n"
      "  BEGIN\n"
      "    RETURN NULL;\n"
      "  END\n"
      "  v_result = p_numerator / p_denominator;\n"
      "  RETURN v_result;\n"
      "END",
      "sbsql.compiled.procedural.safe_divide.v1",
      "compiled safe_divide function",
      {"019f0000-0000-7000-8000-000000e33203"});
}

void RequireCompiledTriggerBodyRoute() {
  const std::string sql =
      "CREATE TRIGGER users.public.trig_items_ai\n"
      "AFTER INSERT\n"
      "ON TABLE users.public.trig_items\n"
      "FOR EACH ROW\n"
      "AS\n"
      "BEGIN\n"
      "  INSERT INTO users.public.trig_audit\n"
      "    (audit_id, event_kind, item_id, old_price, new_price, audit_note)\n"
      "  VALUES\n"
      "    (NEXT VALUE FOR users.public.trig_audit_seq,\n"
      "     'INSERT',\n"
      "     new.item_id,\n"
      "     NULL,\n"
      "     new.item_price,\n"
      "     'item inserted');\n"
      "END";
  const auto artifacts = RunPipeline(
      sql,
      {"019f0000-0000-7000-8000-000000e33101",
       "019f0000-0000-7000-8000-000000e33102",
       "019f0000-0000-7000-8000-000000e33103",
       "019f0000-0000-7000-8000-000000e33104"});
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "compiled trigger CST failed");
  Require(!artifacts.ast.messages.has_errors(), "compiled trigger AST failed");
  Require(artifacts.bound.bound, "compiled trigger bind failed");
  Require(artifacts.verifier.admitted, "compiled trigger verifier rejected exact route");
  Require(Contains(artifacts.envelope.payload,
                   "\"compiled_body_descriptor\":\"sbsql.compiled.trigger.audit_after_insert_row.v1\""),
          "compiled trigger descriptor mismatch");
  Require(Contains(artifacts.envelope.payload,
                   "\"trigger_timing\":\"after\""),
          "compiled trigger timing missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"trigger_scope\":\"row\""),
          "compiled trigger scope missing");
  Require(!Contains(artifacts.envelope.payload, "INSERT INTO"),
          "compiled trigger payload embedded source SQL text");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_create_executable_exact_route_" + std::to_string(CurrentUnixMillis()) +
          ".sbdb");
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810580000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810580001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810580002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CREATE executable engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-executable-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000e30201";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000e30202";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:create_executable_exact_route");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.create_executable.exact_route.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "transaction begin envelope did not validate");
  Require(result.accepted, "transaction begin dispatch did not accept");
  Require(result.api_result.ok, "transaction begin did not return success");
  Require(result.api_result.local_transaction_id != 0,
          "transaction begin did not return local transaction id");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

void AddTextOperand(sblr::SblrOperationEnvelope* envelope,
                    std::string name,
                    std::string value) {
  envelope->operands.push_back({"text", std::move(name), std::move(value)});
}

sblr::SblrOperationEnvelope EngineEnvelope(const Case& route) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(route.operation_id),
                                         std::string(route.opcode),
                                         "trace.create_executable.exact_route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  AddTextOperand(&envelope, "target_object_uuid", std::string(route.object_uuid));
  AddTextOperand(&envelope, "target_object_kind", std::string(route.object_kind));
  AddTextOperand(&envelope, std::string(route.object_kind) + "_object_uuid",
                 std::string(route.object_uuid));
  AddTextOperand(&envelope, std::string(route.object_kind) + "_name",
                 std::string(route.object_name));
  AddTextOperand(&envelope, "target_schema_uuid", std::string(kSchemaUuid));
  AddTextOperand(&envelope, "executable_object_kind", std::string(route.object_kind));
  AddTextOperand(&envelope, "signature_descriptor_kind", "deferred_signature_descriptor");
  AddTextOperand(&envelope, "permission", "manage_executable");
  return envelope;
}

void RequireEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginEngineTransaction(path, database_uuid);

  for (const auto& route : Cases()) {
    const sblr::SblrDispatchRequest request{
        context,
        EngineEnvelope(route),
        api::EngineApiRequest{}};
    const auto result = sblr::DispatchSblrOperation(request);
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Require(result.envelope_validated, "engine SBLR envelope did not validate");
    Require(result.accepted, "engine SBLR dispatch did not accept CREATE executable");
    Require(result.dispatched_to_api,
            "engine SBLR dispatch did not route CREATE executable to internal API");
    Require(result.api_result.ok, "EngineCreate executable did not return success");
    Require(result.api_result.operation_id == route.operation_id,
            "EngineCreate executable returned wrong operation id");
    Require(result.api_result.primary_object.object_kind == route.object_kind,
            "EngineCreate executable returned wrong primary object kind");
    Require(result.api_result.primary_object.uuid.canonical == route.object_uuid,
            "EngineCreate executable returned wrong object UUID");
    Require(HasEvidence(result.api_result, "api_behavior_event", route.operation_id),
            "EngineCreate executable missing API behavior event evidence");
    Require(HasEvidence(result.api_result, route.object_kind, route.object_uuid),
            "EngineCreate executable missing descriptor evidence");
    Require(HasEvidence(result.api_result, "name_registry", route.object_uuid),
            "EngineCreate executable missing name registry evidence");
    Require(!result.api_result.catalog_row_uuid.canonical.empty(),
            "EngineCreate executable missing catalog row UUID evidence");
  }
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  for (const auto& route : Cases()) {
    RequireRegistryEvidence(route);
    const auto artifacts = RunPipeline(route);
    RequireExactLowering(route, artifacts);
    RequireServerAdmission(route, artifacts.envelope);
  }
  RequireCursorRoutineArgumentRoute();
  RequireRoutineInvocationRoute();
  RequireCompiledScalarFunctionBodyRoutes();
  RequireCompiledProcedureBodyRoute();
  RequireCompiledTriggerBodyRoute();
  RequireEngineDispatch();
  std::cout << "sbsql_create_executable_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
