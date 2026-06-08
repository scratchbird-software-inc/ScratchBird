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
#include "rendering/rendering.hpp"
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
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000078001";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view engine_api_function;
  std::string_view runtime_evidence_kind;
  std::string_view runtime_evidence_id;
  bool requires_transaction_context;
};

const CaseRow kCases[] = {
    {"SBSQL-026A4D4C039B", "psql_repeat_stmt", "grammar_production", "PSQL REPEAT LOOP;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_repeat_stmt", true},
    {"SBSQL-02734A0F9F81", "forall_dml_or_execute", "grammar_production", "FORALL DML EXECUTE;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "forall_dml_or_execute", true},
    {"SBSQL-036A5CE9F957", "psql_for_stmt", "grammar_production", "PSQL FOR item IN range;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_for_stmt", true},
    {"SBSQL-07486BB23A2F", "declare_subroutine", "grammar_production", "DECLARE SUBROUTINE helper;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "declare_subroutine", true},
    {"SBSQL-0E3954A70810", "routine_attribute", "grammar_production", "ROUTINE ATTRIBUTE DETERMINISTIC;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "routine_attribute", true},
    {"SBSQL-11A04416EEDE", "psql_call_stmt", "grammar_production", "PSQL CALL helper();", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_call_stmt", true},
    {"SBSQL-198EC86EF3E6", "forall_range", "grammar_production", "FORALL RANGE 1 TO 10;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "forall_range", true},
    {"SBSQL-24F101012F9C", "psql_forall_stmt", "grammar_production", "PSQL FORALL item IN range;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_forall_stmt", true},
    {"SBSQL-28A5C4933A91", "psql_leave_stmt", "grammar_production", "PSQL LEAVE loop_label;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_leave_stmt", true},
    {"SBSQL-2B96962FC600", "signal_info_field", "grammar_production", "SIGNAL INFO FIELD MESSAGE_TEXT;", "general.signal_diagnostic", "SBLR_GENERAL_SIGNAL_DIAGNOSTIC", "EngineSignalDiagnostic", "diagnostic_signal", "signal_info_field", false},
    {"SBSQL-2E73B3E7CB0A", "colon_variable", "grammar_production", "COLON VARIABLE :v;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "colon_variable", true},
    {"SBSQL-2E7EF3FB699A", "declare_variable", "grammar_production", "DECLARE VARIABLE v INT;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "declare_variable", true},
    {"SBSQL-375E2A7771C0", "exception_handler", "grammar_production", "EXCEPTION HANDLER WHEN ANY;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "exception_handler", true},
    {"SBSQL-3EDACF124EA2", "call_target_list", "grammar_production", "CALL TARGET LIST helper;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "call_target_list", true},
    {"SBSQL-3FE17C7E606A", "psql_open_channel_stmt", "grammar_production", "PSQL OPEN CHANNEL events;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_open_channel_stmt", true},
    {"SBSQL-451E4A81B23D", "raise_severity", "grammar_production", "RAISE SEVERITY WARNING;", "general.raise_diagnostic", "SBLR_GENERAL_RAISE_DIAGNOSTIC", "EngineRaiseDiagnostic", "diagnostic_raise", "raise_severity", false},
    {"SBSQL-47E79B4B23EF", "lvalue", "grammar_production", "LVALUE local_var;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "lvalue", true},
    {"SBSQL-499A72248451", "declare_exception", "grammar_production", "DECLARE EXCEPTION ex;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "declare_exception", true},
    {"SBSQL-4A737A655174", "signal", "canonical_surface", "SIGNAL SQLSTATE '45000';", "general.signal_diagnostic", "SBLR_GENERAL_SIGNAL_DIAGNOSTIC", "EngineSignalDiagnostic", "diagnostic_signal", "signal", false},
    {"SBSQL-4B4DAC62299D", "variable_decl_form", "grammar_production", "VARIABLE DECL FORM v INT;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "variable_decl_form", true},
    {"SBSQL-5AD1F33585EA", "single_var_form", "grammar_production", "SINGLE VAR FORM v;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "single_var_form", true},
    {"SBSQL-5AFD1BFCCEC8", "psql_null_stmt", "grammar_production", "PSQL NULL;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_null_stmt", true},
    {"SBSQL-62256BEF9F1B", "call_arg_list", "grammar_production", "CALL ARG LIST a b;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "call_arg_list", true},
    {"SBSQL-66B35A56EFF8", "arg_list", "grammar_production", "ARG LIST a b;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "arg_list", true},
    {"SBSQL-6D4DE2A31C56", "param_mode", "grammar_production", "PARAM MODE INOUT;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "param_mode", true},
    {"SBSQL-6EF52D5CB31E", "exception_condition_list", "grammar_production", "EXCEPTION CONDITION LIST any;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "exception_condition_list", true},
    {"SBSQL-6FABEBB2C400", "routine_body", "grammar_production", "ROUTINE BODY BEGIN END;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "routine_body", true},
    {"SBSQL-7177C130C2B7", "for_range_form", "grammar_production", "FOR RANGE FORM 1 TO 10;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "for_range_form", true},
    {"SBSQL-7359F2775921", "psql_resignal_stmt", "grammar_production", "PSQL RESIGNAL;", "general.resignal_diagnostic", "SBLR_GENERAL_RESIGNAL_DIAGNOSTIC", "EngineResignalDiagnostic", "diagnostic_resignal", "psql_resignal_stmt", false},
    {"SBSQL-74BE46D58008", "psql_declare_section", "grammar_production", "PSQL DECLARE SECTION;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_declare_section", true},
    {"SBSQL-769B003AF4F3", "package_body_item", "grammar_production", "PACKAGE BODY ITEM proc;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "package_body_item", true},
    {"SBSQL-802635EDBB3A", "signal_info_assignment", "grammar_production", "SIGNAL INFO ASSIGNMENT MESSAGE_TEXT;", "general.signal_diagnostic", "SBLR_GENERAL_SIGNAL_DIAGNOSTIC", "EngineSignalDiagnostic", "diagnostic_signal", "signal_info_assignment", false},
    {"SBSQL-81BCBF791042", "package_name", "grammar_production", "PACKAGE NAME pkg;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "package_name", true},
    {"SBSQL-832C2821017E", "psql_while_stmt", "grammar_production", "PSQL WHILE cond LOOP;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_while_stmt", true},
    {"SBSQL-85A5F7E16A21", "psql_emit_channel_stmt", "grammar_production", "PSQL EMIT CHANNEL events;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_emit_channel_stmt", true},
    {"SBSQL-8628143A198B", "psql_assignment", "grammar_production", "PSQL ASSIGN var VALUE;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_assignment", true},
    {"SBSQL-908F3A07EC23", "psql_get_diagnostics", "grammar_production", "PSQL GET DIAGNOSTICS;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_get_diagnostics", true},
    {"SBSQL-9164E0190F24", "into_target_list", "grammar_production", "INTO TARGET LIST var;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "into_target_list", true},
    {"SBSQL-91D6ECC8969F", "signal_target", "grammar_production", "SIGNAL TARGET condition;", "general.signal_diagnostic", "SBLR_GENERAL_SIGNAL_DIAGNOSTIC", "EngineSignalDiagnostic", "diagnostic_signal", "signal_target", false},
    {"SBSQL-931C105F4478", "raise", "canonical_surface", "RAISE EXCEPTION;", "general.raise_diagnostic", "SBLR_GENERAL_RAISE_DIAGNOSTIC", "EngineRaiseDiagnostic", "diagnostic_raise", "raise", false},
    {"SBSQL-96CFEF2C7728", "exception_declaration", "grammar_production", "EXCEPTION DECLARATION ex;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "exception_declaration", true},
    {"SBSQL-A5437DC15591", "raise_option", "grammar_production", "RAISE OPTION MESSAGE_TEXT;", "general.raise_diagnostic", "SBLR_GENERAL_RAISE_DIAGNOSTIC", "EngineRaiseDiagnostic", "diagnostic_raise", "raise_option", false},
    {"SBSQL-A5AA36E99CDB", "psql_return_stmt", "grammar_production", "PSQL RETURN value;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_return_stmt", true},
    {"SBSQL-A61AE21E1DFC", "psql_signal_stmt", "grammar_production", "PSQL SIGNAL SQLSTATE;", "general.signal_diagnostic", "SBLR_GENERAL_SIGNAL_DIAGNOSTIC", "EngineSignalDiagnostic", "diagnostic_signal", "psql_signal_stmt", false},
    {"SBSQL-A61F84867DF2", "diagnostic_filter", "grammar_production", "DIAGNOSTIC FILTER condition;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "diagnostic_filter", true},
    {"SBSQL-A67B68A9BB52", "diagnostic_family", "grammar_production", "DIAGNOSTIC FAMILY exception;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "diagnostic_family", true},
    {"SBSQL-AE02AD3F3CF7", "psql_loop_stmt", "grammar_production", "PSQL LOOP;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "psql_loop_stmt", true},
    {"SBSQL-AFAE77165146", "psql_raise_stmt", "grammar_production", "PSQL RAISE WARNING;", "general.raise_diagnostic", "SBLR_GENERAL_RAISE_DIAGNOSTIC", "EngineRaiseDiagnostic", "diagnostic_raise", "psql_raise_stmt", false},
    {"SBSQL-AFF3B4857945", "return_shape", "grammar_production", "RETURN SHAPE scalar;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "return_shape", true},
    {"SBSQL-BA6B29FD2668", "exception_section", "grammar_production", "EXCEPTION SECTION WHEN ANY;", "general.procedural_operation", "SBLR_GENERAL_PROCEDURAL_OPERATION", "EngineGeneralProceduralOperation", "procedural_general_route", "exception_section", true},
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
  session.session_uuid = "019f0000-0000-7000-8000-000000078101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000078102";
  session.database_uuid = "019f0000-0000-7000-8000-000000078103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 78;
  session.security_policy_epoch = 79;
  session.descriptor_epoch = 80;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_078_procedural_general_residual";
  config.parser_uuid = "019f0000-0000-7000-8000-000000078104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-078-procedural-general-residual";
  config.build_id = "sbsql-sbsfc-078-procedural-general-residual";
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
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-078 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-078 generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "SBSFC-078 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-078 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-078 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-078 generated registry SBLR family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-078 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-078 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          std::string("SBSFC-078 AST row surface id mismatch for ") +
              std::string(row.canonical_name) + ": got " +
              artifacts.ast.statement_surface_id + " expected " +
              std::string(row.surface_id));
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          "SBSFC-078 AST canonical name mismatch");
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          "SBSFC-078 AST registry family mismatch");
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-078 AST operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-078 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-078 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.general.operation.v3",
          std::string("SBSFC-078 operation family mismatch for ") +
              std::string(row.canonical_name) + ": got " +
              artifacts.envelope.operation_family);
  Require(artifacts.envelope.sblr_operation_key == "sblr.general.operation.v3",
          "SBSFC-078 operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "SBSFC-078 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "SBSFC-078 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode, "SBSFC-078 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == row.engine_api_function,
          "SBSFC-078 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-078 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-078 parser no-finality authority missing");
  if (row.requires_transaction_context) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.server.transaction_context_required"),
            "SBSFC-078 transaction context authority missing");
  }
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-078 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_kind),
          "SBSFC-078 payload missing runtime evidence kind");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_id),
          "SBSFC-078 payload missing runtime evidence id");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-078 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-078 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-078 payload used replay, refusal, or cluster-provider evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-078 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-078 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-078 server admission did not require public ABI dispatch");
  Require(admission.operation_id == row.operation_id,
          "SBSFC-078 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.general.operation.v3",
          "SBSFC-078 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation(std::string(row.operation_id));
  Require(opcode != nullptr, "SBSFC-078 opcode registry row missing");
  Require(opcode->opcode == row.opcode, "SBSFC-078 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-078 opcode claimed cluster authority");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_078_procedural_general_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.transaction_inventory", ".dirty.manifest",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810780000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810780001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810780002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "SBSFC-078 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-078-procedural-general-residual";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000078201";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000078202";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000078203";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc078.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const auto result = sblr::DispatchSblrOperation({context, envelope, api::EngineApiRequest{}});
  Require(result.envelope_validated, "SBSFC-078 transaction begin envelope rejected");
  Require(result.accepted, "SBSFC-078 transaction begin not accepted");
  Require(result.api_result.ok, "SBSFC-078 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  return context;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "trace.sbsfc078.procedural_general");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.requires_transaction_context;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "sbsfc078_procedural_surface"});
  envelope.operands.push_back({"text", "sbsfc078_surface_id", std::string(row.surface_id)});
  if (row.operation_id == "general.procedural_operation") {
    envelope.operands.push_back({"text", "procedural_route_kind", std::string(row.canonical_name)});
  } else {
    envelope.operands.push_back({"text", "diagnostic_route_kind", std::string(row.canonical_name)});
  }
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "sbsfc078_procedural_surface";
  request.option_envelopes.push_back(std::string("sbsfc078_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back(std::string("surface_kind:") + std::string(row.canonical_name));
  if (row.operation_id == "general.procedural_operation") {
    request.option_envelopes.push_back(std::string("procedural_route_kind:") + std::string(row.canonical_name));
    request.option_envelopes.push_back("procedural_ir:validated_descriptor");
  } else {
    request.option_envelopes.push_back(std::string("diagnostic_route_kind:") + std::string(row.canonical_name));
    request.diagnostic_options.push_back("sqlstate:45000");
    request.diagnostic_options.push_back("message:row_diagnostic_without_source_sql");
  }
  return request;
}

void RequireEngineDispatch(const api::EngineRequestContext& base_context,
                           const std::filesystem::path& path,
                           const std::string& database_uuid,
                           const CaseRow& row) {
  api::EngineRequestContext context = base_context;
  if (row.requires_transaction_context) {
    context = BeginEngineTransaction(path, database_uuid);
  }
  const auto result = sblr::DispatchSblrOperation({context, EngineEnvelope(row), ApiRequestFor(row)});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-078 engine envelope rejected");
  Require(result.accepted, "SBSFC-078 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-078 engine did not dispatch to API");
  Require(result.api_result.operation_id == row.operation_id,
          "SBSFC-078 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-078 runtime API did not complete");
  Require(HasEvidence(result.api_result, row.runtime_evidence_kind, row.runtime_evidence_id),
          "SBSFC-078 runtime evidence missing");
  Require(HasEvidence(result.api_result, "sbsfc078_surface", row.surface_id),
          "SBSFC-078 runtime did not carry row surface evidence");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-078 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-078 runtime allowed parser SQL execution");
}

}  // namespace

int main() {
  static_assert(sizeof(kCases) / sizeof(kCases[0]) == 50);
  for (const auto& row : kCases) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto base_context = EngineContext(path, database_uuid);
  for (const auto& row : kCases) {
    RequireEngineDispatch(base_context, path, database_uuid, row);
  }
  RemoveDatabaseArtifacts(path);

  std::cout << "sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
