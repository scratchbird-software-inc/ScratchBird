// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_hooks_api.hpp"
#include "agents/agent_management_api.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "sblr_dispatch.hpp"
#include "storage/storage_management_api.hpp"
#include "uuid.hpp"

// SEARCH_KEY: AEIC_AGENT_SECURITY_NEGATIVE_REDACTION_TESTS

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string MakeUuidText(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "PFAR-014 UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string transaction_uuid;
  std::string principal_uuid;
  std::string agent_uuid;
  std::string policy_uuid;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_pfar014_" + std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "pfar014.sbdb";
  fixture.database_uuid = MakeUuidText(platform::UuidKind::database, 14);
  fixture.filespace_uuid = MakeUuidText(platform::UuidKind::filespace, 15);
  fixture.transaction_uuid = MakeUuidText(platform::UuidKind::transaction, 16);
  fixture.principal_uuid = MakeUuidText(platform::UuidKind::principal, 17);
  fixture.agent_uuid = MakeUuidText(platform::UuidKind::object, 18);
  fixture.policy_uuid = MakeUuidText(platform::UuidKind::object, 19);
  return fixture;
}

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::initializer_list<std::string_view> rights) {
  api::EngineRequestContext context;
  context.request_id = "pfar-014-management-authorization";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.cluster_uuid.canonical = MakeUuidText(platform::UuidKind::object, 20);
  context.node_uuid.canonical = MakeUuidText(platform::UuidKind::object, 21);
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = "session-pfar-014";
  context.transaction_uuid.canonical = fixture.transaction_uuid;
  context.local_transaction_id = 14014;
  context.security_context_present = true;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.cluster_authority_available = cluster_provider::ClusterProviderSupportsExecution();
  context.catalog_generation_id = 22;
  context.security_epoch = 23;
  context.resource_epoch = 24;
  for (const auto right : rights) {
    context.trace_tags.push_back("right:" + std::string(right));
  }
  context.trace_tags.push_back("security.fixture_trace_authority");
  return context;
}

api::EngineAgentCommandSurfaceRequest CommandRequest(
    const Fixture& fixture,
    std::string operation_id,
    std::initializer_list<std::string_view> rights) {
  api::EngineAgentCommandSurfaceRequest request;
  request.context = Context(fixture, rights);
  request.operation_id = std::move(operation_id);
  return request;
}

api::EngineObjectReference FilespaceTarget(const Fixture& fixture) {
  api::EngineObjectReference target;
  target.uuid.canonical = fixture.filespace_uuid;
  target.object_kind = "filespace";
  return target;
}

void AddObservedMetricSnapshotEvidence(api::EngineAgentActionHookRequest* request,
                                       const Fixture& fixture,
                                       std::string_view digest_suffix) {
  request->option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request->option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request->option_envelopes.push_back("agent_metric_snapshot_trust_provenance:test_metric_registry");
  request->option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  request->option_envelopes.push_back("agent_metric_snapshot_source_id:pfar014:source:" +
                                      std::string(digest_suffix));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:pfar014:key:" +
                                      std::string(digest_suffix));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:pfar014:attestation:" +
                                      std::string(digest_suffix));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  request->option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_protected_material_present:false");
  request->option_envelopes.push_back("agent_metric_snapshot_provenance_record:pfar014:provenance:" +
                                      std::string(digest_suffix));
  request->option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" + fixture.database_uuid);
  request->option_envelopes.push_back("agent_metric_snapshot_digest:sha256:pfar014:" +
                                      std::string(digest_suffix));
  request->option_envelopes.push_back("agent_metric_snapshot_schema_digest:sha256:pfar014:schema:" +
                                      std::string(digest_suffix));
  request->option_envelopes.push_back("agent_metric_snapshot_id:pfar014:" +
                                      std::string(digest_suffix));
  request->option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" +
                                      MakeUuidText(platform::UuidKind::object, 190));
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
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

std::string FieldValue(const api::EngineApiResult& result, std::string_view field) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == field) { return value.encoded_value; }
    }
  }
  return {};
}

api::EngineFilespacePreallocateRequest FilespacePreallocateRequest(
    const Fixture& fixture,
    std::initializer_list<std::string_view> rights) {
  api::EngineFilespacePreallocateRequest request;
  request.context = Context(fixture, rights);
  request.target_object = FilespaceTarget(fixture);
  request.option_envelopes.push_back("requested_pages:8");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("evidence_sink_available:true");
  return request;
}

