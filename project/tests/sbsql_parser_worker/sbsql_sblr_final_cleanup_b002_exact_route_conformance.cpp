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
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "security/security_principal_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef SCRATCHBIRD_PROJECT_SOURCE_DIR
#define SCRATCHBIRD_PROJECT_SOURCE_DIR "."
#endif

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

struct B002Row {
  std::string_view audit_id;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view family;
  std::string_view result_shape;
  std::string_view target_ref;
  std::string_view target_ref_kind;
};

constexpr std::array<B002Row, 35> kRows{{
    {"AUDIT-0359", "SHOW VERSION", "observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION", "sblr.observability.inspect.v3", "rs.show.version.v1", "", ""},
    {"AUDIT-0360", "SHOW SYSTEM", "observability.show_system", "SBLR_OBSERVABILITY_SHOW_SYSTEM", "sblr.observability.inspect.v3", "rs.show.system.v1", "", ""},
    {"AUDIT-0361", "SHOW SEARCH PATH", "op.show.search_path", "SBLR_OP_SHOW_SEARCH_PATH", "sblr.observability.inspect.v3", "rs.show.path.v1", "", ""},
    {"AUDIT-0362", "SHOW SCHEMA PATH", "op.show.schema_path", "SBLR_OP_SHOW_SCHEMA_PATH", "sblr.observability.inspect.v3", "rs.show.path.v1", "", ""},
    {"AUDIT-0363", "SHOW TRANSACTION", "op.show.transaction", "SBLR_OP_SHOW_TRANSACTION", "sblr.observability.inspect.v3", "rs.show.transaction.v1", "", ""},
    {"AUDIT-0364", "SHOW TRANSACTION ISOLATION", "op.show.transaction_isolation", "SBLR_OP_SHOW_TRANSACTION_ISOLATION", "sblr.observability.inspect.v3", "rs.show.transaction.v1", "", ""},
    {"AUDIT-0365", "SHOW STATEMENT CACHE", "op.show.statement_cache", "SBLR_OP_SHOW_STATEMENT_CACHE", "sblr.observability.inspect.v3", "rs.show.statement_cache.v1", "", ""},
    {"AUDIT-0366", "SHOW SESSIONS", "observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "sblr.observability.inspect.v3", "rs.show.sessions.v1", "", ""},
    {"AUDIT-0367", "SHOW TRANSACTIONS", "observability.show_transactions", "SBLR_OBSERVABILITY_SHOW_TRANSACTIONS", "sblr.observability.inspect.v3", "rs.show.transactions.v1", "", ""},
    {"AUDIT-0368", "SHOW LOCKS", "observability.show_locks", "SBLR_OBSERVABILITY_SHOW_LOCKS", "sblr.observability.inspect.v3", "rs.show.locks.v1", "", ""},
    {"AUDIT-0369", "SHOW STATEMENTS", "observability.show_statements", "SBLR_OBSERVABILITY_SHOW_STATEMENTS", "sblr.observability.inspect.v3", "rs.show.statements.v1", "", ""},
    {"AUDIT-0370", "SHOW JOBS", "observability.show_jobs", "SBLR_OBSERVABILITY_SHOW_JOBS", "sblr.observability.inspect.v3", "rs.show.jobs.v1", "", ""},
    {"AUDIT-0371", "SHOW METRICS", "observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS", "sblr.observability.inspect.v3", "rs.show.metrics.v1", "", ""},
    {"AUDIT-0372", "SHOW METRICS FAMILY runtime", "op.show.metrics_family", "SBLR_OP_SHOW_METRICS_FAMILY", "sblr.observability.inspect.v3", "rs.show.metrics.v1", "runtime", "metrics_family"},
    {"AUDIT-0373", "SHOW PERFORMANCE", "op.show.performance", "SBLR_OP_SHOW_PERFORMANCE", "sblr.observability.inspect.v3", "rs.show.performance.v1", "", ""},
    {"AUDIT-0374", "SHOW WAIT EVENTS", "op.show.wait_events", "SBLR_OP_SHOW_WAIT_EVENTS", "sblr.observability.inspect.v3", "rs.show.wait_events.v1", "", ""},
    {"AUDIT-0375", "SHOW QUERY STORE", "op.show.query_store", "SBLR_OP_SHOW_QUERY_STORE", "sblr.observability.inspect.v3", "rs.show.performance.v1", "", ""},
    {"AUDIT-0376", "SHOW NATIVE COMPILE", "op.show.native_compile", "SBLR_OP_SHOW_NATIVE_COMPILE", "sblr.acceleration.llvm.v3", "rs.show.native_compile.v1", "", ""},
    {"AUDIT-0377", "SHOW NATIVE COMPILE CACHE", "op.show.native_compile_cache", "SBLR_OP_SHOW_NATIVE_COMPILE_CACHE", "sblr.acceleration.llvm.v3", "rs.show.native_compile.v1", "", ""},
    {"AUDIT-0379", "SHOW MANAGEMENT MANAGER", "op.show.management.manager", "SBLR_OP_SHOW_MANAGEMENT_MANAGER", "sblr.management.runtime_operation.v3", "rs.management.manager.v1", "", ""},
    {"AUDIT-0380", "SHOW MANAGEMENT SERVERS", "op.show.management.servers", "SBLR_OP_SHOW_MANAGEMENT_SERVERS", "sblr.management.runtime_operation.v3", "rs.management.server.v1", "", ""},
    {"AUDIT-0381", "SHOW MANAGEMENT LISTENERS", "op.show.management.listeners", "SBLR_OP_SHOW_MANAGEMENT_LISTENERS", "sblr.management.runtime_operation.v3", "rs.management.listener.v1", "", ""},
    {"AUDIT-0382", "SHOW MANAGEMENT PARSER POOL", "op.show.management.parser_pool", "SBLR_OP_SHOW_MANAGEMENT_PARSER_POOL", "sblr.management.runtime_operation.v3", "rs.management.parser_pool.v1", "", ""},
    {"AUDIT-0383", "SHOW MANAGEMENT INSTRUCTIONS", "op.show.management.instructions", "SBLR_OP_SHOW_MANAGEMENT_INSTRUCTIONS", "sblr.management.runtime_operation.v3", "rs.management.instruction.v1", "", ""},
    {"AUDIT-0384", "SHOW MANAGEMENT READINESS", "op.show.management.readiness", "SBLR_OP_SHOW_MANAGEMENT_READINESS", "sblr.management.runtime_operation.v3", "rs.management.readiness.v1", "", ""},
    {"AUDIT-0385", "SHOW MANAGEMENT SUPPORT BUNDLE SAFETY", "op.show.management.support_bundle_safety", "SBLR_OP_SHOW_MANAGEMENT_SUPPORT_BUNDLE_SAFETY", "sblr.management.runtime_operation.v3", "rs.management.readiness.v1", "", ""},
    {"AUDIT-0386", "SHOW MANAGEMENT SUPPORT BUNDLES", "op.show.management.support_bundles", "SBLR_OP_SHOW_MANAGEMENT_SUPPORT_BUNDLES", "sblr.management.runtime_operation.v3", "rs.management.instruction.v1", "", ""},
    {"AUDIT-0387", "SHOW USERS", "op.show.users", "SBLR_OP_SHOW_USERS", "sblr.security.mutation_or_inspect.v3", "rs.security.principal.v1", "", ""},
    {"AUDIT-0388", "SHOW ROLES", "op.show.roles", "SBLR_OP_SHOW_ROLES", "sblr.security.mutation_or_inspect.v3", "rs.security.principal.v1", "", ""},
    {"AUDIT-0389", "SHOW OBJECT VISIBILITY FOR customer_table", "op.show.object_visibility", "SBLR_OP_SHOW_OBJECT_VISIBILITY", "sblr.security.mutation_or_inspect.v3", "rs.security.grant.v1", "customer_table", "object"},
    {"AUDIT-0390", "SHOW POLICIES", "op.show.policies", "SBLR_OP_SHOW_POLICIES", "sblr.security.mutation_or_inspect.v3", "rs.security.policy.v1", "", ""},
    {"AUDIT-0391", "SHOW MASKS", "op.show.masks", "SBLR_OP_SHOW_MASKS", "sblr.security.mutation_or_inspect.v3", "rs.security.policy.v1", "", ""},
    {"AUDIT-0392", "SHOW RLS", "op.show.rls", "SBLR_OP_SHOW_RLS", "sblr.security.mutation_or_inspect.v3", "rs.security.policy.v1", "", ""},
    {"AUDIT-0393", "SHOW SECURITY PROFILES", "op.show.security_profiles", "SBLR_OP_SHOW_SECURITY_PROFILES", "sblr.security.mutation_or_inspect.v3", "rs.security.policy.v1", "", ""},
    {"AUDIT-0394", "SHOW SECURITY EVENTS", "op.show.security_events", "SBLR_OP_SHOW_SECURITY_EVENTS", "sblr.security.mutation_or_inspect.v3", "rs.security.policy.v1", "", ""},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasDiagnosticCode(const MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool ApiResultHasEvidence(const api::EngineApiResult& result,
                          std::string_view kind,
                          std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string EvidenceMessage(const B002Row& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.audit_id);
  rendered += ' ';
  rendered += row.operation_id;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string_view ExpectedEngineApiFunction(const B002Row& row) {
  if (StartsWith(row.operation_id, "op.show.management.")) {
    return "EngineInspectManagementRuntime";
  }
  if (row.operation_id == "op.show.native_compile" ||
      row.operation_id == "op.show.native_compile_cache") {
    return "EngineInspectNativeCompile";
  }
  if (row.family == "sblr.security.mutation_or_inspect.v3") {
    return "EngineSecurityInspectOperation";
  }
  return "EngineInspectShowOperation";
}

std::string_view ExpectedAuthorityStep(const B002Row& row) {
  if (ExpectedEngineApiFunction(row) == "EngineSecurityInspectOperation") {
    return "authority.engine.security_inspection_api_required";
  }
  if (ExpectedEngineApiFunction(row) == "EngineInspectManagementRuntime") {
    return "authority.engine.management_runtime_api_required";
  }
  if (ExpectedEngineApiFunction(row) == "EngineInspectShowOperation") {
    return "authority.engine.observability_api_required";
  }
  return "authority.engine.acceleration_api_required";
}

std::string_view ExpectedAdmissionFamily(const B002Row& row) {
  if (row.operation_id == "op.show.native_compile" ||
      row.operation_id == "op.show.native_compile_cache") {
    return "sblr.acceleration.llvm.v3";
  }
  if (StartsWith(row.operation_id, "op.show.management.")) {
    return "sblr.management.report.v3";
  }
  if (row.operation_id == "op.show.policies" ||
      row.operation_id == "op.show.rls" ||
      row.operation_id == "op.show.masks") {
    return "sblr.policy.operation.v3";
  }
  if (ExpectedEngineApiFunction(row) == "EngineSecurityInspectOperation") {
    return "sblr.catalog.introspect.v3";
  }
  if (row.operation_id == "op.show.metrics" ||
      row.operation_id == "op.show.metrics_family" ||
      row.operation_id == "op.show.performance") {
    return "sblr.metrics.read.v3";
  }
  if (row.operation_id == "observability.show_metrics") {
    return "sblr.metrics.read.v3";
  }
  if (row.operation_id == "observability.show_transactions") {
    return "sblr.mga.report.v3";
  }
  if (StartsWith(row.operation_id, "observability.show_") ||
      StartsWith(row.operation_id, "op.show.")) {
    return "sblr.management.report.v3";
  }
  return row.family;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-00000000d701";
  session.connection_uuid = "019f0000-0000-7000-8000-00000000d702";
  session.database_uuid = "019f0000-0000-7000-8000-00000000d703";
  session.catalog_epoch = 71;
  session.security_policy_epoch = 73;
  session.descriptor_epoch = 79;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-00000000d704";
  config.bundle_contract_id = "sbp_sbsql@sbsql-sblr-final-cleanup-b002";
  config.build_id = "sbsql-sblr-final-cleanup-b002";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

api::EngineRequestContext EngineContext(bool security_context_present = true) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sblr-final-cleanup-b002";
  context.security_context_present = security_context_present;
  context.database_path = "/tmp/sbsql_sblr_final_cleanup_b002.sbdb";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-00000000d801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-00000000d802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-00000000d803";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-00000000d804";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-00000000d805";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-00000000d806";
  context.catalog_generation_id = 71;
  context.security_epoch = 73;
  context.resource_epoch = 79;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:OBS_METRICS_READ_ALL");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const B002Row& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.b002.") +
                                             std::string(row.audit_id));
  envelope.result_shape = row.result_shape;
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

api::EngineApiRequest ApiRequestForRow(const B002Row& row) {
  api::EngineApiRequest request;
  request.option_envelopes.push_back(std::string("result_shape_contract:") +
                                     std::string(row.result_shape));
  if (!row.target_ref.empty()) {
    request.option_envelopes.push_back(std::string("target_ref:") +
                                       std::string(row.target_ref));
    request.option_envelopes.push_back(std::string("target_ref_kind:") +
                                       std::string(row.target_ref_kind));
  }
  return request;
}

void RequireExactLowering(const B002Row& row) {
  const auto artifacts = RunPipeline(row.sql);
  Require(artifacts.bound.bound, EvidenceMessage(row, "parser_bind_lower", "row did not bind"));
  Require(!artifacts.bound.requires_name_resolution,
          EvidenceMessage(row, "parser_bind_lower", "row required parser-side name resolution"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower", "SBLR verifier rejected row"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          EvidenceMessage(row, "parser_bind_lower", "opcode mismatch"));
  Require(artifacts.envelope.operation_family == row.family,
          EvidenceMessage(row, "parser_bind_lower", "operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == row.family,
          EvidenceMessage(row, "parser_bind_lower", "SBLR operation key mismatch"));
  Require(artifacts.envelope.result_shape_key == row.result_shape,
          EvidenceMessage(row, "parser_bind_lower", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == ExpectedEngineApiFunction(row),
          EvidenceMessage(row, "parser_bind_lower", "engine API function missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   ExpectedAuthorityStep(row)),
          EvidenceMessage(row, "parser_bind_lower", "engine API authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "parser_bind_lower", "no-SQL authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          EvidenceMessage(row, "parser_bind_lower", "cluster boundary authority missing"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          EvidenceMessage(row, "parser_bind_lower", "public exact command payload flag missing"));
  Require(Contains(artifacts.envelope.payload, "\"engine_api_function\":\"") &&
              Contains(artifacts.envelope.payload, ExpectedEngineApiFunction(row)),
          EvidenceMessage(row, "parser_bind_lower", "domain API function missing from payload"));
  Require(!Contains(artifacts.envelope.payload, row.audit_id),
          EvidenceMessage(row, "parser_bind_lower", "audit id leaked into production payload"));
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          EvidenceMessage(row, "parser_bind_lower", "parser execution evidence missing"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(row, "parser_bind_lower", "source text key was embedded"));
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":"),
          EvidenceMessage(row, "parser_bind_lower", "SQL text key was embedded"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          EvidenceMessage(row, "server_admission", "server admission rejected row"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "server_admission", "public ABI dispatch not required"));
  Require(admission.operation_id == row.operation_id,
          EvidenceMessage(row, "server_admission", "operation id mismatch"));
  Require(admission.operation_family == ExpectedAdmissionFamily(row),
          EvidenceMessage(row, "server_admission", "operation family mismatch"));
}

void RequireSblrRegistryAndDispatch(const B002Row& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, EvidenceMessage(row, "sblr_registry", "operation missing"));
  Require(entry->opcode == row.opcode, EvidenceMessage(row, "sblr_registry", "opcode mismatch"));
  const auto envelope = EngineEnvelope(row);
  const auto registry_validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(registry_validation.ok,
          EvidenceMessage(row, "sblr_registry", "registry validation rejected row"));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, ApiRequestForRow(row)};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          EvidenceMessage(row, "engine_dispatch", "engine envelope was not valid"));
  Require(result.accepted,
          EvidenceMessage(row, "engine_dispatch", "dispatch did not accept row"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "engine_dispatch", "dispatch did not route to API"));
  Require(result.api_result.ok,
          EvidenceMessage(row, "engine_dispatch", "internal API returned failure"));
  Require(result.api_result.operation_id == row.operation_id,
          EvidenceMessage(row, "engine_dispatch", "API operation id mismatch"));
  Require(result.api_result.result_shape.result_kind == row.result_shape,
          EvidenceMessage(row, "engine_dispatch", "API result shape mismatch"));
  Require(ApiResultHasEvidence(result.api_result,
                               "public_sbsql_operation",
                               row.operation_id),
          EvidenceMessage(row, "engine_dispatch", "public operation API evidence missing"));
  Require(ApiResultHasEvidence(result.api_result,
                               "engine_api_function",
                               ExpectedEngineApiFunction(row)),
          EvidenceMessage(row, "engine_dispatch", "domain API evidence missing"));
  Require(ApiResultHasEvidence(result.api_result,
                               "result_shape_contract",
                               row.result_shape),
          EvidenceMessage(row, "engine_dispatch", "result shape evidence missing"));
}

void RequireInvalidSyntaxDiagnostics() {
  const auto artifacts = RunPipeline("SHOW METRICS FAMILY");
  Require(artifacts.envelope.messages.has_errors(),
          "invalid metrics family shape did not produce a message-vector error");
  Require(HasDiagnosticCode(artifacts.envelope.messages,
                            "SBSQL.EXACT_COMMAND.INVALID_SHAPE"),
          "invalid metrics family shape did not produce the expected diagnostic code");
  const std::string rendered = RenderMessageVectorSet(artifacts.envelope.messages);
  Require(Contains(rendered, "SBSQL.EXACT_COMMAND.INVALID_SHAPE"),
          "rendered exact command message vector omitted diagnostic code");
  Require(Contains(rendered, "op.show.metrics_family") &&
              Contains(rendered, "show.metrics_family"),
          "rendered exact command message vector omitted operation or surface fields");
}

void RequireSecurityRefusalRedaction() {
  api::EngineSecurityInspectOperationRequest request;
  request.context = EngineContext(false);
  request.operation_id = "op.show.object_visibility";
  request.option_envelopes.push_back("target_ref:secret_customer_table");
  request.option_envelopes.push_back("target_ref_kind:object");
  const auto result = api::EngineSecurityInspectOperation(request);
  Require(!result.ok, "security refusal unexpectedly succeeded");
  Require(!result.diagnostics.empty(), "security refusal omitted diagnostics");
  for (const auto& diagnostic : result.diagnostics) {
    Require(diagnostic.detail.find("secret_customer_table") == std::string::npos,
            "security refusal leaked target reference in diagnostic detail");
  }
}

void RequireOverlapRoutesPreserved() {
  const auto plural_jobs = RunPipeline("SHOW JOBS");
  Require(plural_jobs.verifier.admitted, "SHOW JOBS verifier rejected plural route");
  Require(plural_jobs.envelope.operation_id == "observability.show_jobs",
          "SHOW JOBS did not route to plural jobs operation");
  Require(!Contains(plural_jobs.envelope.payload, "\"surface_key\":\"show.job\""),
          "SHOW JOBS was hijacked by singular SHOW JOB route");

  const auto singular_job = RunPipeline("SHOW JOB job_daily");
  Require(singular_job.verifier.admitted, "SHOW JOB verifier rejected singular route");
  Require(singular_job.envelope.operation_id == "op.show.job",
          "SHOW JOB was hijacked away from singular job operation");
  Require(Contains(singular_job.envelope.payload, "\"target_ref\":\"job_daily\""),
          "SHOW JOB singular target reference missing");

  const auto management_root = RunPipeline("SHOW MANAGEMENT");
  Require(management_root.verifier.admitted, "SHOW MANAGEMENT verifier rejected existing route");
  Require(management_root.envelope.operation_id == "observability.show_management",
          "SHOW MANAGEMENT root route was hijacked by management exact subroutes");
  Require(!Contains(management_root.envelope.payload, "\"public_sbsql_exact_command\":true"),
          "SHOW MANAGEMENT root route was marked as an exact subroute");

  const auto metrics_root = RunPipeline("SHOW METRICS");
  Require(metrics_root.verifier.admitted, "SHOW METRICS verifier rejected root route");
  Require(metrics_root.envelope.operation_id == "observability.show_metrics",
          "SHOW METRICS did not route to metrics root exact operation");
  Require(!Contains(metrics_root.envelope.payload, "\"surface_key\":\"show.metrics_family\""),
          "SHOW METRICS was hijacked by metrics-family route");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 21> kForbidden = {
      "sbsql_sblr_final_cleanup",
      "final_cleanup",
      "B001Exact",
      "IsB001",
      "b001_",
      "_b001",
      "B002Exact",
      "IsB002",
      "b002_",
      "_b002",
      "EngineRunSbsqlSblrFinalCleanup",
      "AUDIT-0",
      "AUDIT-1",
      "AUDIT-2",
      "AUDIT-3",
      "AUDIT-4",
      "AUDIT-5",
      "AUDIT-6",
      "AUDIT-7",
      "AUDIT-8",
      "AUDIT-9",
  };
  const std::filesystem::path source_root =
      std::filesystem::path(SCRATCHBIRD_PROJECT_SOURCE_DIR) / "src";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    for (const auto token : kForbidden) {
      Require(!Contains(text, token),
              std::string("production source contains forbidden batch token ") +
                  std::string(token) + " in " + entry.path().string());
    }
  }
}

}  // namespace

int main() {
  RequireProductionSourceIntegrity();
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireSblrRegistryAndDispatch(row);
  }
  RequireInvalidSyntaxDiagnostics();
  RequireSecurityRefusalRedaction();
  RequireOverlapRoutesPreserved();
  std::cout << "sbsql_sblr_final_cleanup_b002_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
