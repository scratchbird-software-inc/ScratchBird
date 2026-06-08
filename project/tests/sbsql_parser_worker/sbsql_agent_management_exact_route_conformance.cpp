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
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

struct AgentRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view expected_evidence;
  std::string_view expected_field;
  std::string_view expected_value;
  bool mutation;
};

constexpr std::string_view kAgentType = "memory_governor";

constexpr std::array<AgentRowEvidence, 5> kAgentRows{{
    {"SBSQL-8A62481C4584",
     "agent_stmt",
     "SBSQL-SURFACE-B64EB22F389B",
     "SHOW AGENT memory_governor",
     "agents.show",
     "SBLR_AGENTS_SHOW",
     "agent_type",
     "agent_type",
     "memory_governor",
     false},
    {"SBSQL-794DBFFF9565",
     "agent_name",
     "SBSQL-SURFACE-1F88A4CE9D41",
     "SHOW AGENT memory_governor",
     "agents.show",
     "SBLR_AGENTS_SHOW",
     "agent_type",
     "agent_type",
     "memory_governor",
     false},
    {"SBSQL-6CC28005D2B9",
     "agent_filter",
     "SBSQL-SURFACE-42AE9D16C494",
     "SHOW AGENTS WHERE AGENT_TYPE = memory_governor",
     "agents.list",
     "SBLR_AGENTS_LIST",
     "agent_registry",
     "agent_type",
     "memory_governor",
     false},
    {"SBSQL-47C66CD94ED9",
     "agent_control_stmt",
     "SBSQL-SURFACE-E68A38E649ED",
     "ALTER AGENT memory_governor START",
     "agents.start",
     "SBLR_AGENTS_START",
     "agent_runtime",
     "target_state",
     "running",
     true},
    {"SBSQL-D204B0D7F0A1",
     "agent_lifecycle_stmt",
     "SBSQL-SURFACE-C0E107503CF8",
     "ALTER AGENT memory_governor START",
     "agents.start",
     "SBLR_AGENTS_START",
     "agent_runtime",
     "target_state",
     "running",
     true},
}};