api::EngineRequestPagePreallocationRequest PageHookRequest(const Fixture& fixture) {
  api::EngineRequestPagePreallocationRequest request;
  request.context = Context(fixture, {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  request.agent_type = "page_allocation_manager";
  request.action_class = "page_preallocation_request";
  request.agent_uuid.canonical = fixture.agent_uuid;
  request.policy_snapshot_uuid.canonical = fixture.policy_uuid;
  request.target_filespace = FilespaceTarget(fixture);
  request.page_family = "data";
  request.page_type = "relation";
  request.requested_pages = 4;
  request.policy_authorized = true;
  request.evidence_sink_available = true;
  request.metrics_fresh = true;
  request.safety_fence_result = "passed";
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  AddObservedMetricSnapshotEvidence(&request, fixture, "page-hook");
  return request;
}

sblr::SblrDispatchResult DispatchClusterAgentList(const Fixture& fixture,
                                                  bool with_rights) {
  sblr::SblrDispatchRequest request;
  request.context = with_rights
      ? Context(fixture, {"OBS_CLUSTER_HEALTH_INSPECT", "OBS_AGENT_STATE_READ"})
      : Context(fixture, {"OBS_AGENT_STATE_READ"});
  request.envelope = sblr::MakeSblrEnvelope(
      "cluster.agent.list", "SBLR_CLUSTER_AGENT_LIST", "pfar-014-cluster");
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.result_shape = "cluster.provider.stub.v1";
  request.envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  request.api_request.context = request.context;
  request.api_request.operation_id = "cluster.agent.list";
  return sblr::DispatchSblrOperation(request);
}

void TestCommandSurfaceAuthorization(const Fixture& fixture) {
  auto read = CommandRequest(
      fixture, "agents.actions.list", {"OBS_AGENT_RECOMMENDATION_READ"});
  const auto read_result = api::EngineAgentCommandSurfaceOperation(read);
  Require(read_result.ok, "authorized read-only action list was denied");
  Require(HasDiagnostic(read_result, "AGENT.NONE"),
          "read-only action list did not return exact empty diagnostic");

  auto no_context = read;
  no_context.context.security_context_present = false;
  const auto no_context_result = api::EngineAgentCommandSurfaceOperation(no_context);
  Require(!no_context_result.ok, "missing security context was accepted");
  Require(HasDiagnostic(no_context_result, "AGENT.SECURITY_CONTEXT_REQUIRED"),
          "missing security context diagnostic drifted");
  Require(HasEvidence(no_context_result, "agent_denial_evidence"),
          "missing security context denial evidence missing");

  auto denied_action = CommandRequest(fixture, "agents.action.approve", {});
  const auto denied_action_result =
      api::EngineAgentCommandSurfaceOperation(denied_action);
  Require(!denied_action_result.ok, "action approval without right accepted");
  Require(HasDiagnostic(denied_action_result, "ACTION.PERMISSION_DENIED"),
          "action approval denial diagnostic drifted");
  Require(!HasEvidence(denied_action_result, "agent_action_approval_evidence"),
          "denied approval emitted success evidence");

  auto policy_denied = CommandRequest(
      fixture, "agents.quarantine", {"OBS_AGENT_CONTROL"});
  policy_denied.option_envelopes.push_back("agent_management_policy:deny");
  const auto policy_result =
      api::EngineAgentCommandSurfaceOperation(policy_denied);
  Require(!policy_result.ok, "policy-denied mutation accepted");
  Require(HasDiagnostic(policy_result, "AGENT.POLICY_DENIED"),
          "policy denial diagnostic drifted");

  auto read_only_denied = CommandRequest(
      fixture, "agents.disable", {"OBS_AGENT_CONTROL"});
  read_only_denied.context.read_only_mode = true;
  const auto read_only_result =
      api::EngineAgentCommandSurfaceOperation(read_only_denied);
  Require(!read_only_result.ok, "read-only mutation accepted");
  Require(HasDiagnostic(read_only_result, "AGENT.MANAGEMENT.READ_ONLY_DENIED"),
          "read-only mutation diagnostic drifted");

  auto redacted = CommandRequest(
      fixture, "agents.evidence.list", {"OBS_AGENT_EVIDENCE_READ"});
  redacted.option_envelopes.push_back("agent_redaction_policy:summary_only");
  const auto redacted_result = api::EngineAgentCommandSurfaceOperation(redacted);
  Require(redacted_result.ok, "redacted evidence read was denied");
  Require(HasDiagnostic(redacted_result, "AGENT.REDACTED"),
          "redacted evidence diagnostic missing");
  Require(FieldValue(redacted_result, "payload_redacted") == "true",
          "redacted evidence payload flag missing");

  auto stale_metric = CommandRequest(
      fixture, "pages.allocation.show", {"OBS_METRICS_READ_FAMILY"});
  stale_metric.option_envelopes.push_back("metrics_fresh:false");
  const auto stale_metric_result =
      api::EngineAgentCommandSurfaceOperation(stale_metric);
  Require(!stale_metric_result.ok, "stale metric route accepted");
  Require(HasDiagnostic(stale_metric_result, "METRIC.STALE"),
          "stale metric diagnostic drifted");

  auto stale_policy = CommandRequest(
      fixture, "agents.policy.apply",
      {"OBS_POLICY_APPROVE", "OBS_POLICY_APPLY", "OBS_AGENT_CONTROL"});
  stale_policy.option_envelopes.push_back("policy_snapshot:stale");
  const auto stale_policy_result =
      api::EngineAgentCommandSurfaceOperation(stale_policy);
  Require(!stale_policy_result.ok, "stale policy mutation accepted");
  Require(HasDiagnostic(stale_policy_result, "POLICY.STALE"),
          "stale policy diagnostic drifted");
}

void TestFilespacePreallocateAuthorization(const Fixture& fixture) {
  auto missing_security_context = FilespacePreallocateRequest(
      fixture, {"OBS_AGENT_CONTROL", "FILESPACE_LIFECYCLE_CONTROL"});
  missing_security_context.context.security_context_present = false;
  const auto missing_security_result =
      api::EngineFilespacePreallocate(missing_security_context);
  Require(!missing_security_result.ok,
          "filespace preallocate accepted without security context");
  Require(HasDiagnostic(missing_security_result, "AGENT.SECURITY_CONTEXT_REQUIRED"),
          "filespace missing-security diagnostic drifted");
  Require(FieldValue(missing_security_result,
                     "filespace_preallocation_ledger_mutated") == "false",
          "missing-security filespace preallocate mutated ledger");

  auto missing_lifecycle = FilespacePreallocateRequest(
      fixture, {"OBS_AGENT_CONTROL"});
  const auto missing_lifecycle_result =
      api::EngineFilespacePreallocate(missing_lifecycle);
  Require(!missing_lifecycle_result.ok,
          "filespace preallocate accepted without lifecycle right");
  Require(HasDiagnostic(missing_lifecycle_result, "FILESPACE.PERMISSION_DENIED"),
          "filespace lifecycle denial diagnostic drifted");
  Require(FieldValue(missing_lifecycle_result,
                     "filespace_preallocation_ledger_mutated") == "false",
          "denied filespace preallocate mutated ledger");

  auto read_only = FilespacePreallocateRequest(
      fixture, {"OBS_AGENT_CONTROL", "FILESPACE_LIFECYCLE_CONTROL"});
  read_only.context.read_only_mode = true;
  const auto read_only_result = api::EngineFilespacePreallocate(read_only);
  Require(!read_only_result.ok, "read-only filespace preallocate accepted");
  Require(HasDiagnostic(read_only_result, "FILESPACE.READ_ONLY_DENIED"),
          "filespace read-only diagnostic drifted");
  Require(FieldValue(read_only_result,
                     "filespace_preallocation_ledger_mutated") == "false",
          "read-only filespace preallocate mutated ledger");

  auto allowed = FilespacePreallocateRequest(
      fixture, {"OBS_AGENT_CONTROL", "FILESPACE_LIFECYCLE_CONTROL"});
  const auto allowed_result = api::EngineFilespacePreallocate(allowed);
  Require(allowed_result.ok, "authorized filespace preallocate failed");
  Require(HasEvidence(allowed_result, "storage_executor", "PreallocateFilespace"),
          "authorized filespace preallocate storage evidence missing");
  Require(FieldValue(allowed_result,
                     "filespace_preallocation_ledger_mutated") == "true",
          "authorized filespace preallocate did not mutate ledger");
}

void TestHookOpenModeAuthorization(const Fixture& fixture) {
  auto request = PageHookRequest(fixture);
  request.option_envelopes.push_back("lifecycle:read_only");
  const auto result = api::EngineRequestPagePreallocation(request);
  Require(!result.ok, "read-only page preallocation hook accepted");
  Require(result.refusal_reason == "agent_management_read_only_denied",
          "read-only hook refusal drifted: " + result.refusal_reason);
  Require(!HasEvidence(result, "page_preallocation"),
          "read-only hook created page preallocation evidence");
}

void TestHookRequiresStrictObservedMetricSnapshot(const Fixture& fixture) {
  auto request = PageHookRequest(fixture);
  request.option_envelopes.clear();
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  const auto missing = api::EngineRequestPagePreallocation(request);
  Require(!missing.ok, "page hook accepted without strict observed metrics");
  Require(missing.refusal_reason.rfind(
              "SB_AGENT_METRIC_SNAPSHOT.PRODUCTION_OBSERVED_SNAPSHOT_REQUIRED",
              0) == 0,
          "missing observed metric refusal drifted: " + missing.refusal_reason);

  auto untrusted = PageHookRequest(fixture);
  untrusted.option_envelopes.clear();
  untrusted.option_envelopes.push_back("wall_now_us:100");
  untrusted.option_envelopes.push_back("monotonic_now_us:100");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_observed:true");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_trust_provenance:fixture_untrusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_source_id:pfar014:source:untrusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:pfar014:key:untrusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:pfar014:attestation:untrusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_protected_material_present:false");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_provenance_record:pfar014:provenance:untrusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" +
                                       fixture.database_uuid);
  untrusted.option_envelopes.push_back("agent_metric_snapshot_digest:sha256:pfar014:untrusted");
  untrusted.option_envelopes.push_back(
      "agent_metric_snapshot_schema_digest:sha256:pfar014:schema:untrusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_id:pfar014:untrusted");
  untrusted.option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" +
                                       MakeUuidText(platform::UuidKind::object, 191));
  const auto refused = api::EngineRequestPagePreallocation(untrusted);
  Require(!refused.ok, "page hook accepted untrusted observed metrics");
  Require(refused.refusal_reason.rfind("SB_AGENT_METRIC_SNAPSHOT.UNTRUSTED", 0) ==
              0,
          "untrusted observed metric refusal drifted: " + refused.refusal_reason);
}

