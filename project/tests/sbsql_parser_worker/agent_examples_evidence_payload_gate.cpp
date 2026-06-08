// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_hooks_api.hpp"
#include "agents/agent_management_api.hpp"
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "management/support_bundle_api.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kExpectedCompileLinkStubDiagnostic =
    "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY";

struct DocumentPaths {
  std::filesystem::path contract;
  std::filesystem::path reference;
};

struct ExampleRoute {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  bool cluster_scoped = false;
};

struct TempDir {
  std::filesystem::path path;
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(in.good(), "missing documentation file: " + path.string());
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string GeneratedUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1915020000000ull + seed);
  Require(generated.ok(), "PFAR-020A generated UUID fixture failed");
  return uuid::UuidToString(generated.value.value);
}

TempDir MakeTempDir(std::string_view prefix, platform::u64 seed) {
  TempDir dir;
  dir.path = std::filesystem::temp_directory_path() /
             (std::string(prefix) + "_" + GeneratedUuid(platform::UuidKind::object, seed));
  std::filesystem::create_directories(dir.path);
  return dir;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = GeneratedUuid(platform::UuidKind::object, 101);
  session.connection_uuid = GeneratedUuid(platform::UuidKind::object, 102);
  session.database_uuid = GeneratedUuid(platform::UuidKind::database, 103);
  session.catalog_epoch = 201;
  session.security_policy_epoch = 203;
  session.descriptor_epoch = 205;
  return session;
}

ParserConfig ParserConfigForExamples() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = GeneratedUuid(platform::UuidKind::object, 104);
  config.bundle_contract_id = "sbp_sbsql@pfar-020a-agent-examples";
  config.build_id = "pfar-020a-agent-examples";
  return config;
}

api::EngineRequestContext EngineContext(std::string_view required_right,
                                        bool security_context_present = true) {
  api::EngineRequestContext context;
  context.request_id = "pfar-020a-agent-examples";
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.security_context_present = security_context_present;
  context.cluster_authority_available = cluster_provider::ClusterProviderSupportsExecution();
  context.database_uuid.canonical = GeneratedUuid(platform::UuidKind::database, 201);
  context.cluster_uuid.canonical = GeneratedUuid(platform::UuidKind::object, 202);
  context.node_uuid.canonical = GeneratedUuid(platform::UuidKind::object, 203);
  context.principal_uuid.canonical = GeneratedUuid(platform::UuidKind::principal, 204);
  context.session_uuid.canonical = GeneratedUuid(platform::UuidKind::object, 205);
  context.transaction_uuid.canonical = GeneratedUuid(platform::UuidKind::transaction, 206);
  context.local_transaction_id = 9201;
  context.catalog_generation_id = 31;
  context.security_epoch = 37;
  context.resource_epoch = 41;
  context.trace_tags.push_back("right:OBS_AGENT_STATE_READ");
  context.trace_tags.push_back("right:OBS_CLUSTER_HEALTH_INSPECT");
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("pfar_020a_agent_examples");
  if (!required_right.empty()) {
    context.trace_tags.push_back("right:" + std::string(required_right));
  }
  return context;
}

sblr::SblrDispatchRequest DispatchRequest(const ExampleRoute& route,
                                          std::string_view required_right,
                                          bool security_context_present = true) {
  sblr::SblrDispatchRequest request;
  request.context = EngineContext(required_right, security_context_present);
  request.envelope = sblr::MakeSblrEnvelope(std::string(route.operation_id),
                                            std::string(route.opcode),
                                            "trace.pfar020a.agent_examples");
  request.envelope.result_shape = route.cluster_scoped ? "cluster.provider.stub.v1"
                                                       : "agent.command_surface.v1";
  request.envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  request.envelope.requires_security_context = true;
  request.envelope.requires_transaction_context = !route.cluster_scoped;
  request.envelope.requires_cluster_authority = route.cluster_scoped;
  request.api_request.context = request.context;
  request.api_request.operation_id = std::string(route.operation_id);
  return request;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code || Contains(diagnostic.detail, code)) {
      return true;
    }
  }
  return false;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code || Contains(diagnostic.message, code)) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::string DiagnosticSummary(const api::EngineApiResult& result) {
  std::string summary;
  for (const auto& diagnostic : result.diagnostics) {
    if (!summary.empty()) {
      summary += ";";
    }
    summary += diagnostic.code + ":" + diagnostic.detail;
  }
  return summary.empty() ? std::string("<none>") : summary;
}

