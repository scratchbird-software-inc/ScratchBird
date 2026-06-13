// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_hooks_api.hpp"
#include "agents/agent_management_api.hpp"
#include "agent_runtime_manager.hpp"
#include "sblr_dispatch.hpp"
#include "storage/storage_management_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
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

std::string Id(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1917019000000ull + salt);
  Require(generated.ok(), "PFAR-017A UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

void RequireTypedUuid(platform::UuidKind kind,
                      std::string_view value,
                      std::string_view field_name) {
  Require(value.find(':') == std::string_view::npos, std::string(field_name) + " used label prefix");
  Require(value.rfind("agent.", 0) != 0 && value.rfind("policy.", 0) != 0 &&
              value.rfind("scope.", 0) != 0,
          std::string(field_name) + " used fake catalog label");
  Require(uuid::ParseDurableEngineIdentityUuid(kind, std::string(value)).ok(),
          std::string(field_name) + " is not a typed durable engine UUID");
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid = Id(platform::UuidKind::database, 1);
  std::string filespace_uuid = Id(platform::UuidKind::filespace, 2);
  std::string transaction_uuid = Id(platform::UuidKind::transaction, 3);
  std::string principal_uuid = Id(platform::UuidKind::principal, 4);
  std::string agent_uuid = Id(platform::UuidKind::object, 5);
  std::string policy_uuid = Id(platform::UuidKind::object, 6);

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_pfar017a_" + std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "pfar017a.sbdb";
  return fixture;
}

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::initializer_list<std::string_view> rights) {
  api::EngineRequestContext context;
  context.request_id = "pfar-017a-open-state-mode";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.transaction_uuid.canonical = fixture.transaction_uuid;
  context.session_uuid.canonical = Id(platform::UuidKind::object, 7);
  context.node_uuid.canonical = Id(platform::UuidKind::object, 8);
  context.cluster_uuid.canonical = Id(platform::UuidKind::object, 9);
  context.local_transaction_id = 17017;
  context.security_context_present = true;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.trace_tags.push_back("security.fixture_trace_authority");
  for (const auto right : rights) {
    context.trace_tags.push_back("right:" + std::string(right));
  }
  return context;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
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

agents::AgentRuntimeActivationEvidence ValidEvidence(const Fixture& fixture) {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = fixture.database_uuid;
  evidence.engine_instance_uuid = Id(platform::UuidKind::object, 10);
  evidence.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  evidence.policy_generation = 2;
  evidence.catalog_generation = 3;
  evidence.security_generation = 4;
  evidence.filespace_generation = 5;
  evidence.agent_set_generation = 6;
  evidence.health_generation = 7;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  evidence.dependency_graph_consistent = true;
  return evidence;
}

api::EngineObjectReference FilespaceTarget(const Fixture& fixture) {
  api::EngineObjectReference target;
  target.uuid.canonical = fixture.filespace_uuid;
  target.object_kind = "filespace";
  return target;
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

api::EngineRequestPagePreallocationRequest PageHookRequest(const Fixture& fixture) {
  api::EngineRequestPagePreallocationRequest request;
  request.context = Context(fixture, {"OBS_AGENT_CONTROL"});
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
  request.option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request.option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request.option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request.option_envelopes.push_back("agent_metric_snapshot_trust_provenance:test_metric_registry");
  request.option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" + fixture.database_uuid);
  request.option_envelopes.push_back("agent_metric_snapshot_digest:sha256:open-state:page_allocation_manager");
  request.option_envelopes.push_back("agent_metric_snapshot_id:open-state:page_allocation_manager");
  request.option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" + fixture.agent_uuid);
  return request;
}

api::EngineAgentCatalogIdentitySource AgentIdentity(const Fixture& fixture) {
  api::EngineAgentCatalogIdentitySource source;
  source.agent_type_id = "page_allocation_manager";
  source.agent_uuid = fixture.agent_uuid;
  source.policy_uuid = fixture.policy_uuid;
  source.scope_uuid = Id(platform::UuidKind::object, 12);
  source.policy_name = "page allocation policy";
  source.scope_kind = "database";
  return source;
}

api::EngineThirdPartyAgentManagementRequest ThirdPartyRequest(
    const Fixture& fixture,
    std::string lifecycle_option) {
  api::EngineThirdPartyAgentManagementRequest request;
  request.context = Context(fixture, {"OBS_AGENT_CONTROL"});
  request.option_envelopes.push_back(std::move(lifecycle_option));
  request.agent_catalog_identity_sources.push_back(AgentIdentity(fixture));
  auto& record = request.management_request;
  record.request_uuid = Id(platform::UuidKind::object, 11);
  record.requester_principal_uuid = fixture.principal_uuid;
  record.external_system_id = "ops-orchestrator";
  record.agent_ref = fixture.agent_uuid;
  record.operation = "agents.disable";
  record.requested_action = "disable";
  record.policy_ref = fixture.policy_uuid;
  record.reason_code = "open_state_mode_gate";
  record.requested_expiry = "2026-05-22T00:00:00Z";
  record.redaction_context = "management_ui_summary";
  record.idempotency_key = "pfar-017a-third-party-open-state";
  record.protected_payload = "safe summary";
  return request;
}

api::EngineFilespacePreallocateRequest FilespacePreallocateRequest(
    const Fixture& fixture) {
  api::EngineFilespacePreallocateRequest request;
  request.context = Context(fixture, {"OBS_AGENT_CONTROL", "FILESPACE_LIFECYCLE_CONTROL"});
  request.target_object = FilespaceTarget(fixture);
  request.option_envelopes.push_back("requested_pages:8");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("evidence_sink_available:true");
  return request;
}

sblr::SblrDispatchResult DispatchCommand(const Fixture& fixture,
                                         std::string operation_id,
                                         std::string opcode,
                                         std::string lifecycle_option) {
  api::EngineApiRequest api_request;
  api_request.option_envelopes.push_back(std::move(lifecycle_option));
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "pfar-017a");
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  sblr::SblrDispatchRequest request{
      Context(fixture, {"OBS_AGENT_CONTROL"}),
      envelope,
      std::move(api_request)};
  return sblr::DispatchSblrOperation(request);
}

void TestRuntimeManagerModesDrainAndSuppressLoops(const Fixture& fixture) {
  agents::DatabaseLocalAgentRuntimeManager manager;
  auto evidence = ValidEvidence(fixture);
  evidence.lifecycle_mode = agents::AgentLifecycleMode::repair;
  const auto repair = manager.Start(evidence, {});
  Require(repair.status.ok, "repair-mode runtime manager failed");
  Require(repair.state == agents::AgentRuntimeManagerState::safe_mode,
          "repair mode did not enter safe_mode");
  Require(!repair.ordinary_admission_allowed,
          "repair mode admitted ordinary agent work");
  Require(repair.supervised_agents.empty(),
          "repair mode started normal supervised agents");

  agents::AgentRuntimeManagerConfig shutdown_config;
  shutdown_config.shutdown_requested = true;
  evidence.lifecycle_mode = agents::AgentLifecycleMode::shutdown;
  const auto stopped = manager.Start(evidence, shutdown_config);
  Require(stopped.status.ok, "shutdown-mode runtime manager failed");
  Require(stopped.state == agents::AgentRuntimeManagerState::stopped,
          "shutdown mode did not stop runtime manager");
  Require(stopped.shutdown_coordination_complete,
          "shutdown mode did not complete drain coordination");
  Require(!stopped.ordinary_admission_allowed,
          "shutdown mode admitted ordinary work");
}

void TestManagementAndSblrOpenStateRefusals(const Fixture& fixture) {
  auto read = CommandRequest(
      fixture, "agents.actions.list", {"OBS_AGENT_RECOMMENDATION_READ"});
  read.option_envelopes.push_back("lifecycle:repair");
  const auto read_result = api::EngineAgentCommandSurfaceOperation(read);
  Require(read_result.ok, "repair-mode read surface was denied");
  Require(HasDiagnostic(read_result, "AGENT.NONE"),
          "repair-mode read surface diagnostic mismatch");

  auto mutate = CommandRequest(fixture, "agents.disable", {"OBS_AGENT_CONTROL"});
  mutate.option_envelopes.push_back("lifecycle:repair");
  const auto mutate_result = api::EngineAgentCommandSurfaceOperation(mutate);
  Require(!mutate_result.ok, "repair-mode mutation was accepted");
  Require(HasDiagnostic(mutate_result, "AGENT.MANAGEMENT.REPAIR_DENIED"),
          "repair-mode mutation diagnostic mismatch");

  api::EngineStartAgentRequest start;
  start.context = Context(fixture, {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  start.target_object.object_kind = "storage_health_manager";
  start.option_envelopes.push_back("lifecycle:maintenance");
  const auto start_result = api::EngineStartAgent(start);
  Require(!start_result.ok, "maintenance-mode direct start was accepted");
  Require(HasDiagnostic(start_result, "AGENT.MANAGEMENT.MAINTENANCE_DENIED"),
          "maintenance-mode direct start diagnostic mismatch");

  const auto sblr_result = DispatchCommand(
      fixture,
      "agents.disable",
      "SBLR_AGENT_LIFECYCLE_DISABLE",
      "lifecycle:archive_hold");
  Require(sblr_result.accepted && sblr_result.dispatched_to_api,
          "SBLR open-state command was not dispatched");
  Require(!sblr_result.api_result.ok, "SBLR archive-hold mutation was accepted");
  Require(HasDiagnostic(sblr_result.api_result, "AGENT.MANAGEMENT.ARCHIVE_HOLD_DENIED"),
          "SBLR archive-hold diagnostic mismatch");

  const auto third_party_result = api::EngineSubmitThirdPartyAgentManagementRequest(
      ThirdPartyRequest(fixture, "lifecycle:shutdown"));
  Require(!third_party_result.ok, "third-party shutdown mutation was accepted");
  Require(HasDiagnostic(third_party_result, "AGENT.MANAGEMENT.SHUTDOWN_IN_PROGRESS"),
          "third-party shutdown diagnostic mismatch");
  Require(FieldValue(third_party_result, "request_queued") != "true",
          "third-party shutdown request was queued");
}

void TestActionAndStorageModesRefuseBeforeMutation(const Fixture& fixture) {
  auto page = PageHookRequest(fixture);
  page.option_envelopes.push_back("lifecycle:archive_hold");
  const auto page_result = api::EngineRequestPagePreallocation(page);
  Require(!page_result.ok, "archive-hold page action hook was accepted");
  Require(HasDiagnostic(page_result, "AGENT.MANAGEMENT.ARCHIVE_HOLD_DENIED"),
          "archive-hold page action diagnostic mismatch");
  Require(FieldValue(page_result, "page_preallocation_ledger_mutated") != "true",
          "archive-hold page action mutated ledger");

  auto dry_run = PageHookRequest(fixture);
  dry_run.dry_run = true;
  dry_run.option_envelopes.push_back("lifecycle:archive_hold");
  const auto dry_run_result = api::EngineRequestPagePreallocation(dry_run);
  Require(!dry_run_result.ok, "archive-hold dry-run page action unexpectedly succeeded");
  Require(FieldValue(dry_run_result, "page_preallocation_ledger_mutated") != "true",
          "archive-hold dry-run page action mutated ledger");

  auto filespace = FilespacePreallocateRequest(fixture);
  filespace.option_envelopes.push_back("lifecycle:shutdown");
  const auto filespace_result = api::EngineFilespacePreallocate(filespace);
  Require(!filespace_result.ok, "shutdown filespace preallocate was accepted");
  Require(HasDiagnostic(filespace_result, "FILESPACE.SHUTDOWN_IN_PROGRESS"),
          "shutdown filespace diagnostic mismatch");
  Require(FieldValue(filespace_result, "filespace_preallocation_ledger_mutated") != "true",
          "shutdown filespace preallocate mutated ledger");

  Require(!HasEvidence(page_result, "storage_executor"),
          "refused page hook emitted storage executor evidence");
  Require(!HasEvidence(filespace_result, "storage_executor"),
          "refused filespace preallocate emitted storage executor evidence");
}

}  // namespace

int main() {
  const auto fixture = MakeFixture();
  TestRuntimeManagerModesDrainAndSuppressLoops(fixture);
  TestManagementAndSblrOpenStateRefusals(fixture);
  TestActionAndStorageModesRefuseBeforeMutation(fixture);
  return EXIT_SUCCESS;
}