std::string EvidenceMessage(const AgentRowEvidence& row,
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
  session.session_uuid = "019f0000-0000-7000-8000-000000001701";
  session.connection_uuid = "019f0000-0000-7000-8000-000000001702";
  session.database_uuid = "019f0000-0000-7000-8000-000000001703";
  session.catalog_epoch = 17;
  session.security_policy_epoch = 19;
  session.descriptor_epoch = 23;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000001704";
  config.bundle_contract_id = "sbp_sbsql@agent-management-route-test";
  config.build_id = "sbsql-agent-management-route-test";
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

void RequireRegistryEvidence(const AgentRowEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "runtime_management",
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "registry", "SBLR operation family mismatch"));
  Require(registry_row->parser_handler_key == "parser.statement_family.runtime_management",
          EvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key ==
              "lowering.sblr_family.sblr_management_runtime_operation_v3",
          EvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key ==
              "server.admission.sblr_management_runtime_operation_v3",
          EvidenceMessage(row, "server_admission", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_management_runtime_operation_v3",
          EvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireExactLowering(const AgentRowEvidence& row) {
  const auto artifacts = RunPipeline(row.sql);
  Require(artifacts.bound.bound,
          EvidenceMessage(row, "parser_bind_lower", "agent statement did not bind"));
  Require(!artifacts.bound.requires_name_resolution,
          EvidenceMessage(row, "parser_bind_lower", "agent route required name registry resolution"));
  Require(!artifacts.bound.requires_transaction_authority,
          EvidenceMessage(row, "parser_bind_lower", "agent route requested transaction authority"));
  Require(artifacts.bound.statement_parser_category == "runtime_management",
          EvidenceMessage(row, "parser_bind_lower", "agent route did not bind as runtime management"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower", "agent SBLR verifier rejected exact route"));
  Require(artifacts.envelope.operation_family == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "agent operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "agent SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "agent operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          EvidenceMessage(row, "parser_bind_lower", "agent opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.agent_management_api_required"),
          EvidenceMessage(row, "parser_bind_lower", "agent management authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_agent_execution"),
          EvidenceMessage(row, "parser_bind_lower", "parser no-agent-execution step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "parser no-SQL-execution authority step missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.agents"),
          EvidenceMessage(row, "parser_bind_lower", "sys.agents descriptor evidence missing"));
  Require(row.mutation ==
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.engine.agent_evidence_required"),
          EvidenceMessage(row, "parser_bind_lower", "agent evidence authority mismatch"));
  Require(HasValue(artifacts.envelope.required_rights,
                   row.mutation ? "right.agent_control" : "right.agent_state_read"),
          EvidenceMessage(row, "parser_bind_lower", "agent required right mismatch"));
  Require(!artifacts.envelope.parser_executes_sql,
          EvidenceMessage(row, "no_sql_engine_execution",
                          "agent lowering allowed parser SQL execution"));
  Require(!artifacts.envelope.real_file_effects,
          EvidenceMessage(row, "no_donor_execution",
                          "agent lowering allowed parser file effects"));
  Require(!Contains(artifacts.envelope.payload, row.sql),
          EvidenceMessage(row, "no_sql_text_authority", "agent envelope embedded source SQL"));
  Require(Contains(artifacts.envelope.payload, "\"management_envelope_kind\":\"agent_runtime\""),
          EvidenceMessage(row, "parser_bind_lower", "agent management envelope kind missing"));
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_agent_action\":false"),
          EvidenceMessage(row, "parser_bind_lower", "parser agent-execution denial missing"));
  if (row.operation_id != "agents.list") {
    Require(Contains(artifacts.envelope.payload, "\"agent_type\":\"memory_governor\""),
            EvidenceMessage(row, "parser_bind_lower", "agent type missing from payload"));
  }

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          EvidenceMessage(row, "server_admission", "server admission rejected agent route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "server_admission",
                          "server admission did not require engine public ABI dispatch"));
  Require(admission.operation_id == row.operation_id,
          EvidenceMessage(row, "server_admission", "server admission operation id mismatch"));
  Require(admission.operation_family == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "server_admission", "server admission family mismatch"));
}

api::EngineRequestContext EngineContext(const AgentRowEvidence& row) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-agent-management-exact-route";
  context.security_context_present = true;
  context.trace_tags.push_back("right:OBS_AGENT_STATE_READ");
  context.trace_tags.push_back("right:OBS_AGENT_CONTROL");
  context.trace_tags.push_back(std::string("sbsql_surface_id:") + std::string(row.surface_id));
  context.database_path = "/tmp/sbsql_agent_management_exact_route_conformance.sbdb";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000001801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000001802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000001803";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000001804";
  context.cluster_uuid.canonical = "019f0000-0000-7000-8000-000000001807";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000001805";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000001806";
  context.catalog_generation_id = 17;
  context.security_epoch = 19;
  context.resource_epoch = 23;
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const AgentRowEvidence& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.agent_management.exact_route.") +
                                             std::string(row.surface_id));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireEngineDispatch(const AgentRowEvidence& row) {
  const auto engine_envelope = EngineEnvelope(row);
  api::EngineApiRequest api_request;
  api_request.option_envelopes.push_back("agent_type:memory_governor");
  api_request.option_envelopes.push_back("wall_now_us:1");
  api_request.option_envelopes.push_back("monotonic_now_us:1");
  api_request.option_envelopes.push_back("private_features:true");
  api_request.option_envelopes.push_back("standalone_edition:true");
  const sblr::SblrDispatchRequest request{
      EngineContext(row),
      engine_envelope,
      api_request};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          EvidenceMessage(row, "engine_dispatch", "engine SBLR envelope did not validate"));
  Require(result.accepted,
          EvidenceMessage(row, "engine_dispatch", "engine SBLR dispatch did not accept operation"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "engine_dispatch", "engine SBLR dispatch did not route to API"));
  Require(result.api_result.ok,
          EvidenceMessage(row, "engine_dispatch", "engine API returned failure"));
  Require(result.api_result.operation_id == row.operation_id,
          EvidenceMessage(row, "engine_dispatch", "engine API operation id mismatch"));
  Require(ApiResultHasEvidence(result.api_result, row.expected_evidence),
          EvidenceMessage(row, "engine_dispatch", "expected agent evidence missing"));
  Require(ApiResultHasField(result.api_result, row.expected_field, row.expected_value),
          EvidenceMessage(row, "engine_dispatch", "expected agent result field missing"));
  if (row.operation_id == "agents.list") {
    Require(result.api_result.result_shape.rows.size() == 1,
            EvidenceMessage(row, "engine_dispatch", "agent filter did not narrow list result"));
  }
}

}  // namespace

int main() {
  std::remove("/tmp/sbsql_agent_management_exact_route_conformance.sbdb.sb.api_events");
  for (const auto& row : kAgentRows) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row);
    RequireEngineDispatch(row);
  }
  std::cout << "sbsql_agent_management_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