void TestProductionHookRequiresDurableResourceReservationContext(
    const Fixture& fixture) {
  auto request = PageHookRequest(fixture);
  request.option_envelopes.push_back("agent_action_hook_production_live:true");
  request.option_envelopes.push_back(
      "agent_durable_catalog_fsync_or_checkpoint_evidence:true");
  const auto result = api::EngineRequestPagePreallocation(request);
  Require(!result.ok,
          "production page hook accepted without durable resource context");
  Require(result.refusal_reason.rfind(
              "SB_AGENT_HOOK_RESOURCE_RESERVATION.",
              0) == 0,
          "durable resource reservation refusal drifted: " +
              result.refusal_reason);
}

void TestClusterSecurityBeforeProvider(const Fixture& fixture) {
  const auto denied = DispatchClusterAgentList(fixture, false);
  Require(denied.accepted && denied.dispatched_to_api,
          "cluster denied route did not dispatch to API");
  Require(!denied.api_result.ok, "cluster route without local right accepted");
  Require(HasDiagnostic(denied.api_result, "CLUSTER.PERMISSION_DENIED"),
          "cluster local security diagnostic drifted");
  Require(!HasEvidence(denied.api_result, "agent_cluster_api_route"),
          "cluster provider boundary reached before local security denial");

  const auto routed = DispatchClusterAgentList(fixture, true);
  Require(routed.accepted && routed.dispatched_to_api,
          "cluster authorized route did not dispatch");
  const auto provider = cluster_provider::DescribeClusterProvider();
  if (provider.provider_type == std::string_view("compile_link_stub")) {
    Require(!routed.api_result.ok, "compile-link stub provider accepted route");
    Require(HasDiagnostic(
                routed.api_result,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub diagnostic missing");
  } else if (provider.supports_execution) {
    Require(routed.api_result.ok, "external cluster provider did not accept");
  } else {
    Require(!routed.api_result.ok, "no-cluster provider route accepted");
    Require(HasDiagnostic(routed.api_result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            "no-cluster diagnostic missing");
  }
  Require(HasEvidence(routed.api_result, "agent_cluster_api_route"),
          "authorized cluster route did not reach provider boundary");
}

}  // namespace

int main() {
  const auto fixture = MakeFixture();
  TestCommandSurfaceAuthorization(fixture);
  TestFilespacePreallocateAuthorization(fixture);
  TestHookOpenModeAuthorization(fixture);
  TestHookRequiresStrictObservedMetricSnapshot(fixture);
  TestProductionHookRequiresDurableResourceReservationContext(fixture);
  TestClusterSecurityBeforeProvider(fixture);
  return EXIT_SUCCESS;
}