std::string FieldValue(const api::EngineApiResult& result, std::string_view name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == name) {
        return field.second.encoded_value;
      }
    }
  }
  return {};
}

std::vector<api::EngineAgentCatalogIdentitySource> CatalogIdentities() {
  return {
      {.agent_type_id = "memory_governor",
       .agent_uuid = GeneratedUuid(platform::UuidKind::object, 301),
       .scope_uuid = GeneratedUuid(platform::UuidKind::database, 302),
       .policy_uuid = GeneratedUuid(platform::UuidKind::object, 303),
       .policy_name = "memory_governor_baseline",
       .component = "engine.runtime",
       .scope_kind = "database"},
      {.agent_type_id = "page_allocation_manager",
       .agent_uuid = GeneratedUuid(platform::UuidKind::object, 304),
       .scope_uuid = GeneratedUuid(platform::UuidKind::database, 305),
       .policy_uuid = GeneratedUuid(platform::UuidKind::object, 306),
       .policy_name = "page_preallocation_baseline",
       .component = "storage.pages",
       .scope_kind = "database"},
  };
}

void RequireDocumentCoverage(const DocumentPaths& paths,
                             const std::vector<ExampleRoute>& examples) {
  const auto spec = ReadText(paths.contract);
  const auto reference = ReadText(paths.reference);
  const auto combined = spec + "\n" + reference;
  const std::regex uuid_literal(
      R"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");

  Require(!std::regex_search(combined, uuid_literal),
          "documentation pack contains a fixed UUID literal instead of resolver placeholders");
  Require(Contains(combined, "<engine-generated:agent_uuid>"),
          "documentation pack missing generated agent UUID placeholder");
  Require(Contains(combined, "<engine-generated:evidence_uuid>"),
          "documentation pack missing generated evidence UUID placeholder");
  Require(Contains(combined, "payload_redacted"),
          "documentation pack missing payload redaction field");
  Require(Contains(combined, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
          "documentation pack missing no-cluster provider response");
  Require(cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode ==
              kExpectedCompileLinkStubDiagnostic,
          "cluster stub provider diagnostic constant drifted");
  Require(Contains(combined,
                   cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
          "documentation pack missing cluster stub provider response");
  Require(Contains(combined, "ACTION.PERMISSION_DENIED"),
          "documentation pack missing denied action diagnostic");
  Require(Contains(combined, "support_bundle_agent_runtime_evidence"),
          "documentation pack missing support evidence collection shape");
  Require(Contains(combined, "PreallocatePageFamilyPool"),
          "documentation pack missing page preallocation actuator evidence");
  Require(Contains(combined, "PreallocateFilespace"),
          "documentation pack missing filespace preallocation actuator evidence");
  Require(!Contains(combined, std::string("docs/") + "execution-plans") &&
              !Contains(combined, std::string("docs/") + "completed-execution-plans"),
          "documentation pack depends on execution_plan artifacts");

  for (const auto& example : examples) {
    Require(Contains(combined, example.sql),
            "documentation pack missing runnable command: " + std::string(example.sql));
    Require(Contains(combined, example.operation_id),
            "documentation pack missing operation id: " + std::string(example.operation_id));
    Require(Contains(combined, example.opcode),
            "documentation pack missing SBLR opcode: " + std::string(example.opcode));
  }
}

void RequireLoweringRoute(const ExampleRoute& route) {
  const auto session = ParserSession();
  const auto cst = BuildCst(route.sql);
  const auto ast = BuildAst(cst);
  const auto bound = BindAst(ast, cst, ParserConfigForExamples(), session, {});
  const auto envelope = LowerToSblr(bound, cst, session);
  const auto verifier = VerifySblrEnvelope(envelope);
  Require(bound.bound, std::string(route.sql) + " did not bind");
  Require(verifier.admitted,
          std::string(route.sql) + " verifier rejected route: " +
              RenderMessageVectorSet(verifier.messages));
  Require(envelope.operation_id == route.operation_id,
          std::string(route.sql) + " operation id mismatch");
  Require(envelope.sblr_opcode == route.opcode,
          std::string(route.sql) + " opcode mismatch");
  Require(envelope.operation_family ==
              (route.cluster_scoped ? "sblr.cluster.private_operation.v3"
                                    : "sblr.management.runtime_operation.v3"),
          std::string(route.sql) + " operation family mismatch");
  Require(!envelope.parser_executes_sql,
          std::string(route.sql) + " allowed parser-side execution");

  const auto admission = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted,
          std::string(route.sql) + " server admission rejected route");
  Require(admission.operation_id == route.operation_id,
          std::string(route.sql) + " server admission operation drifted");
}

bool ClusterProviderCompileLinkStubBuild() {
  const auto info = cluster_provider::DescribeClusterProvider();
  return info.compile_link_only && info.provider_type == "compile_link_stub";
}

void RequireClusterProviderExample(const ExampleRoute& route) {
  const auto dispatch = sblr::DispatchSblrOperation(
      DispatchRequest(route, "OBS_CLUSTER_HEALTH_INSPECT"));
  Require(dispatch.accepted && dispatch.dispatched_to_api,
          "cluster example did not dispatch to provider boundary");
  if (ClusterProviderCompileLinkStubBuild()) {
    Require(!dispatch.api_result.ok,
            "compile-link cluster stub unexpectedly executed example route");
    Require(HasDiagnostic(dispatch.api_result,
                          cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "cluster stub provider diagnostic missing");
    Require(HasDispatchDiagnostic(
                dispatch,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "cluster stub provider dispatch diagnostic missing");
    Require(HasEvidence(dispatch.api_result, "cluster_provider", "stub"),
            "cluster stub provider evidence missing");
  } else if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(dispatch.api_result.ok, "cluster stub provider refused example route");
  } else {
    Require(!dispatch.api_result.ok, "no-cluster build accepted cluster example route");
    Require(HasDiagnostic(dispatch.api_result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            "no-cluster API diagnostic missing");
    Require(HasDispatchDiagnostic(dispatch, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            "no-cluster dispatch diagnostic missing");
  }
}

void RequireCommandSurfaceExample(std::string operation_id,
                                  std::string expected_state,
                                  std::string expected_diagnostic,
                                  std::string expected_surface) {
  api::EngineAgentCommandSurfaceRequest request;
  request.context = EngineContext("OBS_AGENT_STATE_READ");
  request.context.trace_tags.push_back("right:OBS_POLICY_READ");
  request.context.trace_tags.push_back("right:OBS_AGENT_EVIDENCE_READ");
  request.context.trace_tags.push_back("right:OBS_AGENT_RECOMMENDATION_READ");
  request.operation_id = operation_id;
  request.target_object.object_kind = "memory_governor";
  request.agent_catalog_identity_sources = CatalogIdentities();

  const auto result = api::EngineAgentCommandSurfaceOperation(request);
  Require(result.ok || expected_state == "refused",
          "command surface example returned unexpected hard failure");
  Require(FieldValue(result, "result_state") == expected_state,
          operation_id + " result state mismatch");
  Require(FieldValue(result, "diagnostic_code") == expected_diagnostic,
          operation_id + " diagnostic field mismatch");
  Require(FieldValue(result, "sys_surface") == expected_surface,
          operation_id + " sys surface mismatch");
  Require(HasDiagnostic(result, expected_diagnostic),
          operation_id + " diagnostic vector missing");
  Require(HasEvidence(result, "agent_command_surface", operation_id),
          operation_id + " command-surface evidence missing");
  Require(!Contains(FieldValue(result, "agent_uuid"), "agent."),
          operation_id + " leaked synthetic agent UUID text");
  Require(!Contains(FieldValue(result, "policy_uuid"), "policy."),
          operation_id + " leaked synthetic policy UUID text");
}

void RequireDeniedActionExample() {
  const ExampleRoute route{"ALTER AGENT ACTION action_uuid APPROVE",
                           "agents.action.approve",
                           "SBLR_AGENT_ACTION_APPROVE",
                           false};
  const auto dispatch = sblr::DispatchSblrOperation(DispatchRequest(route, ""));
  Require(dispatch.accepted && dispatch.dispatched_to_api,
          "denied action example did not dispatch");
  Require(!dispatch.api_result.ok,
          "denied action example unexpectedly succeeded without approval right");
  Require(HasDiagnostic(dispatch.api_result, "ACTION.PERMISSION_DENIED"),
          "denied action example diagnostic missing");
  Require(HasEvidence(dispatch.api_result, "agent_denial_evidence"),
          "denied action example evidence missing");
  Require(!HasEvidence(dispatch.api_result, "agent_action_approval_evidence"),
          "denied action example produced approval evidence");
}

api::EngineObjectReference FilespaceTarget() {
  api::EngineObjectReference target;
  target.object_kind = "filespace";
  target.uuid.canonical = GeneratedUuid(platform::UuidKind::filespace, 401);
  return target;
}

void RequirePagePreallocationExample() {
  const auto temp_dir = MakeTempDir("sb_pfar020a_page", 450);
  api::EngineRequestPagePreallocationRequest request;
  request.context = EngineContext("OBS_AGENT_CONTROL");
  request.context.database_path = (temp_dir.path / "pfar020a.sbdb").string();
  request.context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  request.agent_type = "page_allocation_manager";
  request.action_class = "page_preallocation_request";
  request.agent_uuid.canonical = GeneratedUuid(platform::UuidKind::object, 402);
  request.policy_snapshot_uuid.canonical = GeneratedUuid(platform::UuidKind::object, 403);
  request.target_filespace = FilespaceTarget();
  request.page_family = "data";
  request.page_type = "relation";
  request.safety_fence_result = "passed";
  request.requested_pages = 4;
  request.policy_authorized = true;
  request.evidence_sink_available = true;
  request.metrics_fresh = true;
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  request.option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request.option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request.option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request.option_envelopes.push_back("agent_metric_snapshot_trust_provenance:example_metric_registry");
  request.option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  request.option_envelopes.push_back("agent_metric_snapshot_source_id:pfar020a:source");
  request.option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:pfar020a:key");
  request.option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:pfar020a:attestation");
  request.option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  request.option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  request.option_envelopes.push_back("agent_metric_snapshot_protected_material_present:false");
  request.option_envelopes.push_back("agent_metric_snapshot_provenance_record:pfar020a:provenance");
  request.option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" +
                                     request.context.database_uuid.canonical);
  request.option_envelopes.push_back("agent_metric_snapshot_digest:sha256:pfar020a:page");
  request.option_envelopes.push_back("agent_metric_snapshot_value_digest:sha256:pfar020a:page:value");
  request.option_envelopes.push_back("agent_metric_snapshot_schema_digest:sha256:pfar020a:page:schema");
  request.option_envelopes.push_back("agent_metric_snapshot_id:pfar020a:page");
  request.option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" +
                                     GeneratedUuid(platform::UuidKind::object, 404));

  const auto result = api::EngineRequestPagePreallocation(request);
  Require(result.ok, "page preallocation example failed: " +
                         result.refusal_reason + " diagnostics=" +
                         DiagnosticSummary(result));
  Require(HasEvidence(result, "agent_hook", "agents.request_page_preallocation"),
          "page preallocation hook evidence missing");
  Require(HasEvidence(result, "storage_executor", "PreallocatePageFamilyPool"),
          "page preallocation storage executor evidence missing");
  Require(FieldValue(result, "storage_execution") == "completed",
          "page preallocation storage execution did not complete");
  Require(FieldValue(result, "page_preallocation_ledger_mutated") == "true",
          "page preallocation ledger was not mutated");
}

void RequireSupportEvidenceExample() {
  api::EnginePrepareSupportBundleRequest request;
  request.context = EngineContext("OBS_AGENT_EVIDENCE_READ");
  request.option_envelopes.push_back("engine_authorized_support_export:true");

  api::EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = "page_allocation_manager";
  source.agent_uuid = GeneratedUuid(platform::UuidKind::object, 501);
  source.filespace_uuid = GeneratedUuid(platform::UuidKind::filespace, 502);
  source.policy_uuid = GeneratedUuid(platform::UuidKind::object, 503);
  source.evidence_uuid = GeneratedUuid(platform::UuidKind::object, 504);
  source.evidence_kind = "page_preallocation";
  source.result_state = "success";
  source.diagnostic_code = "AGENT.PAGE_PREALLOCATION.COMPLETED";
  source.payload_digest = "sha256:pfar020a";
  source.retention_class = "agent_control_audit";
  source.physical_path = "/tmp/protected/runtime.sbdb";
  source.unsafe_payload = "password=cleartext token=secret-token";
  source.payload_redacted = true;
  request.agent_runtime_evidence.push_back(source);

  const auto result = api::EnginePrepareSupportBundle(request);
  Require(result.ok, "support bundle evidence example failed");
  Require(result.agent_runtime_evidence_collected,
          "support bundle did not collect agent evidence");
  Require(result.redaction_applied, "support bundle did not apply redaction");
  Require(HasEvidence(result, "support_bundle_agent_runtime_evidence", "redacted"),
          "support evidence marker missing");
  Require(!Contains(FieldValue(result, "physical_path"), "/tmp/protected"),
          "support evidence leaked protected physical path");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 3,
          "usage: agent_examples_evidence_payload_gate <spec-doc> <reference-doc>");
  const DocumentPaths paths{argv[1], argv[2]};

  const std::vector<ExampleRoute> examples = {
      {"SHOW AGENT memory_governor METRICS",
       "agents.metrics.get",
       "SBLR_AGENT_METRICS_GET",
       false},
      {"SHOW AGENT memory_governor POLICY",
       "agents.policy.get",
       "SBLR_AGENT_POLICY_GET",
       false},
      {"SHOW AGENT memory_governor EVIDENCE",
       "agents.evidence.list",
       "SBLR_AGENT_EVIDENCE_LIST",
       false},
      {"SHOW AGENT ACTIONS",
       "agents.actions.list",
       "SBLR_AGENT_ACTION_LIST",
       false},
      {"ALTER AGENT ACTION action_uuid APPROVE",
       "agents.action.approve",
       "SBLR_AGENT_ACTION_APPROVE",
       false},
      {"SHOW CLUSTER AGENTS",
       "cluster.agent.list",
       "SBLR_CLUSTER_AGENT_LIST",
       true},
      {"SHOW FILESPACES",
       "filespaces.show",
       "SBLR_SHOW_FILESPACES",
       false},
      {"SHOW PAGE ALLOCATION BY FAMILY",
       "pages.allocation.family.show",
       "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY",
       false},
  };

  RequireDocumentCoverage(paths, examples);
  for (const auto& example : examples) {
    RequireLoweringRoute(example);
  }

  RequireCommandSurfaceExample("agents.policy.get",
                               "refused",
                               "POLICY.NOT_FOUND",
                               "sys.agent_policies");
  RequireCommandSurfaceExample("agents.evidence.list",
                               "refused",
                               "AGENT.EVIDENCE_NOT_FOUND",
                               "sys.agent_evidence");
  RequireCommandSurfaceExample("agents.actions.list",
                               "empty",
                               "AGENT.NONE",
                               "sys.agent_actions");
  RequireDeniedActionExample();
  RequireClusterProviderExample(examples[5]);
  RequirePagePreallocationExample();
  RequireSupportEvidenceExample();

  std::cout << "agent_examples_evidence_payload_gate=passed\n";
  return EXIT_SUCCESS;
}
