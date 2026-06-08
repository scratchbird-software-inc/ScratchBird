// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_hooks_api.hpp"
#include "agents/agent_management_api.hpp"
#include "agent_runtime.hpp"
#include "agent_runtime_manifest.hpp"
#include "online_maintenance_progress.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace scratchbird::core::agents;
using namespace scratchbird::engine::internal_api;
using namespace scratchbird::engine::sblr;

namespace {

struct CheckState {
  bool ok = true;
  std::vector<std::string> failures;
};

void Require(CheckState* state, bool condition, const std::string& message) {
  if (!condition) {
    state->ok = false;
    state->failures.push_back(message);
  }
}

AgentRuntimeContext CoreContext(bool cluster = false) {
  AgentRuntimeContext ctx;
  ctx.security_context_present = true;
  ctx.cluster_authority_available = cluster;
  ctx.cluster_time_majority_available = cluster;
  ctx.private_features_available = true;
  ctx.standalone_edition = !cluster;
  ctx.wall_now_microseconds = 1000000;
  ctx.monotonic_now_microseconds = 1000000;
  ctx.cluster_now_microseconds = cluster ? 1000000 : 0;
  ctx.rights = {"OBS_AGENT_STATE_READ", "OBS_AGENT_EVIDENCE_READ", "OBS_AGENT_CONTROL", "OBS_AGENT_POLICY_CONTROL", "OBS_AGENT_OVERRIDE", "OBS_METRICS_READ_ALL", "OBS_CLUSTER_CONTROL"};
  ctx.groups = {"OPS"};
  ctx.trace_tags = {"security.bootstrap"};
  return ctx;
}

EngineRequestContext ApiContext(bool cluster = false) {
  EngineRequestContext ctx;
  ctx.trust_mode = EngineTrustMode::embedded_in_process;
  ctx.security_context_present = true;
  ctx.cluster_authority_available = cluster;
  ctx.database_path = "/tmp/sb_agent_runtime_probe.db";
  ctx.request_id = "sb-agent-runtime-probe";
  ctx.trace_tags = {"security.bootstrap", "group:OPS", "right:OBS_AGENT_STATE_READ", "right:OBS_AGENT_EVIDENCE_READ", "right:OBS_AGENT_CONTROL", "right:OBS_AGENT_POLICY_CONTROL", "right:OBS_AGENT_OVERRIDE", "right:OBS_METRICS_READ_ALL", "right:OBS_CLUSTER_CONTROL"};
  return ctx;
}

EngineLocalizedName Name(const std::string& value) { return {"en", "default", value, value, true}; }

EngineObjectReference ObjectRef(const std::string& kind, const std::string& uuid) {
  EngineObjectReference ref;
  ref.object_kind = kind;
  ref.uuid.canonical = uuid;
  return ref;
}

EngineListAgentsRequest ListRequest() {
  EngineListAgentsRequest req;
  req.context = ApiContext(false);
  return req;
}

EngineShowAgentRequest ShowRequest(const std::string& type, bool cluster = false) {
  EngineShowAgentRequest req;
  req.context = ApiContext(cluster);
  req.localized_names.push_back(Name(type));
  if (cluster) {
    req.option_envelopes.push_back("cluster_time_majority:true");
    req.option_envelopes.push_back("cluster_now_us:1000000");
    req.option_envelopes.push_back("standalone_edition:false");
  }
  return req;
}

EngineDryRunAgentRequest DryRunRequest(const std::string& type) {
  EngineDryRunAgentRequest req;
  req.context = ApiContext(false);
  req.localized_names.push_back(Name(type));
  return req;
}

EngineRequestPagePreallocationRequest PagePreallocationHookRequest() {
  EngineRequestPagePreallocationRequest req;
  req.context = ApiContext(false);
  req.context.local_transaction_id = 77;
  req.context.transaction_uuid.canonical = "tx:agent-hook:page-preallocation";
  req.agent_type = "page_allocation_manager";
  req.action_class = "request_action";
  req.agent_uuid.canonical = "agent:local:page_allocation_manager";
  req.policy_snapshot_uuid.canonical = "policy:page_allocation_manager:probe";
  req.target_filespace = ObjectRef("filespace", "filespace:primary");
  req.page_family = "data";
  req.page_type = "heap_data";
  req.safety_fence_result = "passed";
  req.cooldown_key = "cooldown:page_preallocation";
  req.requested_pages = 8;
  req.policy_authorized = true;
  req.evidence_sink_available = true;
  req.metrics_fresh = true;
  return req;
}

EngineRequestFilespaceGrowthRequest FilespaceGrowthHookRequest() {
  EngineRequestFilespaceGrowthRequest req;
  req.context = ApiContext(false);
  req.context.local_transaction_id = 78;
  req.context.transaction_uuid.canonical = "tx:agent-hook:filespace-growth";
  req.agent_type = "filespace_capacity_manager";
  req.action_class = "request_action";
  req.agent_uuid.canonical = "agent:local:filespace_capacity_manager";
  req.policy_snapshot_uuid.canonical = "policy:filespace_capacity_manager:probe";
  req.target_filespace = ObjectRef("filespace", "filespace:primary");
  req.safety_fence_result = "passed";
  req.cooldown_key = "cooldown:filespace_growth";
  req.requested_bytes = 65536;
  req.policy_authorized = true;
  req.evidence_sink_available = true;
  req.metrics_fresh = true;
  return req;
}

SblrDispatchResult DispatchAgentHookViaSblr() {
  SblrDispatchRequest dispatch;
  dispatch.context = ApiContext(false);
  dispatch.context.local_transaction_id = 79;
  dispatch.context.transaction_uuid.canonical = "tx:agent-hook:sblr";
  dispatch.envelope = MakeSblrEnvelope("agents.request_page_preallocation", "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION");
  dispatch.envelope.requires_transaction_context = true;
  dispatch.api_request.target_object = ObjectRef("filespace", "filespace:primary");
  dispatch.api_request.option_envelopes = {
      "agent_type:page_allocation_manager",
      "action_class:request_action",
      "agent_uuid:agent:local:page_allocation_manager",
      "policy_snapshot_uuid:policy:page_allocation_manager:sblr",
      "page_family:data",
      "page_type:heap_data",
      "safety_fence_result:passed",
      "cooldown_key:cooldown:sblr",
      "requested_pages:8",
      "policy_authorized:true",
      "evidence_sink_available:true",
      "metrics_fresh:true",
      "idempotency_key:idem:agent-hook:sblr"};
  return DispatchSblrOperation(dispatch);
}

SblrDispatchResult DispatchAgentManagementViaSblr() {
  SblrDispatchRequest dispatch;
  dispatch.context = ApiContext(false);
  dispatch.envelope = MakeSblrEnvelope("agents.list", "SBLR_AGENTS_LIST");
  return DispatchSblrOperation(dispatch);
}

bool HasRows(const EngineApiResult& result) { return result.ok && !result.result_shape.rows.empty(); }

void RunOnlineMaintenanceChecks(CheckState* state) {
  OnlineMaintenanceStateStore maintenance_store;
  OnlineMaintenanceStartRequest maintenance_start;
  maintenance_start.kind = OnlineMaintenanceOperationKind::optimizer_stats_refresh;
  maintenance_start.operation_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey("tool|odf121|operation");
  maintenance_start.database_uuid =
      DeterministicAgentRuntimeDatabaseUuidFromKey("tool|odf121|database");
  maintenance_start.target_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey("tool|odf121|target");
  maintenance_start.total_units = 4;
  maintenance_start.engine_mga_authoritative = true;
  maintenance_start.durable_checkpoint_persisted = true;
  maintenance_start.support_bundle_sink_available = true;
  maintenance_start.observability_sink_available = true;
  auto maintenance_started =
      StartOnlineMaintenanceOperation(&maintenance_store, maintenance_start);
  Require(state, maintenance_started.ok(), "online maintenance admits checkpointed operation");
  OnlineMaintenanceProgressRequest maintenance_progress;
  maintenance_progress.operation_uuid = maintenance_start.operation_uuid;
  maintenance_progress.completed_units = 4;
  maintenance_progress.total_units = 4;
  maintenance_progress.stage = "catalog_persisted";
  maintenance_progress.durable_checkpoint_persisted = true;
  auto maintenance_ready =
      RecordOnlineMaintenanceProgress(&maintenance_store, maintenance_progress);
  Require(state,
          maintenance_ready.ok() &&
              maintenance_ready.snapshot.phase == OnlineMaintenancePhase::completed,
          "online maintenance records stable non-publish completion");
}

void RunAllChecks(CheckState* state) {
  std::filesystem::remove("/tmp/sb_agent_runtime_probe.db");
  std::filesystem::remove("/tmp/sb_agent_runtime_probe.db.sb.api_events");

  const auto registry = CanonicalAgentRegistry();
  Require(state, ValidateCanonicalAgentRegistry().ok, "canonical registry validates");
  Require(state, registry.size() == CanonicalAgentManifestCount(),
          "canonical registry matches manifest count");
  Require(state, registry.size() == 29, "canonical registry has 29 agents");
  Require(state, FindAgentType("memory_governor").has_value(), "memory_governor registered");
  Require(state, FindAgentType("filespace_capacity_manager").has_value(), "filespace_capacity_manager registered");
  Require(state, FindAgentType("page_allocation_manager").has_value(), "page_allocation_manager registered");
  Require(state, FindAgentType("storage_health_manager")->authority == AgentAuthorityClass::recommend_only,
          "storage_health_manager is recommendation-only");
  Require(state, !FindAgentType("unknown_agent").has_value(), "unknown agent rejected");

  Require(state, ValidateAgentLifecycleTransition(AgentLifecycleState::created, AgentLifecycleState::registered).ok, "valid lifecycle transition accepted");
  Require(state, !ValidateAgentLifecycleTransition(AgentLifecycleState::stopped, AgentLifecycleState::running).ok, "invalid lifecycle transition rejected");

  const auto memory = *FindAgentType("memory_governor");
  auto policy = BaselinePolicyForAgent(memory);
  Require(state, ValidateAgentPolicy(policy, memory).ok, "baseline policy validates");
  Require(state, LintAgentPolicy(policy, memory).front().ok, "policy lint accepts baseline");
  policy.activation = AgentActivationProfile::live_action;
  policy.allow_live_action = false;
  Require(state, !ValidateAgentPolicy(policy, memory).ok, "unsafe live policy rejected");
  policy = BaselinePolicyForAgent(memory);

  auto local_ctx = CoreContext(false);
  auto cluster_ctx = CoreContext(true);
  Require(state, ValidateAgentSecurity(local_ctx, memory, "control").ok, "agent security accepts OPS control");
  local_ctx.security_context_present = false;
  Require(state, !ValidateAgentSecurity(local_ctx, memory, "control").ok, "agent security requires context");
  local_ctx = CoreContext(false);

  Require(state, ResolveAgentMetricDependencies(memory, local_ctx).ok, "local metric dependencies resolve");
  const auto cluster_agent = *FindAgentType("cluster_autoscale_manager");
  Require(state, !ResolveAgentMetricDependencies(cluster_agent, local_ctx).ok, "cluster metric dependencies fail closed locally");
  Require(state, ResolveAgentMetricDependencies(cluster_agent, cluster_ctx).ok, "cluster metric dependencies resolve with authority");

  Require(state, EvaluateAgentFeatureAvailability(cluster_agent, local_ctx) != AgentFeatureAvailability::available, "cluster feature gate fails closed on standalone");
  Require(state, EvaluateAgentFeatureAvailability(cluster_agent, cluster_ctx) == AgentFeatureAvailability::available, "cluster feature gate opens with authority");

  Require(state, ResolveAgentTimeAuthority(local_ctx, false).status.ok, "single node OS time authority available");
  Require(state, !ResolveAgentTimeAuthority(local_ctx, true).status.ok, "cluster majority time required for cluster actions");
  Require(state, ResolveAgentTimeAuthority(cluster_ctx, true).status.ok, "cluster majority time authority available");

  AgentInstanceRecord instance;
  instance.instance_uuid = "instance:memory_governor";
  instance.agent_type_id = "memory_governor";
  instance.policy_uuid = "policy:memory_governor:baseline";
  instance.scope = "node/database/session/workload";
  AgentInstanceRecord restored_instance;
  Require(state, RestoreAgentInstanceRecord(SerializeAgentInstanceRecord(instance), &restored_instance).ok, "agent instance serializes and restores");
  Require(state, AcquireAgentRunLease(&instance, policy, 1000000).ok, "run lease acquired");
  Require(state, !AcquireAgentRunLease(&instance, policy, 1000001).ok, "duplicate run lease rejected");
  Require(state, ValidateExecutionIsolation(instance, false, false).ok, "healthy execution isolation accepted");
  instance.crash_loop_count = 3;
  Require(state, !ValidateExecutionIsolation(instance, false, false).ok, "crash-loop quarantine triggered");

  AgentActionRequest action;
  action.action_uuid = "action:memory";
  action.agent_type_id = "memory_governor";
  action.action_class = AgentActionClass::direct_bounded_action;
  action.actuator_id = "memory_governor";
  action.operation_id = "tighten_budget";
  action.idempotency_key = "idem:memory:1";
  action.dry_run = true;
  Require(state, ValidateActionSafetyBudget(policy, action, 0).ok, "action safety budget accepts dry-run");
  Require(state, ValidateManualApproval(policy, action).ok, "manual approval not required for dry-run");
  Require(state, EvaluateAgentAction(CoreContext(false), memory, policy, action).result_class == AgentActionResultClass::dry_run_only, "action evaluates dry-run only");
  action.dry_run = false;
  Require(state, !ValidateManualApproval(policy, action).ok, "manual approval required for live action");
  Require(state, VerifyActionOutcome(action, true, false).result_class == AgentActionResultClass::failed_closed, "unverified outcome fails closed");

  Require(state, ValidateAgentResourceQuota(policy, policy.max_runtime_microseconds, 1, 1).ok, "resource quota accepts bounded use");
  Require(state, !ValidateAgentResourceQuota(policy, policy.max_runtime_microseconds + 1, 1, 1).ok, "resource quota rejects runtime excess");
  Require(state, ValidateAgentCardinality(policy, 1, 1, 1).ok, "cardinality accepts bounded use");
  Require(state, !ValidateAgentCardinality(policy, policy.max_label_cardinality + 1, 1, 1).ok, "cardinality rejects label explosion");
  Require(state, ValidateAgentDependencyGraph({"memory_governor->admission_control_manager"}).ok, "dependency graph accepts edge");
  Require(state, !ValidateAgentDependencyGraph({"memory_governor->memory_governor"}).ok, "dependency graph rejects self-loop");
  Require(state, ReplayAgentDecision(CoreContext(false), memory, BaselinePolicyForAgent(memory), {"sb_agent_actions_total"}).status.ok, "simulation replay accepts captured window");
  Require(state, !ReplayAgentDecision(CoreContext(false), memory, BaselinePolicyForAgent(memory), {}).status.ok, "simulation replay rejects incomplete captured window");
  Require(state, ValidateAgentOverheadGate(policy, 1, 1, 1).ok, "overhead gate accepts bounded use");
  Require(state, ValidateAgentStateMigration(1, 2).ok, "state migration upgrade accepted");
  Require(state, !ValidateAgentStateMigration(2, 1).ok, "state migration downgrade rejected");
  Require(state, EffectiveActivationForLifecycle(AgentActivationProfile::live_action, AgentLifecycleMode::read_only) == AgentActivationProfile::dry_run, "read-only lifecycle downgrades live action");
  Require(state, !ValidateRolloutTransition(AgentActivationProfile::disabled, AgentActivationProfile::live_action, false).ok, "rollout escalation requires approval");
  Require(state, ValidateRolloutTransition(AgentActivationProfile::disabled, AgentActivationProfile::live_action, true).ok, "rollout escalation accepts approval");
  Require(state, !ExplainAgentDecision(memory, BaselinePolicyForAgent(memory), BuildDryRunDecision(memory, action)).empty(), "explainability output exists");
  Require(state, IsKnownAgentDiagnosticCode("SB_AGENT_ACTION.DRY_RUN_ONLY"), "diagnostic code known");
  Require(state, !AgentFaultInjectionScenarios().empty(), "fault injection scenarios defined");
  Require(state, !EvaluateFaultInjectionScenario("lost_lease").ok, "fault injection fails closed");
  Require(state, AgentPersistenceUsesScratchBirdStorageAuthority(), "agent persistence uses ScratchBird storage authority");
  Require(state, !AgentCatalogRecordLayouts().empty(), "agent catalog layouts exist");
  Require(state, ValidateHumanCommandPrecedence("emergency_shutdown", action).diagnostic_code == "SB_AGENT_PRECEDENCE.HUMAN_COMMAND_WINS", "human command precedence enforced");

  Require(state, HasRows(EngineListAgents(ListRequest())), "engine list agents returns rows");
  Require(state, HasRows(EngineShowAgent(ShowRequest("memory_governor"))), "engine show agent returns rows");
  EngineSysAgentsRequest sys_req;
  sys_req.context = ApiContext(false);
  Require(state, HasRows(EngineSysAgents(sys_req)), "sys.agents returns rows");
  EngineClusterSysAgentsRequest cluster_sys_fail;
  cluster_sys_fail.context = ApiContext(false);
  Require(state, !EngineClusterSysAgents(cluster_sys_fail).ok, "cluster.sys.agents fails closed without cluster authority");
  EngineClusterSysAgentsRequest cluster_sys_ok;
  cluster_sys_ok.context = ApiContext(true);
  Require(state, HasRows(EngineClusterSysAgents(cluster_sys_ok)), "cluster.sys.agents returns rows with authority");
  Require(state, EngineDryRunAgent(DryRunRequest("memory_governor")).ok, "engine dry-run agent succeeds");

  auto page_hook = PagePreallocationHookRequest();
  Require(state, EngineRequestPagePreallocation(page_hook).ok, "page allocation agent hook accepts policy-authorized request");
  page_hook.metrics_fresh = false;
  Require(state, !EngineRequestPagePreallocation(page_hook).ok, "page allocation agent hook requires fresh metrics");
  page_hook = PagePreallocationHookRequest();
  page_hook.lifecycle_fence_active = true;
  Require(state, !EngineRequestPagePreallocation(page_hook).ok, "page allocation agent hook honors lifecycle fence");
  page_hook = PagePreallocationHookRequest();
  page_hook.cooldown_active = true;
  Require(state, EngineRequestPagePreallocation(page_hook).action_deferred, "page allocation agent hook defers during cooldown");
  Require(state, EngineRequestFilespaceGrowth(FilespaceGrowthHookRequest()).ok, "filespace capacity agent hook accepts growth request");
  Require(state, DispatchAgentHookViaSblr().api_result.ok, "SBLR dispatch maps agent action hook");
  Require(state, DispatchAgentManagementViaSblr().api_result.ok, "SBLR dispatch maps agent management surface");
}

}  // namespace

int main(int argc, char** argv) {
  CheckState state;
  const std::string probe_name =
      argc > 0 && argv != nullptr && argv[0] != nullptr
          ? std::filesystem::path(argv[0]).filename().string()
          : "sb_agent_runtime_probe";
  if (probe_name == "sb_agent_online_maintenance_probe") {
    RunOnlineMaintenanceChecks(&state);
  } else {
    RunAllChecks(&state);
  }
  std::cout << "{\n  \"ok\": " << (state.ok ? "true" : "false") << ",\n";
  std::cout << "  \"failure_count\": " << state.failures.size() << ",\n";
  std::cout << "  \"failures\": [";
  for (std::size_t i = 0; i < state.failures.size(); ++i) {
    if (i != 0) { std::cout << ", "; }
    std::cout << '"' << state.failures[i] << '"';
  }
  std::cout << "]\n}\n";
  return state.ok ? 0 : 1;
}
