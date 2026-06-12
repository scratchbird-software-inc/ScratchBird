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
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

struct ObservabilityRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view validation_fixture_id;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view resolved_object_uuid;
};

constexpr std::array<ObservabilityRowEvidence, 32> kObservabilityRows{{
    {"SBSQL-B7EF40AE00EB",
     "show_identity_session",
     "grammar_production",
     "SBSQL-SURFACE-58A4E6258E2F",
     "SHOW VERSION",
     "observability.show_version",
     "SBLR_OBSERVABILITY_SHOW_VERSION",
     ""},
    {"SBSQL-B7EF40AE00EB",
     "show_identity_session",
     "grammar_production",
     "SBSQL-SURFACE-58A4E6258E2F",
     "SHOW DATABASE",
     "observability.show_database",
     "SBLR_OBSERVABILITY_SHOW_DATABASE",
     ""},
    {"SBSQL-2A0B29C713C7",
     "show",
     "canonical_surface",
     "SBSQL-SURFACE-C1BB2613E82A",
     "SHOW METRICS",
     "observability.show_metrics",
     "SBLR_OBSERVABILITY_SHOW_METRICS",
     ""},
    {"SBSQL-9481A549CA44",
     "show_metrics_observability",
     "grammar_production",
     "SBSQL-SURFACE-493BC3E58E59",
     "SHOW METRICS",
     "observability.show_metrics",
     "SBLR_OBSERVABILITY_SHOW_METRICS",
     ""},
    {"SBSQL-564D768C8ADF",
     "show_stmt",
     "grammar_production",
     "SBSQL-SURFACE-8337E4AC144C",
     "SHOW METRICS",
     "observability.show_metrics",
     "SBLR_OBSERVABILITY_SHOW_METRICS",
     ""},
    {"SBSQL-837C1681258A",
     "show_target",
     "grammar_production",
     "SBSQL-SURFACE-9259EFBE11A1",
     "SHOW SESSIONS",
     "observability.show_sessions",
     "SBLR_OBSERVABILITY_SHOW_SESSIONS",
     ""},
    {"SBSQL-E72BE3893672",
     "show_objects",
     "grammar_production",
     "SBSQL-SURFACE-6F059367D286",
     "SHOW OBJECTS",
     "observability.show_catalog",
     "SBLR_OBSERVABILITY_SHOW_CATALOG",
     ""},
    {"SBSQL-E6A0DDD9D2D7",
     "show_jobs",
     "grammar_production",
     "SBSQL-SURFACE-823E8FDBDEC4",
     "SHOW JOBS",
     "observability.show_jobs",
     "SBLR_OBSERVABILITY_SHOW_JOBS",
     ""},
    {"SBSQL-556812D92760",
     "show_management",
     "grammar_production",
     "SBSQL-SURFACE-CF4AA6A99B37",
     "SHOW MANAGEMENT",
     "observability.show_management",
     "SBLR_OBSERVABILITY_SHOW_MANAGEMENT",
     ""},
    {"SBSQL-0DA421E46090",
     "show_diagnostics",
     "grammar_production",
     "SBSQL-SURFACE-F37A830E2B2B",
     "SHOW DIAGNOSTICS",
     "observability.show_diagnostics",
     "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS",
     ""},
    {"SBSQL-D2617A466C64",
     "show_diagnostics_extended",
     "grammar_production",
     "SBSQL-SURFACE-1B0D1E6663CD",
     "SHOW DIAGNOSTICS EXTENDED",
     "observability.show_diagnostics_extended",
     "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED",
     ""},
    {"SBSQL-669919A582BD",
     "show_archive_replication",
     "grammar_production",
     "SBSQL-SURFACE-5846604B881B",
     "SHOW ARCHIVE REPLICATION",
     "observability.show_archive_replication",
     "SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION",
     ""},
    {"SBSQL-790219E347FD",
     "show_agents_extended",
     "grammar_production",
     "SBSQL-SURFACE-0B15D645EA41",
     "SHOW AGENTS EXTENDED",
     "observability.show_agents_extended",
     "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED",
     ""},
    {"SBSQL-81F70EB4CEF8",
     "agent_state",
     "grammar_production",
     "SBSQL-SURFACE-D9172D7111C9",
     "SHOW AGENTS EXTENDED",
     "observability.show_agents_extended",
     "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED",
     ""},
    {"SBSQL-97CD29B9F122",
     "show_filespace_extended",
     "grammar_production",
     "SBSQL-SURFACE-5589CBC5EBDC",
     "SHOW FILESPACE EXTENDED",
     "observability.show_filespace_extended",
     "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED",
     ""},
    {"SBSQL-6482A2299513",
     "filespace_show_target",
     "grammar_production",
     "SBSQL-SURFACE-B3D7869F2CB4",
     "SHOW FILESPACE EXTENDED",
     "observability.show_filespace_extended",
     "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED",
     ""},
    {"SBSQL-789915B429AF",
     "show_decision_service_stmt",
     "grammar_production",
     "SBSQL-SURFACE-E8B810A4D666",
     "SHOW DECISION SERVICE",
     "observability.show_decision_service",
     "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE",
     ""},
    {"SBSQL-D01384EE782E",
     "decision_show_target",
     "grammar_production",
     "SBSQL-SURFACE-64389E4D6B89",
     "SHOW DECISION SERVICE",
     "observability.show_decision_service",
     "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE",
     ""},
    {"SBSQL-DF68DFFA5C1E",
     "show_acceleration",
     "grammar_production",
     "SBSQL-SURFACE-B1551EE42FB6",
     "SHOW ACCELERATION",
     "observability.show_acceleration",
     "SBLR_OBSERVABILITY_SHOW_ACCELERATION",
     ""},
    {"SBSQL-8E570F4EEEF3",
     "accel_show_target",
     "grammar_production",
     "SBSQL-SURFACE-AA51AF83BFF3",
     "SHOW ACCELERATION",
     "observability.show_acceleration",
     "SBLR_OBSERVABILITY_SHOW_ACCELERATION",
     ""},
    {"SBSQL-41F75C7C86A7",
     "show_acceleration_extended",
     "grammar_production",
     "SBSQL-SURFACE-DC4D4612AD42",
     "SHOW ACCELERATION EXTENDED",
     "observability.show_acceleration_extended",
     "SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED",
     ""},
    {"SBSQL-9E28B50323FA",
     "describe",
     "canonical_surface",
     "SBSQL-SURFACE-D8267C0D3A55",
     "DESCRIBE replay_target",
     "observability.show_catalog",
     "SBLR_OBSERVABILITY_SHOW_CATALOG",
     "019f0000-0000-7000-8000-000000000901"},
    {"SBSQL-DF65788531DB",
     "describe_stmt",
     "grammar_production",
     "SBSQL-SURFACE-5F8E79EE2D90",
     "DESCRIBE replay_target",
     "observability.show_catalog",
     "SBLR_OBSERVABILITY_SHOW_CATALOG",
     "019f0000-0000-7000-8000-000000000901"},
    {"SBSQL-D29E98768D48",
     "describe_target",
     "grammar_production",
     "SBSQL-SURFACE-75A920E1AC97",
     "DESCRIBE replay_target",
     "observability.show_catalog",
     "SBLR_OBSERVABILITY_SHOW_CATALOG",
     "019f0000-0000-7000-8000-000000000901"},
    {"SBSQL-C7750F0B80EB",
     "show_plan_extended",
     "grammar_production",
     "SBSQL-SURFACE-848C9893FA08",
     "SHOW PLAN EXTENDED",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
    {"SBSQL-01FAACE4DEFE",
     "explain",
     "canonical_surface",
     "SBSQL-SURFACE-FBFE05527E62",
     "EXPLAIN SELECT 1",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
    {"SBSQL-AB95F9C8DC77",
     "explain_stmt",
     "grammar_production",
     "SBSQL-SURFACE-1CCD011F473F",
     "EXPLAIN SELECT 1",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
    {"SBSQL-BB31ED9589C7",
     "explainable",
     "grammar_production",
     "SBSQL-SURFACE-F6EEFB74B3C5",
     "EXPLAIN SELECT 1",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
    {"SBSQL-EFE86345F99D",
     "explain_kind",
     "grammar_production",
     "SBSQL-SURFACE-7C2D3E579F7C",
     "EXPLAIN PLAN SELECT 1",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
    {"SBSQL-B3771D9D39C1",
     "explain_option",
     "grammar_production",
     "SBSQL-SURFACE-288089CED337",
     "EXPLAIN (VERBOSE) SELECT 1",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
    {"SBSQL-627723269A64",
     "explain_option_keyword",
     "grammar_production",
     "SBSQL-SURFACE-52FCDB09A23F",
     "EXPLAIN (VERBOSE) SELECT 1",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
    {"SBSQL-1BA82B56911E",
     "explain_options",
     "grammar_production",
     "SBSQL-SURFACE-64933186778D",
     "EXPLAIN (VERBOSE) SELECT 1",
     "observability.explain_operation",
     "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
     ""},
}};

std::string EvidenceMessage(const ObservabilityRowEvidence& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string_view ExpectedServerAdmissionFamily(
    const ObservabilityRowEvidence& row,
    std::string_view payload) {
  if (payload.find("\"public_sbsql_exact_command\":true") ==
          std::string_view::npos &&
      payload.find("\"public_sbsql_exact_command\": true") ==
          std::string_view::npos) {
    return "sblr.observability.inspect.v3";
  }
  if (row.operation_id == "observability.show_metrics") {
    return "sblr.metrics.read.v3";
  }
  if (row.operation_id == "observability.show_transactions") {
    return "sblr.mga.report.v3";
  }
  if (row.operation_id == "observability.show_catalog") {
    return "sblr.catalog.introspect.v3";
  }
  if (row.operation_id == "observability.show_diagnostics" ||
      row.operation_id == "observability.show_diagnostics_extended") {
    return "sblr.diagnostic.control.v3";
  }
  if (row.operation_id == "observability.show_filespace_extended") {
    return "sblr.filespace.management.v3";
  }
  if (row.operation_id == "observability.show_archive_replication") {
    return "sblr.replication.consumer.v3";
  }
  if (row.operation_id == "observability.explain_operation") {
    return "sblr.optimizer.plan.v3";
  }
  return "sblr.management.report.v3";
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool RequiresRouteSurfacePayload(std::string_view surface_id) {
  return surface_id == "SBSQL-6482A2299513" ||
         surface_id == "SBSQL-D01384EE782E" ||
         surface_id == "SBSQL-8E570F4EEEF3";
}

bool ApiResultHasField(const api::EngineApiResult& result,
                       std::string_view name,
                       std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == name && field.second.encoded_value == value) return true;
    }
  }
  return false;
}

bool ApiResultHasEvidence(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000000701";
  session.connection_uuid = "019f0000-0000-7000-8000-000000000702";
  session.database_uuid = "019f0000-0000-7000-8000-000000000703";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000000704";
  config.bundle_contract_id = "sbp_sbsql@observability-route-test";
  config.build_id = "sbsql-observability-route-test";
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
                              const std::vector<std::string>& resolved_object_uuids = {}) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            resolved_object_uuids);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const ObservabilityRowEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == row.surface_kind,
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "observability",
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.observability.inspect.v3",
          EvidenceMessage(row, "registry", "SBLR operation family mismatch"));
  Require(registry_row->parser_handler_key == "parser.statement_family.observability",
          EvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key ==
              "lowering.sblr_family.sblr_observability_inspect_v3",
          EvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key ==
              "server.admission.sblr_observability_inspect_v3",
          EvidenceMessage(row, "server_admission", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_observability_inspect_v3",
          EvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireExactLowering(const ObservabilityRowEvidence& row) {
  std::vector<std::string> resolved_object_uuids;
  if (!row.resolved_object_uuid.empty()) {
    resolved_object_uuids.emplace_back(row.resolved_object_uuid);
  }
  const auto artifacts = RunPipeline(row.sql, resolved_object_uuids);
  Require(artifacts.bound.bound,
          EvidenceMessage(row, "parser_bind_lower", "observability statement did not bind"));
  if (!row.resolved_object_uuid.empty()) {
    Require(artifacts.bound.requires_name_resolution,
            EvidenceMessage(row,
                            "parser_bind_lower",
                            "DESCRIBE route did not require name resolution"));
    Require(HasValue(artifacts.envelope.resolved_object_uuids, row.resolved_object_uuid),
            EvidenceMessage(row,
                            "parser_bind_lower",
                            "DESCRIBE route missing resolved object UUID"));
    Require(Contains(artifacts.envelope.payload, row.resolved_object_uuid),
            EvidenceMessage(row,
                            "parser_bind_lower",
                            "DESCRIBE payload missing resolved object UUID evidence"));
  }
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower",
                          "observability SBLR verifier rejected exact route"));
  Require(artifacts.envelope.operation_family == "sblr.observability.inspect.v3",
          EvidenceMessage(row, "parser_bind_lower", "observability operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.observability.inspect.v3",
          EvidenceMessage(row, "parser_bind_lower", "observability SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "observability operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          EvidenceMessage(row, "parser_bind_lower", "observability opcode mismatch"));
  if (RequiresRouteSurfacePayload(row.surface_id)) {
    Require(Contains(artifacts.envelope.payload, "\"observability_envelope_kind\":\"inspect\""),
            EvidenceMessage(row,
                            "parser_bind_lower",
                            "observability envelope kind missing from payload"));
    Require(Contains(artifacts.envelope.payload, "\"observability_surface_ids\""),
            EvidenceMessage(row,
                            "parser_bind_lower",
                            "observability surface ids missing from payload"));
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            EvidenceMessage(row,
                            "parser_bind_lower",
                            "target surface id missing from lowered observability payload"));
  }
  Require(HasValue(artifacts.envelope.required_rights, "right.observe"),
          EvidenceMessage(row, "parser_bind_lower", "observability right missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.observability_api_required"),
          EvidenceMessage(row, "parser_bind_lower",
                          "engine observability authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "parser no-SQL-execution authority step missing"));
  Require(!artifacts.bound.requires_transaction_authority,
          EvidenceMessage(row, "no_transaction_finality",
                          "observability route requested transaction authority"));
  Require(!HasValue(artifacts.envelope.required_authority_steps,
                    "authority.engine.mga_transaction_control_required"),
          EvidenceMessage(row, "no_transaction_finality",
                          "observability route requested MGA transaction control"));
  Require(!artifacts.envelope.parser_executes_sql,
          EvidenceMessage(row, "no_sql_engine_execution",
                          "observability lowering allowed parser SQL execution"));
  Require(!artifacts.envelope.real_file_effects,
          EvidenceMessage(row, "no_reference_execution",
                          "observability lowering allowed reference/file effects"));
  Require(!Contains(artifacts.envelope.payload, row.sql),
          EvidenceMessage(row, "no_sql_text_authority",
                          "observability envelope embedded source SQL text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(row, "no_sql_text_authority",
                          "observability envelope embedded source_text"));
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "observability envelope embedded sql_text"));
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          EvidenceMessage(row, "no_sql_engine_execution",
                          "observability payload did not prove parser_executes_sql=false"));
  Require(Contains(artifacts.envelope.payload, "\"real_file_effects\":false"),
          EvidenceMessage(row, "no_reference_execution",
                          "observability payload did not prove real_file_effects=false"));
  Require(!Contains(artifacts.envelope.payload, "reference"),
          EvidenceMessage(row, "no_reference_execution",
                          "observability payload carried reference authority"));
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "\"wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "\"recovery_authority\":true"),
          EvidenceMessage(row, "no_wal_recovery_authority",
                          "observability payload carried WAL/recovery authority"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          EvidenceMessage(row, "server_admission",
                          "server admission rejected exact observability route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "server_admission",
                          "server admission did not require engine public ABI dispatch"));
  Require(admission.operation_id == row.operation_id,
          EvidenceMessage(row, "server_admission", "server admission operation id mismatch"));
  const std::string_view expected_family =
      ExpectedServerAdmissionFamily(row, artifacts.envelope.payload);
  if (admission.operation_family != expected_family) {
    std::cerr << EvidenceMessage(row,
                                 "server_admission",
                                 "expected family " +
                                     std::string(expected_family) +
                                     " actual " + admission.operation_family)
              << '\n';
  }
  Require(admission.operation_family == expected_family,
          EvidenceMessage(row, "server_admission",
                          "server admission operation family mismatch"));
}

api::EngineRequestContext EngineContext(const ObservabilityRowEvidence& row) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.request_id = "sbsql-observability-exact-route";
  context.security_context_present = true;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:OBS_METRICS_READ_ALL");
  context.trace_tags.push_back(std::string("sbsql_surface_id:") + std::string(row.surface_id));
  context.database_path = "/tmp/sbsql_observability_exact_route_conformance.sbdb";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000000801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000000802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000000803";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000000804";
  context.cluster_uuid.canonical = "019f0000-0000-7000-8000-000000000807";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000000805";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000000806";
  context.current_sqlstate = "01000";
  context.catalog_generation_id = 7;
  context.security_epoch = 11;
  context.resource_epoch = 13;
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const ObservabilityRowEvidence& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.observability.exact_route.") +
                                             std::string(row.surface_id));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireEngineDispatch(const ObservabilityRowEvidence& row) {
  const auto engine_envelope = EngineEnvelope(row);
  Require(!engine_envelope.contains_sql_text,
          EvidenceMessage(row, "no_sql_text_authority",
                          "engine SBLR envelope carried SQL text authority"));
  Require(!engine_envelope.requires_transaction_context,
          EvidenceMessage(row, "no_transaction_finality",
                          "engine dispatch envelope requested transaction context"));
  Require(!engine_envelope.requires_cluster_authority,
          EvidenceMessage(row, "cluster_fail_closed",
                          "engine dispatch envelope requested cluster authority"));
  api::EngineApiRequest api_request;
  if (!row.resolved_object_uuid.empty()) {
    api_request.target_object.uuid.canonical = std::string(row.resolved_object_uuid);
    api_request.target_object.object_kind = "table";
  }
  const sblr::SblrDispatchRequest request{
      EngineContext(row),
      engine_envelope,
      api_request};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(result.envelope_validated,
          EvidenceMessage(row, "engine_dispatch", "engine SBLR envelope did not validate"));
  Require(result.accepted,
          EvidenceMessage(row, "engine_dispatch",
                          "engine SBLR dispatch did not accept observability operation"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "engine_dispatch",
                          "engine SBLR dispatch did not route to an internal API"));
  Require(result.api_result.operation_id == row.operation_id,
          EvidenceMessage(row, "engine_dispatch",
                          "engine SBLR dispatch returned wrong operation id"));
  if (row.operation_id == "observability.show_version") {
    Require(ApiResultHasField(result.api_result, "product", "ScratchBird"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowVersion did not return product evidence"));
  }
  if (row.operation_id == "observability.show_database") {
    Require(ApiResultHasField(result.api_result,
                              "database_uuid",
                              "019f0000-0000-7000-8000-000000000801"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowDatabase did not return database UUID evidence"));
  }
  if (row.operation_id == "observability.show_catalog") {
    Require(ApiResultHasEvidence(result.api_result, "catalog_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowCatalog did not return catalog row evidence"));
  }
  if (row.operation_id == "observability.show_system") {
    Require(ApiResultHasField(result.api_result, "trust_mode", "server_isolated"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowSystem did not return trust mode evidence"));
  }
  if (row.operation_id == "observability.show_sessions") {
    Require(ApiResultHasField(result.api_result,
                              "session_uuid",
                              "019f0000-0000-7000-8000-000000000802"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowSessions did not return session UUID evidence"));
  }
  if (row.operation_id == "observability.show_locks") {
    Require(ApiResultHasField(result.api_result, "lock_count", "0"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowLocks did not return local lock evidence"));
  }
  if (row.operation_id == "observability.show_statements") {
    Require(ApiResultHasField(result.api_result, "statement_count", "0"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowStatements did not return local statement evidence"));
  }
  if (row.operation_id == "observability.show_jobs") {
    Require(ApiResultHasEvidence(result.api_result, "jobs_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowJobs did not return jobs row evidence"));
    Require(ApiResultHasField(result.api_result, "scheduler_scope", "local_node"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowJobs did not return local scheduler evidence"));
  }
  if (row.operation_id == "observability.show_management") {
    Require(ApiResultHasEvidence(result.api_result, "management_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowManagement did not return management row evidence"));
    Require(ApiResultHasField(result.api_result, "catalog_generation_id", "7"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowManagement did not return catalog generation evidence"));
  }
  if (row.operation_id == "observability.show_diagnostics") {
    Require(ApiResultHasEvidence(result.api_result, "diagnostic_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowDiagnostics did not return diagnostic row evidence"));
    Require(ApiResultHasField(result.api_result, "current_sqlstate", "01000"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowDiagnostics did not return current SQLSTATE evidence"));
  }
  if (row.operation_id == "observability.show_diagnostics_extended") {
    Require(ApiResultHasEvidence(result.api_result, "diagnostic_extended_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowDiagnosticsExtended did not return extended row evidence"));
    Require(ApiResultHasField(result.api_result, "trace_tag_count", "3"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowDiagnosticsExtended did not return trace evidence"));
  }
  if (row.operation_id == "observability.show_archive_replication") {
    Require(ApiResultHasEvidence(result.api_result, "archive_replication_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowArchiveReplication did not return archive row evidence"));
    Require(ApiResultHasField(result.api_result, "archive_mode", "local_mga_inventory"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowArchiveReplication did not preserve MGA archive authority"));
  }
  if (row.operation_id == "observability.show_agents_extended") {
    Require(ApiResultHasEvidence(result.api_result, "agent_registry"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowAgentsExtended did not return agent registry evidence"));
    Require(!result.api_result.result_shape.rows.empty(),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowAgentsExtended did not return agent rows"));
  }
  if (row.operation_id == "observability.show_filespace_extended") {
    Require(ApiResultHasEvidence(result.api_result, "filespace_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowFilespaceExtended did not return filespace evidence"));
    Require(ApiResultHasField(result.api_result,
                              "mga_finality_authority",
                              "local_transaction_inventory"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowFilespaceExtended did not preserve MGA finality authority"));
  }
  if (row.operation_id == "observability.show_decision_service") {
    Require(ApiResultHasEvidence(result.api_result, "decision_service_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowDecisionService did not return decision service evidence"));
    Require(ApiResultHasField(result.api_result,
                              "provider_boundary",
                              "compile_gated_cluster_provider"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowDecisionService did not preserve cluster provider boundary"));
  }
  if (row.operation_id == "observability.show_acceleration") {
    Require(ApiResultHasEvidence(result.api_result, "acceleration_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowAcceleration did not return acceleration row evidence"));
    Require(ApiResultHasField(result.api_result, "runtime_mode", "interpreted_sblr"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowAcceleration did not return runtime evidence"));
  }
  if (row.operation_id == "observability.show_acceleration_extended") {
    Require(ApiResultHasEvidence(result.api_result, "acceleration_extended_rows"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowAccelerationExtended did not return acceleration row evidence"));
    Require(ApiResultHasField(result.api_result, "gpu_queue_count", "0"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowAccelerationExtended did not return GPU queue evidence"));
  }
  if (row.operation_id == "observability.show_metrics") {
    Require(!result.api_result.result_shape.rows.empty(),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowMetrics did not return bounded metric rows"));
    Require(ApiResultHasField(result.api_result, "metric", "sb_page_cache_resident_pages"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowMetrics did not return local metric evidence"));
    Require(ApiResultHasField(result.api_result, "namespace", "sys.metrics.storage.pages.cache"),
            EvidenceMessage(row, "engine_dispatch",
                            "EngineShowMetrics did not return local sys.metrics evidence"));
  }
}

void RequireClusterObservabilityRefusal() {
  for (const auto& row : kObservabilityRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr,
            EvidenceMessage(row, "cluster_fail_closed", "missing generated registry row"));
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            EvidenceMessage(row, "cluster_fail_closed", "row is not noncluster scoped"));
  }
  constexpr std::array<std::string_view, 17> kClusterFixtures{{
      "SHOW METRICS CLUSTER",
      "SHOW VERSION CLUSTER",
      "SHOW DATABASE CLUSTER",
      "SHOW SESSIONS CLUSTER",
      "SHOW OBJECTS CLUSTER",
      "SHOW JOBS CLUSTER",
      "SHOW MANAGEMENT CLUSTER",
      "SHOW DIAGNOSTICS CLUSTER",
      "SHOW DIAGNOSTICS EXTENDED CLUSTER",
      "SHOW ARCHIVE REPLICATION CLUSTER",
      "SHOW AGENTS EXTENDED CLUSTER",
      "SHOW FILESPACE EXTENDED CLUSTER",
      "SHOW DECISION SERVICE CLUSTER",
      "SHOW ACCELERATION CLUSTER",
      "SHOW ACCELERATION EXTENDED CLUSTER",
      "SHOW PLAN CLUSTER",
      "EXPLAIN CLUSTER SELECT 1",
  }};
  for (const auto fixture : kClusterFixtures) {
    const auto artifacts = RunPipeline(fixture);
    Require(!artifacts.bound.bound || artifacts.envelope.messages.has_errors() ||
                artifacts.verifier.messages.has_errors(),
            std::string("SBSQL-B7EF40AE00EB SBSQL-2A0B29C713C7 "
                        "SBSQL-9481A549CA44 SBSQL-01FAACE4DEFE "
                        "SBSQL-AB95F9C8DC77 SBSQL-564D768C8ADF "
                        "SBSQL-BB31ED9589C7 "
                        "cluster_fail_closed: ") +
                std::string(fixture) +
                " cluster observability target did not fail closed");
  }
}

}  // namespace

int main() {
  for (const auto& row : kObservabilityRows) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row);
    RequireEngineDispatch(row);
  }
  RequireClusterObservabilityRefusal();
  std::cout << "sbsql_observability_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
