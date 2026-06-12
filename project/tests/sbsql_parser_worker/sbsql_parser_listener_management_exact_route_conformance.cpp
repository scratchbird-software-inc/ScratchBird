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

struct ManagementRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view sql;
  std::string_view runtime_component;
  std::string_view runtime_target_name;
  std::string_view expected_field;
  std::string_view expected_value;
};

constexpr std::array<ManagementRowEvidence, 3> kManagementRows{{
    {"SBSQL-E37C329D2D91",
     "listener_stmt",
     "SBSQL-SURFACE-B8C892E5C2CB",
     "SHOW LISTENERS",
     "listeners",
     "",
     "listener_name",
     "native_listener"},
    {"SBSQL-A4DE345FD38F",
     "parser_name",
     "SBSQL-SURFACE-1EB3F62C9F8F",
     "SHOW PARSER sbsql",
     "parsers",
     "sbsql",
     "parser_name",
     "sbsql"},
    {"SBSQL-945FF66E5BF5",
     "parser_package_stmt",
     "SBSQL-SURFACE-92ABBE8E402F",
     "SHOW PARSER PACKAGES",
     "parser_packages",
     "",
     "package_name",
     "sbp_sbsql"},
}};

std::string EvidenceMessage(const ManagementRowEvidence& row,
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
  session.session_uuid = "019f0000-0000-7000-8000-000000002701";
  session.connection_uuid = "019f0000-0000-7000-8000-000000002702";
  session.database_uuid = "019f0000-0000-7000-8000-000000002703";
  session.catalog_epoch = 31;
  session.security_policy_epoch = 37;
  session.descriptor_epoch = 41;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000002704";
  config.bundle_contract_id = "sbp_sbsql@parser-listener-management-route-test";
  config.build_id = "sbsql-parser-listener-management-route-test";
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

void RequireRegistryEvidence(const ManagementRowEvidence& row) {
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

void RequireExactLowering(const ManagementRowEvidence& row) {
  const auto artifacts = RunPipeline(row.sql);
  Require(artifacts.bound.bound,
          EvidenceMessage(row, "parser_bind_lower", "management statement did not bind"));
  Require(!artifacts.bound.requires_name_resolution,
          EvidenceMessage(row, "parser_bind_lower", "management route required name registry resolution"));
  Require(!artifacts.bound.requires_transaction_authority,
          EvidenceMessage(row, "parser_bind_lower", "management route requested transaction authority"));
  Require(artifacts.bound.statement_surface_name == row.canonical_name,
          EvidenceMessage(row, "parser_bind_lower", "statement surface mismatch"));
  Require(artifacts.bound.statement_parser_category == "runtime_management",
          EvidenceMessage(row, "parser_bind_lower", "route did not bind as runtime management"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower", "SBLR verifier rejected exact route"));
  Require(artifacts.envelope.operation_family == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == "management.inspect_runtime",
          EvidenceMessage(row, "parser_bind_lower", "operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == "management.inspect_runtime",
          EvidenceMessage(row, "parser_bind_lower", "engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_MANAGEMENT_INSPECT_RUNTIME",
          EvidenceMessage(row, "parser_bind_lower", "opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.management_runtime_api_required"),
          EvidenceMessage(row, "parser_bind_lower", "management runtime authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_listener_or_parser_process_control"),
          EvidenceMessage(row, "parser_bind_lower", "parser process-control denial missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "parser no-SQL-execution authority step missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.management.runtime"),
          EvidenceMessage(row, "parser_bind_lower", "management runtime descriptor missing"));
  Require(HasValue(artifacts.envelope.required_rights, "right.management_runtime_read"),
          EvidenceMessage(row, "parser_bind_lower", "required right mismatch"));
  Require(!artifacts.envelope.parser_executes_sql,
          EvidenceMessage(row, "no_sql_engine_execution",
                          "management lowering allowed parser SQL execution"));
  Require(!artifacts.envelope.real_file_effects,
          EvidenceMessage(row, "no_reference_execution",
                          "management lowering allowed parser file effects"));
  Require(!Contains(artifacts.envelope.payload, row.sql),
          EvidenceMessage(row, "no_sql_text_authority", "management envelope embedded source SQL"));
  Require(Contains(artifacts.envelope.payload,
                   "\"management_envelope_kind\":\"parser_listener_runtime\""),
          EvidenceMessage(row, "parser_bind_lower", "management envelope kind missing"));
  const std::string runtime_component_needle =
      "\"runtime_component\":\"" + std::string(row.runtime_component) + "\"";
  Require(Contains(artifacts.envelope.payload, std::string_view(runtime_component_needle)),
          EvidenceMessage(row, "parser_bind_lower", "runtime component missing from payload"));
  Require(Contains(artifacts.envelope.payload,
                   "\"parser_controls_listener_or_parser_process\":false"),
          EvidenceMessage(row, "parser_bind_lower", "parser process-control denial missing"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          EvidenceMessage(row, "server_admission", "server admission rejected management route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "server_admission",
                          "server admission did not require engine public ABI dispatch"));
  Require(admission.operation_id == "management.inspect_runtime",
          EvidenceMessage(row, "server_admission", "server admission operation id mismatch"));
  Require(admission.operation_family == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "server_admission", "server admission family mismatch"));
}

api::EngineRequestContext EngineContext(const ManagementRowEvidence& row) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-parser-listener-management-exact-route";
  context.security_context_present = true;
  context.trace_tags.push_back("right:MANAGEMENT_RUNTIME_READ");
  context.trace_tags.push_back(std::string("sbsql_surface_id:") + std::string(row.surface_id));
  context.database_path = "/tmp/sbsql_parser_listener_management_exact_route_conformance.sbdb";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000002801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000002802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000002803";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000002804";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000002805";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000002806";
  context.catalog_generation_id = 31;
  context.security_epoch = 37;
  context.resource_epoch = 41;
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const ManagementRowEvidence& row) {
  auto envelope = sblr::MakeSblrEnvelope("management.inspect_runtime",
                                         "SBLR_MANAGEMENT_INSPECT_RUNTIME",
                                         std::string("trace.management_runtime.exact_route.") +
                                             std::string(row.surface_id));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireEngineDispatch(const ManagementRowEvidence& row) {
  const auto engine_envelope = EngineEnvelope(row);
  api::EngineApiRequest api_request;
  api_request.option_envelopes.push_back(std::string("runtime_component:") +
                                         std::string(row.runtime_component));
  if (!row.runtime_target_name.empty()) {
    api_request.option_envelopes.push_back(std::string("runtime_target_name:") +
                                           std::string(row.runtime_target_name));
  }
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
  Require(result.api_result.operation_id == "management.inspect_runtime",
          EvidenceMessage(row, "engine_dispatch", "engine API operation id mismatch"));
  Require(ApiResultHasEvidence(result.api_result, "parser_listener_runtime",
                               row.runtime_component),
          EvidenceMessage(row, "engine_dispatch", "expected parser/listener evidence missing"));
  Require(ApiResultHasEvidence(result.api_result, "authority_boundary",
                               "engine_owned_no_parser_process_control"),
          EvidenceMessage(row, "engine_dispatch", "authority-boundary evidence missing"));
  Require(ApiResultHasField(result.api_result, row.expected_field, row.expected_value),
          EvidenceMessage(row, "engine_dispatch", "expected management result field missing"));
}

}  // namespace

int main() {
  for (const auto& row : kManagementRows) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row);
    RequireEngineDispatch(row);
  }
  std::cout << "sbsql_parser_listener_management_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
