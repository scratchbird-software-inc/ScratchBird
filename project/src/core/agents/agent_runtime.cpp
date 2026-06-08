// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"
#include "agent_policy_schema.hpp"
#include "agent_runtime_manifest.hpp"

#include "metric_contracts.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace scratchbird::core::agents {
namespace {

namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

AgentMetricDependency ContractDep(std::string family,
                                  std::string ns,
                                  AgentMetricDependencyKind kind,
                                  u64 max_freshness_microseconds,
                                  AgentMetricSourceQuality source_quality,
                                  std::string required_quality,
                                  std::string aggregation,
                                  std::string policy_field,
                                  std::string decision_use,
                                  std::string fail_behavior,
                                  std::string evidence_field) {
  AgentMetricDependency dep;
  dep.metric_family = std::move(family);
  dep.namespace_prefix = std::move(ns);
  dep.dependency_kind = kind;
  dep.required = kind != AgentMetricDependencyKind::optional;
  dep.cluster_only = dep.namespace_prefix.rfind("cluster.sys.metrics.", 0) == 0;
  dep.max_freshness_microseconds = max_freshness_microseconds;
  dep.required_source_quality = source_quality;
  dep.required_quality = std::move(required_quality);
  dep.aggregation = std::move(aggregation);
  dep.policy_field = std::move(policy_field);
  dep.decision_use = std::move(decision_use);
  dep.fail_behavior = std::move(fail_behavior);
  dep.evidence_field = std::move(evidence_field);
  return dep;
}

AgentMetricDependencyContractRow ContractRow(std::string agent,
                                             std::string family,
                                             std::string ns,
                                             AgentMetricDependencyKind kind,
                                             u64 max_freshness_microseconds,
                                             AgentMetricSourceQuality source_quality,
                                             std::string required_quality,
                                             std::string aggregation,
                                             std::string policy_field,
                                             std::string decision_use,
                                             std::string fail_behavior,
                                             std::string evidence_field) {
  AgentMetricDependencyContractRow row;
  row.agent_type_id = std::move(agent);
  row.dependency = ContractDep(std::move(family), std::move(ns), kind,
                               max_freshness_microseconds, source_quality,
                               std::move(required_quality), std::move(aggregation),
                               std::move(policy_field), std::move(decision_use),
                               std::move(fail_behavior), std::move(evidence_field));
  return row;
}

u64 Seconds(u64 value) {
  return value * 1000000ull;
}

AgentTypeDescriptor Agent(std::string type_id,
                          AgentDeployment deployment,
                          std::string scope,
                          AgentAuthorityClass authority,
                          AgentActivationProfile activation,
                          std::vector<AgentMetricDependency> deps) {
  AgentTypeDescriptor descriptor;
  descriptor.type_id = std::move(type_id);
  descriptor.deployment = deployment;
  descriptor.scope = std::move(scope);
  descriptor.authority = authority;
  descriptor.default_activation = activation;
  descriptor.cluster_only = deployment == AgentDeployment::cluster;
  descriptor.metric_dependencies = std::move(deps);
  descriptor.required_rights.push_back("OBS_AGENT_STATE_READ");
  if (authority == AgentAuthorityClass::request_action || authority == AgentAuthorityClass::direct_bounded_action) {
    descriptor.required_rights.push_back("OBS_AGENT_CONTROL");
  }
  return descriptor;
}

AgentActionClass ContractActionClass(const std::string& action_id,
                                     const std::string& risk_class,
                                     const std::string& permission) {
  if (risk_class == "observe_notify" || risk_class == "evidence_publish") {
    return AgentActionClass::none;
  }
  if (risk_class == "recommendation" ||
      action_id.rfind("recommend_", 0) == 0 ||
      permission.find("OBS_AGENT_RECOMMENDATION_READ") != std::string::npos) {
    return AgentActionClass::recommendation;
  }
  if (permission.find("APPROVE") != std::string::npos ||
      permission.find("CANCEL") != std::string::npos ||
      risk_class.find("operator") != std::string::npos ||
      risk_class.find("security") != std::string::npos ||
      risk_class.find("destructive") != std::string::npos) {
    return AgentActionClass::manual_approval_required;
  }
  return AgentActionClass::direct_bounded_action;
}

bool ContractRequiresApproval(const std::string& risk_class,
                              const std::string& permission) {
  return permission.find("APPROVE") != std::string::npos ||
         permission.find("CANCEL") != std::string::npos ||
         risk_class.find("operator") != std::string::npos ||
         risk_class.find("security") != std::string::npos ||
         risk_class.find("destructive") != std::string::npos;
}

AgentActionResultClass ContractDefaultResult(AgentActionClass action_class) {
  switch (action_class) {
    case AgentActionClass::none:
    case AgentActionClass::recommendation:
      return AgentActionResultClass::accepted;
    case AgentActionClass::manual_approval_required:
      return AgentActionResultClass::approval_required;
    case AgentActionClass::direct_bounded_action:
    case AgentActionClass::request_action:
    case AgentActionClass::dry_run:
      return AgentActionResultClass::dry_run_only;
    case AgentActionClass::refusal:
    case AgentActionClass::override_action:
      return AgentActionResultClass::refused;
  }
  return AgentActionResultClass::failed_closed;
}

AgentActionContractDescriptor Contract(std::string action_id,
                                       std::string owning_agent,
                                       std::string actuator,
                                       std::string risk_class,
                                       std::string sync_async,
                                       std::string permission,
                                       std::string policy_gate,
                                       std::string metric_precondition_text,
                                       std::initializer_list<const char*> metric_families,
                                       std::string evidence_kind,
                                       std::string retry_cooldown,
                                       std::string failure_behavior) {
  AgentActionContractDescriptor descriptor;
  descriptor.action_id = std::move(action_id);
  descriptor.owning_agent = std::move(owning_agent);
  descriptor.actuator = std::move(actuator);
  descriptor.risk_class = std::move(risk_class);
  descriptor.sync_async = std::move(sync_async);
  descriptor.permission = std::move(permission);
  descriptor.policy_gate = std::move(policy_gate);
  descriptor.metric_precondition_text = std::move(metric_precondition_text);
  for (const char* family : metric_families) {
    descriptor.metric_families.emplace_back(family);
  }
  descriptor.evidence_kind = std::move(evidence_kind);
  descriptor.retry_cooldown = std::move(retry_cooldown);
  descriptor.failure_behavior = std::move(failure_behavior);
  descriptor.action_class = ContractActionClass(descriptor.action_id,
                                                descriptor.risk_class,
                                                descriptor.permission);
  descriptor.default_result_class = ContractDefaultResult(descriptor.action_class);
  descriptor.manual_approval_required =
      ContractRequiresApproval(descriptor.risk_class, descriptor.permission);
  descriptor.operator_approval_required = descriptor.manual_approval_required;
  descriptor.cluster_scoped = false;
  return descriptor;
}

std::string Join(const std::vector<std::string>& values, const std::string& sep) {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) { out += sep; }
    out += value;
  }
  return out;
}

u64 NowMicros() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool IsHigherActivation(AgentActivationProfile from, AgentActivationProfile to) {
  return static_cast<int>(to) > static_cast<int>(from);
}

bool IsLifecycleReadonlyMode(AgentLifecycleMode mode) {
  return mode == AgentLifecycleMode::backup || mode == AgentLifecycleMode::restore ||
         mode == AgentLifecycleMode::shutdown || mode == AgentLifecycleMode::crash_recovery ||
         mode == AgentLifecycleMode::restricted_open || mode == AgentLifecycleMode::read_only ||
         mode == AgentLifecycleMode::repair || mode == AgentLifecycleMode::archive_hold ||
         mode == AgentLifecycleMode::pitr || mode == AgentLifecycleMode::clone ||
         mode == AgentLifecycleMode::role_change;
}

bool IsUnsafeLeaseState(AgentLifecycleState state) {
  return state == AgentLifecycleState::disabled ||
         state == AgentLifecycleState::safe_mode ||
         state == AgentLifecycleState::quarantined ||
         state == AgentLifecycleState::stopping ||
         state == AgentLifecycleState::stopped ||
         state == AgentLifecycleState::retired ||
         state == AgentLifecycleState::failed;
}

bool IsSupervisionRunnableState(AgentLifecycleState state) {
  return state == AgentLifecycleState::observe_only ||
         state == AgentLifecycleState::recommend_only ||
         state == AgentLifecycleState::dry_run ||
         state == AgentLifecycleState::running ||
         state == AgentLifecycleState::paused ||
         state == AgentLifecycleState::registered;
}

void ApplySupervisionState(AgentInstanceRecord* instance, AgentLifecycleState state) {
  if (instance == nullptr) { return; }
  instance->state = state;
  instance->disabled_by_operator = state == AgentLifecycleState::disabled;
  instance->safe_mode = state == AgentLifecycleState::safe_mode;
  instance->quarantined = state == AgentLifecycleState::quarantined;
  if (IsUnsafeLeaseState(state)) {
    instance->lease_until_microseconds = 0;
  }
}

std::string SupervisionFailureDiagnostic(AgentSupervisionFailureKind failure_kind) {
  switch (failure_kind) {
    case AgentSupervisionFailureKind::tick_timeout:
      return "SB_AGENT_SUPERVISION.TICK_TIMEOUT";
    case AgentSupervisionFailureKind::watchdog_timeout:
      return "SB_AGENT_ISOLATION.WATCHDOG_EXPIRED";
    case AgentSupervisionFailureKind::exception:
      return "SB_AGENT_ISOLATION.EXCEPTION_CONTAINED";
    case AgentSupervisionFailureKind::runtime_timeout:
      return "SB_AGENT_SUPERVISION.RUNTIME_TIMEOUT";
    case AgentSupervisionFailureKind::action_failed:
      return "SB_AGENT_ACTION.FAILED_BACKOFF";
  }
  return "SB_AGENT_SUPERVISION.FAILURE";
}

std::string DefaultInvalidPolicyBehavior(const std::string& policy_family) {
  if (policy_family == "node_resource_policy") { return "mark suitability unknown"; }
  if (policy_family == "metric_registry_policy") { return "reject invalid samples"; }
  if (policy_family == "storage_health_policy") { return "health unknown and deny destructive actions"; }
  if (policy_family == "memory_governor_policy") { return "conservative grants"; }
  if (policy_family == "index_health_policy") { return "suppress recommendations"; }
  if (policy_family == "admission_control_policy") { return "conservative throttle"; }
  if (policy_family == "parser_health_policy") { return "deny new parser assignment"; }
  if (policy_family == "long_transaction_policy") { return "warning only"; }
  if (policy_family == "cleanup_archive_policy") { return "cleanup denied"; }
  if (policy_family == "policy_recommendation_policy") { return "no recommendation"; }
  if (policy_family == "optimizer_learning_policy") { return "disable correction"; }
  if (policy_family == "support_bundle_policy") { return "deny bundle"; }
  if (policy_family == "job_control_policy") { return "deny control action"; }
  if (policy_family == "backup_policy") { return "progress unknown or deny start"; }
  if (policy_family == "archive_policy") { return "hold slice"; }
  if (policy_family == "restore_drill_policy") { return "deny drill"; }
  if (policy_family == "pitr_policy") { return "report unreachable"; }
  if (policy_family == "identity_lifecycle_policy") { return "security unknown or deny action"; }
  if (policy_family == "session_control_policy") { return "deny session action"; }
  if (policy_family == "alerting_baseline") { return "local event only"; }
  if (policy_family == "export_default_baseline") { return "deny export"; }
  if (policy_family == "filespace_capacity_policy") { return "deny filespace mutation; emit recommendation/refusal only"; }
  if (policy_family == "filespace_shadow_promotion_policy") { return "no promotion recommendation"; }
  if (policy_family == "page_preallocation_policy") { return "suppress live preallocation"; }
  if (policy_family == "page_relocation_policy") { return "deny relocation; publish blockers only"; }
  return "fail closed";
}

std::string ActivationActionMode(AgentActivationProfile activation) {
  switch (activation) {
    case AgentActivationProfile::disabled: return "disabled";
    case AgentActivationProfile::observe_only: return "observe_only";
    case AgentActivationProfile::recommend_only: return "recommend_only";
    case AgentActivationProfile::dry_run: return "direct_bounded";
    case AgentActivationProfile::live_action: return "live_action";
  }
  return "disabled";
}

std::string BaselineDefaultModeForPolicyFamily(const std::string& policy_family) {
  if (policy_family == "node_resource_policy" ||
      policy_family == "distributed_query_policy") {
    return "observe_only";
  }
  if (policy_family == "metric_registry_policy" ||
      policy_family == "memory_governor_policy" ||
      policy_family == "admission_control_policy" ||
      policy_family == "cleanup_archive_policy" ||
      policy_family == "archive_policy" ||
      policy_family == "alerting_baseline") {
    return "direct_bounded";
  }
  if (policy_family == "storage_health_policy" ||
      policy_family == "parser_health_policy" ||
      policy_family == "support_bundle_policy" ||
      policy_family == "backup_policy" ||
      policy_family == "pitr_policy") {
    return "request_action";
  }
  if (policy_family == "index_health_policy" ||
      policy_family == "cluster_autoscale_policy" ||
      policy_family == "long_transaction_policy" ||
      policy_family == "policy_recommendation_policy" ||
      policy_family == "remote_routing_policy" ||
      policy_family == "optimizer_learning_policy" ||
      policy_family == "filespace_capacity_policy" ||
      policy_family == "filespace_shadow_promotion_policy" ||
      policy_family == "page_preallocation_policy" ||
      policy_family == "page_relocation_policy") {
    return "recommend_only";
  }
  if (policy_family == "job_control_policy" ||
      policy_family == "restore_drill_policy" ||
      policy_family == "identity_lifecycle_policy" ||
      policy_family == "session_control_policy" ||
      policy_family == "upgrade_policy") {
    return "operator_only";
  }
  if (policy_family == "export_default_baseline") {
    return "disabled";
  }
  return "recommend_only";
}

AgentActivationProfile ActivationForBaselineDefaultMode(const std::string& mode) {
  if (mode == "observe_only") { return AgentActivationProfile::observe_only; }
  if (mode == "recommend_only" || mode == "request_action") {
    return AgentActivationProfile::recommend_only;
  }
  if (mode == "direct_bounded") { return AgentActivationProfile::dry_run; }
  if (mode == "disabled" || mode == "operator_only") {
    return AgentActivationProfile::disabled;
  }
  return AgentActivationProfile::recommend_only;
}

u64 Fnv1a64(const std::string& input, u64 seed) {
  u64 hash = seed;
  for (unsigned char ch : input) {
    hash ^= static_cast<u64>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string StableUuidFromKey(const std::string& key) {
  const u64 a = Fnv1a64(key, 14695981039346656037ull);
  const u64 b = Fnv1a64(key, 1099511628211ull);
  std::ostringstream out;
  out << std::hex << std::setfill('0')
      << std::setw(8) << static_cast<unsigned>((a >> 32) & 0xffffffffull) << '-'
      << std::setw(4) << static_cast<unsigned>((a >> 16) & 0xffffull) << '-'
      << std::setw(4) << static_cast<unsigned>(0x7000u | (a & 0x0fffull)) << '-'
      << std::setw(4) << static_cast<unsigned>(0x8000u | ((b >> 48) & 0x3fffull)) << '-'
      << std::setw(12) << (b & 0xffffffffffffull);
  return out.str();
}

std::string DurableUuidFromKey(platform::UuidKind kind, const std::string& key) {
  const auto candidate = StableUuidFromKey(key);
  const auto parsed = uuid::ParseDurableEngineIdentityUuid(kind, candidate);
  return parsed.ok() ? candidate : std::string{};
}

std::string DurableObjectUuidFromKey(const std::string& key) {
  return DurableUuidFromKey(platform::UuidKind::object, key);
}

std::string DurablePrincipalUuidFromKey(const std::string& key) {
  return DurableUuidFromKey(platform::UuidKind::principal, key);
}

std::string DurableDatabaseUuidFromKey(const std::string& key) {
  return DurableUuidFromKey(platform::UuidKind::database, key);
}

const std::string* FindInput(const AgentActionRequest& action, const std::string& key) {
  const auto found = action.inputs.find(key);
  if (found == action.inputs.end()) { return nullptr; }
  return &found->second;
}

std::string InputOr(const AgentActionRequest& action,
                    const std::string& key,
                    std::string fallback = {}) {
  const auto* value = FindInput(action, key);
  return value == nullptr ? std::move(fallback) : *value;
}

bool IsFalseValue(const std::string& value) {
  return value == "0" || value == "false" || value == "failed" ||
         value == "no" || value == "blocked";
}

u64 ParseU64Input(const AgentActionRequest& action,
                  const std::string& key,
                  u64 fallback = 0) {
  const auto* value = FindInput(action, key);
  if (value == nullptr || value->empty()) { return fallback; }
  try {
    return static_cast<u64>(std::stoull(*value));
  } catch (...) {
    return fallback;
  }
}

AgentArbitrationActionClass ParseArbitrationActionClass(const std::string& value,
                                                        AgentActionClass fallback) {
  if (value == "protect_correctness") { return AgentArbitrationActionClass::protect_correctness; }
  if (value == "protect_security") { return AgentArbitrationActionClass::protect_security; }
  if (value == "protect_durability") { return AgentArbitrationActionClass::protect_durability; }
  if (value == "protect_availability") { return AgentArbitrationActionClass::protect_availability; }
  if (value == "reduce_pressure") { return AgentArbitrationActionClass::reduce_pressure; }
  if (value == "optimize_performance") { return AgentArbitrationActionClass::optimize_performance; }
  if (value == "reduce_cost") { return AgentArbitrationActionClass::reduce_cost; }
  switch (fallback) {
    case AgentActionClass::refusal:
      return AgentArbitrationActionClass::protect_correctness;
    case AgentActionClass::override_action:
      return AgentArbitrationActionClass::protect_security;
    case AgentActionClass::direct_bounded_action:
    case AgentActionClass::request_action:
      return AgentArbitrationActionClass::reduce_pressure;
    case AgentActionClass::recommendation:
    case AgentActionClass::dry_run:
      return AgentArbitrationActionClass::optimize_performance;
    case AgentActionClass::manual_approval_required:
    case AgentActionClass::none:
      return AgentArbitrationActionClass::reduce_cost;
  }
  return AgentArbitrationActionClass::reduce_pressure;
}

AgentArbitrationRisk ParseArbitrationRisk(const std::string& value) {
  if (value == "low") { return AgentArbitrationRisk::low; }
  if (value == "high") { return AgentArbitrationRisk::high; }
  if (value == "critical") { return AgentArbitrationRisk::critical; }
  return AgentArbitrationRisk::medium;
}

AgentArbitrationReversibility ParseArbitrationReversibility(const std::string& value) {
  if (value == "bounded_reversible") {
    return AgentArbitrationReversibility::bounded_reversible;
  }
  if (value == "irreversible") { return AgentArbitrationReversibility::irreversible; }
  return AgentArbitrationReversibility::reversible;
}

bool ScopeOverlaps(const std::string& left, const std::string& right) {
  if (left.empty() || right.empty()) { return true; }
  if (left == right) { return true; }
  const auto left_prefix = left + "/";
  const auto right_prefix = right + "/";
  return right.rfind(left_prefix, 0) == 0 || left.rfind(right_prefix, 0) == 0;
}

bool CandidateTouchesForbiddenOverrideAuthority(const AgentArbitrationCandidate& candidate) {
  const std::string authority = candidate.scope_uuid + "|" + candidate.actuator_id + "|" +
                                candidate.operation_id + "|" + candidate.agent_type_id;
  return authority.find("transaction") != std::string::npos ||
         authority.find("|tx") != std::string::npos ||
         authority.find("catalog") != std::string::npos ||
         authority.find("security") != std::string::npos ||
         authority.find("identity") != std::string::npos ||
         authority.find("cluster") != std::string::npos;
}

bool OverrideActionTokenMatches(const std::string& token,
                                const AgentArbitrationCandidate& candidate) {
  return token == candidate.action_uuid ||
         token == candidate.actuator_id ||
         token == candidate.operation_id ||
         token == candidate.actuator_id + ":" + candidate.operation_id ||
         token == AgentArbitrationActionClassName(candidate.action_class);
}

bool OverrideListsAction(const std::vector<std::string>& tokens,
                         const AgentArbitrationCandidate& candidate) {
  for (const auto& token : tokens) {
    if (OverrideActionTokenMatches(token, candidate)) { return true; }
  }
  return false;
}

bool OverrideIsActiveAt(const AgentArbitrationOverride& override_record, u64 now_microseconds) {
  return override_record.active &&
         (override_record.expires_at_microseconds == 0 ||
          override_record.expires_at_microseconds > now_microseconds);
}

std::string ArbitrationEvidenceUuid(const AgentRuntimeContext& context,
                                    const std::string& reason,
                                    const std::vector<std::string>& action_uuids) {
  return "agent-arbitration-evidence:" +
         StableUuidFromKey(context.database_uuid + "|" + reason + "|" +
                           Join(action_uuids, ",") + "|" +
                           std::to_string(context.wall_now_microseconds));
}

std::string FirstPolicyUuid(const std::vector<AgentArbitrationCandidate>& candidates) {
  for (const auto& candidate : candidates) {
    if (!candidate.policy_uuid.empty()) { return candidate.policy_uuid; }
  }
  return {};
}

const AgentArbitrationCandidate* FindCandidateByUuid(
    const std::vector<AgentArbitrationCandidate>& candidates,
    const std::string& action_uuid) {
  for (const auto& candidate : candidates) {
    if (candidate.action_uuid == action_uuid) { return &candidate; }
  }
  return nullptr;
}

std::vector<std::string> CandidateUuids(const std::vector<AgentArbitrationCandidate>& candidates) {
  std::vector<std::string> out;
  for (const auto& candidate : candidates) { out.push_back(candidate.action_uuid); }
  return out;
}

bool IsNonClusterRuntimeAgent(const AgentTypeDescriptor& descriptor) {
  return !descriptor.cluster_only && descriptor.deployment != AgentDeployment::cluster &&
         (descriptor.deployment == AgentDeployment::local ||
          descriptor.deployment == AgentDeployment::both);
}

const AgentPolicy* FindExplicitPolicyForAgent(
    const AgentTypeDescriptor& descriptor,
    const AgentTickHealthRequest& request) {
  const auto families = RequiredPolicyFamiliesForAgent(descriptor);
  for (const auto& policy : request.policies) {
    if (!Contains(families, policy.policy_family)) { continue; }
    if (policy.scope != descriptor.scope) { continue; }
    return &policy;
  }
  return nullptr;
}

AgentTickHealthRecord BaseTickHealthRecord(const AgentTypeDescriptor& descriptor,
                                           const AgentPolicy& policy,
                                           const AgentRuntimeContext& context,
                                           AgentTickHealthClass tick_class,
                                           std::string diagnostic_code,
                                           std::string detail) {
  AgentTickHealthRecord record;
  record.agent_type_id = descriptor.type_id;
  record.deployment = descriptor.deployment;
  record.policy_uuid = policy.policy_uuid;
  record.tick_class = tick_class;
  record.diagnostic_code = std::move(diagnostic_code);
  record.detail = std::move(detail);
  const std::string evidence_key =
      context.database_uuid + "|" + descriptor.type_id + "|" +
      AgentTickHealthClassName(record.tick_class) + "|" +
      std::to_string(context.wall_now_microseconds) + "|" +
      std::to_string(policy.policy_generation);
  record.health_evidence_uuid = DurableObjectUuidFromKey("agent_tick_health|" + evidence_key);
  record.action_evidence_uuid = DurableObjectUuidFromKey("agent_tick_action|" + evidence_key + "|action");
  record.tick_produced = true;
  record.health_published = true;
  return record;
}

AgentTickHealthRecord FailedClosedTickHealthRecord(const AgentTypeDescriptor& descriptor,
                                                   const AgentPolicy& policy,
                                                   const AgentRuntimeContext& context,
                                                   const AgentRuntimeStatus& status,
                                                   std::vector<AgentDependencyDiagnostic> diagnostics = {}) {
  auto record = BaseTickHealthRecord(descriptor, policy, context,
                                     AgentTickHealthClass::failed_closed,
                                     status.diagnostic_code, status.detail);
  record.lifecycle_state = AgentLifecycleState::failed;
  record.action_class = AgentActionClass::refusal;
  record.action_result_class = AgentActionResultClass::failed_closed;
  record.action_evidence_published = true;
  record.failed_closed = true;
  record.dependency_diagnostics = std::move(diagnostics);
  return record;
}

AgentTickHealthRecord ResourceBudgetTickHealthRecord(
    const AgentTypeDescriptor& descriptor,
    const AgentPolicy& policy,
    const AgentRuntimeContext& context,
    const AgentResourceBudgetDecision& decision) {
  const bool failed_closed =
      decision.decision == AgentResourceBudgetDecisionKind::fail_closed;
  auto record = BaseTickHealthRecord(
      descriptor, policy, context,
      failed_closed ? AgentTickHealthClass::failed_closed
                    : AgentTickHealthClass::suppressed,
      decision.status.diagnostic_code,
      decision.status.detail);
  record.selected = true;
  record.runnable = false;
  record.failed_closed = failed_closed;
  record.suppressed = !failed_closed;
  record.resource_budget_limited = true;
  record.lifecycle_state =
      decision.decision == AgentResourceBudgetDecisionKind::cancel_drain
          ? AgentLifecycleState::stopping
          : AgentLifecycleState::paused;
  record.action_class = AgentActionClass::refusal;
  record.action_result_class = failed_closed
      ? AgentActionResultClass::failed_closed
      : AgentActionResultClass::suppressed;
  if (decision.decision == AgentResourceBudgetDecisionKind::shed_refuse ||
      decision.decision == AgentResourceBudgetDecisionKind::foreground_protection) {
    record.action_result_class = AgentActionResultClass::refused;
  }
  record.action_evidence_published = true;
  record.resource_budget_diagnostics = decision.diagnostics;
  if (!decision.evidence_uuid.empty()) {
    record.action_evidence_uuid = decision.evidence_uuid;
  }
  return record;
}

const AgentMetricObservation* FindMetricObservation(
    const std::vector<AgentMetricObservation>& observations,
    const std::string& family) {
  for (const auto& observation : observations) {
    if (observation.metric_family == family) { return &observation; }
  }
  return nullptr;
}

const AgentPolicyDependencyState* FindPolicyDependencyState(
    const std::vector<AgentPolicyDependencyState>& policy_state,
    const std::string& policy_family) {
  for (const auto& state : policy_state) {
    if (state.policy_family == policy_family) { return &state; }
  }
  return nullptr;
}

std::string DependencyEvidenceUuid(const AgentTypeDescriptor& descriptor,
                                   const std::string& subject_kind,
                                   const std::string& subject,
                                   const std::string& diagnostic_code,
                                   const std::string& explicit_evidence_uuid) {
  if (!explicit_evidence_uuid.empty()) { return explicit_evidence_uuid; }
  return DurableObjectUuidFromKey("agent_dependency_evidence|" + descriptor.type_id + "|" +
                                  subject_kind + "|" + subject + "|" + diagnostic_code);
}

AgentDependencyDiagnostic MakeDependencyDiagnostic(
    const AgentTypeDescriptor& descriptor,
    std::string diagnostic_code,
    std::string subject_kind,
    std::string subject,
    std::string evidence_uuid,
    std::string snapshot_id = {},
    bool optional_suppressed = false,
    bool failed_closed = true) {
  AgentDependencyDiagnostic diagnostic;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.subject_kind = std::move(subject_kind);
  diagnostic.subject = std::move(subject);
  diagnostic.evidence_uuid = DependencyEvidenceUuid(
      descriptor, diagnostic.subject_kind, diagnostic.subject,
      diagnostic.diagnostic_code, std::move(evidence_uuid));
  diagnostic.snapshot_id = std::move(snapshot_id);
  diagnostic.optional_suppressed = optional_suppressed;
  diagnostic.failed_closed = failed_closed;
  return diagnostic;
}

void AttachMetricContractDiagnosticFields(AgentDependencyDiagnostic* diagnostic,
                                          const AgentMetricDependency& dep) {
  if (diagnostic == nullptr) { return; }
  diagnostic->namespace_prefix = dep.namespace_prefix;
  diagnostic->policy_field = dep.policy_field;
  diagnostic->decision_use = dep.decision_use;
  diagnostic->fail_behavior = dep.fail_behavior;
  diagnostic->dependency_evidence_field = dep.evidence_field;
}

AgentDependencyDiagnostic MakeMetricDependencyDiagnostic(
    const AgentTypeDescriptor& descriptor,
    std::string diagnostic_code,
    const AgentMetricDependency& dep,
    std::string evidence_uuid,
    std::string snapshot_id = {},
    bool optional_suppressed = false,
    bool failed_closed = true) {
  AgentDependencyDiagnostic diagnostic = MakeDependencyDiagnostic(
      descriptor,
      std::move(diagnostic_code),
      "metric",
      dep.metric_family,
      std::move(evidence_uuid),
      std::move(snapshot_id),
      optional_suppressed,
      failed_closed);
  AttachMetricContractDiagnosticFields(&diagnostic, dep);
  return diagnostic;
}

AgentDependencyEvaluation FailDependencyEvaluation(
    const AgentTypeDescriptor& descriptor,
    std::string code,
    std::string subject_kind,
    std::string subject,
    std::string evidence_uuid,
    std::string snapshot_id = {}) {
  AgentDependencyEvaluation evaluation;
  evaluation.status = AgentError(code, subject);
  evaluation.failed_closed = true;
  evaluation.diagnostics.push_back(MakeDependencyDiagnostic(
      descriptor, std::move(code), std::move(subject_kind), std::move(subject),
      std::move(evidence_uuid), std::move(snapshot_id), false, true));
  return evaluation;
}

AgentDependencyEvaluation FailMetricDependencyEvaluation(
    const AgentTypeDescriptor& descriptor,
    std::string code,
    const AgentMetricDependency& dep,
    std::string evidence_uuid,
    std::string snapshot_id = {}) {
  AgentDependencyEvaluation evaluation;
  evaluation.status = AgentError(code, dep.metric_family);
  evaluation.failed_closed = true;
  evaluation.diagnostics.push_back(MakeMetricDependencyDiagnostic(
      descriptor, std::move(code), dep, std::move(evidence_uuid),
      std::move(snapshot_id), false, true));
  return evaluation;
}

void AddOptionalSuppressedDiagnostic(AgentDependencyEvaluation* evaluation,
                                     const AgentTypeDescriptor& descriptor,
                                     const AgentMetricDependency& dep,
                                     const AgentMetricObservation* observation) {
  if (evaluation == nullptr) { return; }
  evaluation->optional_suppressed = true;
  evaluation->diagnostics.push_back(MakeMetricDependencyDiagnostic(
      descriptor,
      "SB_AGENT_METRICS.OPTIONAL_METRIC_SUPPRESSED",
      dep,
      observation == nullptr ? std::string{} : observation->evidence_uuid,
      observation == nullptr ? std::string{} : observation->snapshot_id,
      true,
      false));
}

AgentRuntimeStatus ResolveNonClusterTickMetricDependencies(
    const AgentTypeDescriptor& descriptor,
    const AgentTickHealthRequest& request,
    const scratchbird::core::metrics::MetricRegistry& registry,
    bool* cluster_path_failed_closed,
    std::vector<AgentDependencyDiagnostic>* dependency_diagnostics) {
  bool local_dependency_seen = false;
  bool cluster_dependency_seen = false;
  if (cluster_path_failed_closed != nullptr) { *cluster_path_failed_closed = false; }
  if (dependency_diagnostics != nullptr) { dependency_diagnostics->clear(); }

  if (request.enforce_metric_observation_dependencies ||
      request.enforce_policy_dependency_state) {
    const auto evaluation = EvaluateAgentMetricDependencies(
        descriptor,
        request.context,
        request.metric_observations,
        request.policy_dependency_states,
        request.enforce_policy_dependency_state,
        registry);
    if (cluster_path_failed_closed != nullptr) {
      *cluster_path_failed_closed = evaluation.cluster_path_failed_closed;
    }
    if (dependency_diagnostics != nullptr) {
      *dependency_diagnostics = evaluation.diagnostics;
    }
    return evaluation.status;
  }

  for (const auto& dep : descriptor.metric_dependencies) {
    const auto* metric = registry.FindDescriptorOrAlias(dep.metric_family);
    if (dep.cluster_only) {
      cluster_dependency_seen = true;
      if (!request.context.cluster_authority_available &&
          cluster_path_failed_closed != nullptr) {
        *cluster_path_failed_closed = true;
      }
      continue;
    }
    local_dependency_seen = true;
    if (dep.required && metric == nullptr) {
      return AgentError("SB_AGENT_METRICS.REQUIRED_METRIC_MISSING", dep.metric_family);
    }
    if (Contains(request.missing_metric_families, dep.metric_family)) {
      return AgentError("SB_AGENT_METRICS.REQUIRED_METRIC_MISSING", dep.metric_family);
    }
    if (metric != nullptr && !dep.namespace_prefix.empty() &&
        metric->namespace_path.rfind(dep.namespace_prefix, 0) != 0) {
      return AgentError("SB_AGENT_METRICS.NAMESPACE_MISMATCH", dep.metric_family);
    }
  }

  if (!local_dependency_seen && cluster_dependency_seen &&
      !request.context.cluster_authority_available) {
    return AgentError("SB_AGENT_METRICS.CLUSTER_AUTHORITY_REQUIRED", descriptor.type_id);
  }
  return AgentOk();
}

AgentTickHealthRecord ClassifyRunnableTickHealthRecord(
    const AgentTypeDescriptor& descriptor,
    const AgentPolicy& policy,
    const AgentRuntimeContext& context,
    bool cluster_path_failed_closed) {
  const bool operator_only = policy.enabled &&
      policy.activation == AgentActivationProfile::disabled &&
      policy.action_mode == "operator_only";
  if (operator_only) {
    auto record = BaseTickHealthRecord(
        descriptor, policy, context,
        AgentTickHealthClass::manual_approval_operator_only,
        "SB_AGENT_TICK_HEALTH.OPERATOR_ONLY",
        descriptor.type_id);
    record.selected = true;
    record.runnable = false;
    record.failed_closed = false;
    record.manual_approval_required = true;
    record.lifecycle_state = AgentLifecycleState::disabled;
    record.action_class = AgentActionClass::manual_approval_required;
    record.action_result_class = AgentActionResultClass::approval_required;
    record.action_evidence_published = true;
    record.cluster_path_failed_closed = cluster_path_failed_closed;
    return record;
  }

  if (!policy.enabled || policy.activation == AgentActivationProfile::disabled ||
      policy.action_mode == "disabled") {
    auto record = BaseTickHealthRecord(
        descriptor, policy, context,
        AgentTickHealthClass::policy_disabled,
        "SB_AGENT_TICK_HEALTH.POLICY_DISABLED",
        descriptor.type_id);
    record.policy_disabled = true;
    record.lifecycle_state = AgentLifecycleState::disabled;
    record.action_class = AgentActionClass::refusal;
    record.action_result_class = AgentActionResultClass::refused;
    record.action_evidence_published = true;
    record.failed_closed = false;
    record.cluster_path_failed_closed = cluster_path_failed_closed;
    return record;
  }

  if (policy.activation == AgentActivationProfile::observe_only) {
    auto record = BaseTickHealthRecord(
        descriptor, policy, context,
        AgentTickHealthClass::observe_only,
        "SB_AGENT_TICK_HEALTH.OBSERVE_ONLY",
        descriptor.type_id);
    record.selected = true;
    record.runnable = true;
    record.failed_closed = false;
    record.lifecycle_state = AgentLifecycleState::observe_only;
    record.action_class = AgentActionClass::none;
    record.action_result_class = AgentActionResultClass::accepted;
    record.cluster_path_failed_closed = cluster_path_failed_closed;
    return record;
  }

  if (policy.activation == AgentActivationProfile::recommend_only) {
    auto record = BaseTickHealthRecord(
        descriptor, policy, context,
        AgentTickHealthClass::recommend_only,
        "SB_AGENT_TICK_HEALTH.RECOMMEND_ONLY",
        descriptor.type_id);
    record.selected = true;
    record.runnable = true;
    record.failed_closed = false;
    record.lifecycle_state = AgentLifecycleState::recommend_only;
    record.action_class = AgentActionClass::recommendation;
    record.action_result_class = AgentActionResultClass::accepted;
    record.action_evidence_published = true;
    record.cluster_path_failed_closed = cluster_path_failed_closed;
    return record;
  }

  if (policy.activation == AgentActivationProfile::dry_run ||
      !policy.allow_live_action) {
    auto record = BaseTickHealthRecord(
        descriptor, policy, context,
        AgentTickHealthClass::dry_run,
        "SB_AGENT_TICK_HEALTH.DRY_RUN_ONLY",
        descriptor.type_id);
    record.selected = true;
    record.runnable = true;
    record.failed_closed = false;
    record.lifecycle_state = AgentLifecycleState::dry_run;
    record.action_class = AgentActionClass::dry_run;
    record.action_result_class = AgentActionResultClass::dry_run_only;
    record.action_evidence_published = true;
    record.cluster_path_failed_closed = cluster_path_failed_closed;
    return record;
  }

  auto record = BaseTickHealthRecord(
      descriptor, policy, context,
      AgentTickHealthClass::selected_running,
      "SB_AGENT_TICK_HEALTH.SELECTED_RUNNING",
      descriptor.type_id);
  record.selected = true;
  record.runnable = true;
  record.failed_closed = false;
  record.lifecycle_state = AgentLifecycleState::running;
  record.action_class = descriptor.authority == AgentAuthorityClass::direct_bounded_action
      ? AgentActionClass::direct_bounded_action
      : AgentActionClass::request_action;
  record.action_result_class = AgentActionResultClass::accepted;
  record.action_evidence_published = true;
  record.cluster_path_failed_closed = cluster_path_failed_closed;
  return record;
}

}  // namespace

std::string DeterministicAgentRuntimeObjectUuidFromKey(const std::string& key) {
  return DurableObjectUuidFromKey(key);
}

std::string DeterministicAgentRuntimePrincipalUuidFromKey(const std::string& key) {
  return DurablePrincipalUuidFromKey(key);
}

std::string DeterministicAgentRuntimeDatabaseUuidFromKey(const std::string& key) {
  return DurableDatabaseUuidFromKey(key);
}

AgentRuntimeStatus AgentOk() { return {true, "SB_AGENT_OK", {}}; }
AgentRuntimeStatus AgentError(std::string code, std::string detail) { return {false, std::move(code), std::move(detail)}; }

const char* AgentDeploymentName(AgentDeployment value) {
  switch (value) {
    case AgentDeployment::local: return "local";
    case AgentDeployment::both: return "both";
    case AgentDeployment::cluster: return "cluster";
  }
  return "unknown";
}

const char* AgentAuthorityClassName(AgentAuthorityClass value) {
  switch (value) {
    case AgentAuthorityClass::observe_only: return "observe_only";
    case AgentAuthorityClass::recommend_only: return "recommend_only";
    case AgentAuthorityClass::request_action: return "request_action";
    case AgentAuthorityClass::direct_bounded_action: return "direct_bounded_action";
  }
  return "unknown";
}

const char* AgentLifecycleStateName(AgentLifecycleState value) {
  switch (value) {
    case AgentLifecycleState::created: return "created";
    case AgentLifecycleState::registered: return "registered";
    case AgentLifecycleState::disabled: return "disabled";
    case AgentLifecycleState::observe_only: return "observe_only";
    case AgentLifecycleState::recommend_only: return "recommend_only";
    case AgentLifecycleState::dry_run: return "dry_run";
    case AgentLifecycleState::running: return "running";
    case AgentLifecycleState::paused: return "paused";
    case AgentLifecycleState::safe_mode: return "safe_mode";
    case AgentLifecycleState::quarantined: return "quarantined";
    case AgentLifecycleState::stopping: return "stopping";
    case AgentLifecycleState::stopped: return "stopped";
    case AgentLifecycleState::retired: return "retired";
    case AgentLifecycleState::failed: return "failed";
  }
  return "unknown";
}

const char* AgentActivationProfileName(AgentActivationProfile value) {
  switch (value) {
    case AgentActivationProfile::disabled: return "disabled";
    case AgentActivationProfile::observe_only: return "observe_only";
    case AgentActivationProfile::recommend_only: return "recommend_only";
    case AgentActivationProfile::dry_run: return "dry_run";
    case AgentActivationProfile::live_action: return "live_action";
  }
  return "unknown";
}

const char* AgentActionClassName(AgentActionClass value) {
  switch (value) {
    case AgentActionClass::none: return "none";
    case AgentActionClass::recommendation: return "recommendation";
    case AgentActionClass::request_action: return "request_action";
    case AgentActionClass::direct_bounded_action: return "direct_bounded_action";
    case AgentActionClass::dry_run: return "dry_run";
    case AgentActionClass::refusal: return "refusal";
    case AgentActionClass::override_action: return "override_action";
    case AgentActionClass::manual_approval_required: return "manual_approval_required";
  }
  return "unknown";
}

const char* AgentActionResultClassName(AgentActionResultClass value) {
  switch (value) {
    case AgentActionResultClass::accepted: return "accepted";
    case AgentActionResultClass::refused: return "refused";
    case AgentActionResultClass::suppressed: return "suppressed";
    case AgentActionResultClass::dry_run_only: return "dry_run_only";
    case AgentActionResultClass::approval_required: return "approval_required";
    case AgentActionResultClass::failed_closed: return "failed_closed";
    case AgentActionResultClass::quarantined: return "quarantined";
  }
  return "unknown";
}

const char* AgentLifecycleModeName(AgentLifecycleMode value) {
  switch (value) {
    case AgentLifecycleMode::normal: return "normal";
    case AgentLifecycleMode::database_create: return "database_create";
    case AgentLifecycleMode::database_open: return "database_open";
    case AgentLifecycleMode::database_close: return "database_close";
    case AgentLifecycleMode::backup: return "backup";
    case AgentLifecycleMode::restore: return "restore";
    case AgentLifecycleMode::shutdown: return "shutdown";
    case AgentLifecycleMode::crash_recovery: return "crash_recovery";
    case AgentLifecycleMode::restricted_open: return "restricted_open";
    case AgentLifecycleMode::read_only: return "read_only";
    case AgentLifecycleMode::maintenance: return "maintenance";
    case AgentLifecycleMode::repair: return "repair";
    case AgentLifecycleMode::archive_hold: return "archive_hold";
    case AgentLifecycleMode::pitr: return "pitr";
    case AgentLifecycleMode::clone: return "clone";
    case AgentLifecycleMode::role_change: return "role_change";
  }
  return "unknown";
}

const char* AgentFeatureAvailabilityName(AgentFeatureAvailability value) {
  switch (value) {
    case AgentFeatureAvailability::available: return "available";
    case AgentFeatureAvailability::unavailable_disabled_stub: return "unavailable_disabled_stub";
    case AgentFeatureAvailability::unavailable_edition: return "unavailable_edition";
    case AgentFeatureAvailability::unavailable_cluster_authority: return "unavailable_cluster_authority";
    case AgentFeatureAvailability::unavailable_private_feature: return "unavailable_private_feature";
  }
  return "unknown";
}

const char* AgentTickHealthClassName(AgentTickHealthClass value) {
  switch (value) {
    case AgentTickHealthClass::selected_running: return "selected_running";
    case AgentTickHealthClass::observe_only: return "observe_only";
    case AgentTickHealthClass::recommend_only: return "recommend_only";
    case AgentTickHealthClass::dry_run: return "dry_run";
    case AgentTickHealthClass::policy_disabled: return "policy_disabled";
    case AgentTickHealthClass::suppressed: return "suppressed";
    case AgentTickHealthClass::manual_approval_operator_only:
      return "manual_approval_operator_only";
    case AgentTickHealthClass::failed_closed: return "failed_closed";
  }
  return "unknown";
}

const char* AgentSupervisionFailureKindName(AgentSupervisionFailureKind value) {
  switch (value) {
    case AgentSupervisionFailureKind::tick_timeout: return "tick_timeout";
    case AgentSupervisionFailureKind::watchdog_timeout: return "watchdog_timeout";
    case AgentSupervisionFailureKind::exception: return "exception";
    case AgentSupervisionFailureKind::runtime_timeout: return "runtime_timeout";
    case AgentSupervisionFailureKind::action_failed: return "action_failed";
  }
  return "unknown";
}

const char* AgentResourceBudgetDecisionKindName(
    AgentResourceBudgetDecisionKind value) {
  switch (value) {
    case AgentResourceBudgetDecisionKind::allow: return "allow";
    case AgentResourceBudgetDecisionKind::throttle_defer: return "throttle_defer";
    case AgentResourceBudgetDecisionKind::shed_refuse: return "shed_refuse";
    case AgentResourceBudgetDecisionKind::fail_closed: return "fail_closed";
    case AgentResourceBudgetDecisionKind::cancel_drain: return "cancel_drain";
    case AgentResourceBudgetDecisionKind::foreground_protection:
      return "foreground_protection";
  }
  return "unknown";
}

const char* AgentResourceBudgetDimensionName(AgentResourceBudgetDimension value) {
  switch (value) {
    case AgentResourceBudgetDimension::foreground_protection:
      return "foreground_protection";
    case AgentResourceBudgetDimension::cpu_time: return "cpu_time";
    case AgentResourceBudgetDimension::memory_bytes: return "memory_bytes";
    case AgentResourceBudgetDimension::io_bytes: return "io_bytes";
    case AgentResourceBudgetDimension::io_ops: return "io_ops";
    case AgentResourceBudgetDimension::thread_slots: return "thread_slots";
    case AgentResourceBudgetDimension::queue_depth: return "queue_depth";
    case AgentResourceBudgetDimension::cadence: return "cadence";
    case AgentResourceBudgetDimension::retry_backoff: return "retry_backoff";
    case AgentResourceBudgetDimension::runtime_timeout: return "runtime_timeout";
    case AgentResourceBudgetDimension::cancellation_drain:
      return "cancellation_drain";
    case AgentResourceBudgetDimension::history_rows: return "history_rows";
    case AgentResourceBudgetDimension::evidence_fanout: return "evidence_fanout";
    case AgentResourceBudgetDimension::label_cardinality:
      return "label_cardinality";
  }
  return "unknown";
}

const char* AgentArbitrationActionClassName(AgentArbitrationActionClass value) {
  switch (value) {
    case AgentArbitrationActionClass::protect_correctness: return "protect_correctness";
    case AgentArbitrationActionClass::protect_security: return "protect_security";
    case AgentArbitrationActionClass::protect_durability: return "protect_durability";
    case AgentArbitrationActionClass::protect_availability: return "protect_availability";
    case AgentArbitrationActionClass::reduce_pressure: return "reduce_pressure";
    case AgentArbitrationActionClass::optimize_performance: return "optimize_performance";
    case AgentArbitrationActionClass::reduce_cost: return "reduce_cost";
  }
  return "unknown";
}

const char* AgentArbitrationRiskName(AgentArbitrationRisk value) {
  switch (value) {
    case AgentArbitrationRisk::low: return "low";
    case AgentArbitrationRisk::medium: return "medium";
    case AgentArbitrationRisk::high: return "high";
    case AgentArbitrationRisk::critical: return "critical";
  }
  return "unknown";
}

const char* AgentArbitrationReversibilityName(AgentArbitrationReversibility value) {
  switch (value) {
    case AgentArbitrationReversibility::reversible: return "reversible";
    case AgentArbitrationReversibility::bounded_reversible: return "bounded_reversible";
    case AgentArbitrationReversibility::irreversible: return "irreversible";
  }
  return "unknown";
}

const char* AgentArbitrationOutcomeName(AgentArbitrationOutcome value) {
  switch (value) {
    case AgentArbitrationOutcome::winner_executes: return "winner_executes";
    case AgentArbitrationOutcome::both_denied: return "both_denied";
    case AgentArbitrationOutcome::operator_review_required: return "operator_review_required";
    case AgentArbitrationOutcome::suppressed_by_override: return "suppressed_by_override";
  }
  return "unknown";
}

const char* AgentArbitrationPriorityRuleName(AgentArbitrationPriorityRule value) {
  switch (value) {
    case AgentArbitrationPriorityRule::no_actions: return "no_actions";
    case AgentArbitrationPriorityRule::safety_precondition_failed: return "safety_precondition_failed";
    case AgentArbitrationPriorityRule::single_action: return "single_action";
    case AgentArbitrationPriorityRule::override_suppression: return "override_suppression";
    case AgentArbitrationPriorityRule::override_right_required: return "override_right_required";
    case AgentArbitrationPriorityRule::override_authority_forbidden: return "override_authority_forbidden";
    case AgentArbitrationPriorityRule::action_class_priority: return "action_class_priority";
    case AgentArbitrationPriorityRule::evidence_quality: return "evidence_quality";
    case AgentArbitrationPriorityRule::exact_tie_operator_review: return "exact_tie_operator_review";
  }
  return "unknown";
}

const char* AgentFaultInjectionClassName(AgentFaultInjectionClass value) {
  switch (value) {
    case AgentFaultInjectionClass::supervision: return "supervision";
    case AgentFaultInjectionClass::storage_io: return "storage_io";
    case AgentFaultInjectionClass::metric_input: return "metric_input";
    case AgentFaultInjectionClass::policy_input: return "policy_input";
    case AgentFaultInjectionClass::queue_integrity: return "queue_integrity";
    case AgentFaultInjectionClass::partial_action: return "partial_action";
  }
  return "unknown";
}

const char* AgentFaultInjectionRecoveryResponseName(
    AgentFaultInjectionRecoveryResponse value) {
  switch (value) {
    case AgentFaultInjectionRecoveryResponse::fail_closed:
      return "fail_closed";
    case AgentFaultInjectionRecoveryResponse::reject_metric_sample:
      return "reject_metric_sample";
    case AgentFaultInjectionRecoveryResponse::reject_policy:
      return "reject_policy";
    case AgentFaultInjectionRecoveryResponse::supervision_restart_backoff:
      return "supervision_restart_backoff";
    case AgentFaultInjectionRecoveryResponse::supervision_quarantine:
      return "supervision_quarantine";
  }
  return "unknown";
}

// SEARCH_KEY: SB_AGENT_METRIC_DEPENDENCY_CONTRACTS
std::vector<AgentMetricDependencyContractRow> AgentMetricDependencyContractRegistry() {
  const auto req = AgentMetricDependencyKind::required;
  const auto opt = AgentMetricDependencyKind::optional;
  const auto cluster_req = AgentMetricDependencyKind::required_for_cluster_scope;
  const auto shrink_req = AgentMetricDependencyKind::required_for_shrink;
  const auto reloc_req = AgentMetricDependencyKind::required_for_relocation;
  const auto trusted = AgentMetricSourceQuality::trusted;
  const auto cluster_confirmed = AgentMetricSourceQuality::cluster_confirmed;
  return {
      ContractRow("node_resource_agent", "sb_cluster_node_cpu_feature_available", "sys.metrics.physical", req, Seconds(60), trusted, "trusted node probe", "latest", "required_cpu_features", "role suitability", "suitability unknown", "cpu_feature_snapshot"),
      ContractRow("metrics_registry_manager", "sb_metric_samples_rejected_total", "sys.metrics.registry", req, Seconds(15), trusted, "trusted counter", "rate 5m", "rejection_rate_limit", "cardinality/backpressure", "reject invalid samples", "rejected_sample_count"),
      ContractRow("storage_health_manager", "sb_storage_fsync_latency_microseconds", "sys.metrics.storage", req, Seconds(15), trusted, "trusted histogram", "p99 over 5m", "fsync_p99_critical_us", "health and optimizer cost", "health unknown", "fsync_p99"),
      ContractRow("storage_health_manager", "sb_storage_unknown_pages_total", "sys.metrics.storage", req, Seconds(5), trusted, "trusted counter", "latest/rate", "unknown_page_quarantine_threshold", "quarantine request", "deny destructive actions", "unknown_page_count"),
      ContractRow("storage_health_manager", "sb_filespace_health_state", "sys.metrics.storage.filespaces", req, Seconds(15), trusted, "trusted state", "latest", "storage_health_policy", "health summary", "health unknown", "storage_health_snapshot"),
      ContractRow("storage_health_manager", "sb_filespace_device_error_total", "sys.metrics.storage.filespaces", req, Seconds(5), trusted, "trusted counter", "rate/latest", "device_error_threshold", "health escalation", "destructive actions denied", "device_error_snapshot"),
      ContractRow("storage_health_manager", "sb_page_allocation_failures_total", "sys.metrics.storage.pages", opt, Seconds(15), trusted, "trusted counter", "rate 5m", "storage_health_policy", "allocator-health summary", "page health unknown", "page_allocation_failures"),
      ContractRow("memory_governor", "sb_memory_emergency_reserve_bytes", "sys.metrics.memory", req, Seconds(5), trusted, "trusted gauge", "latest plus 30s min", "emergency_reserve_percent", "grant denial/spill", "deny large grants", "reserve_bytes_snapshot"),
      ContractRow("memory_governor", "sb_memory_allocation_failures_total", "sys.metrics.memory", req, Seconds(5), trusted, "trusted counter", "rate 1m", "allocation_failure_pressure", "reserve protection", "conservative grants", "allocation_failure_rate"),
      ContractRow("index_health_manager", "sb_index_read_amplification_ratio", "sys.metrics.indexes", req, Seconds(300), trusted, "trusted gauge", "p95/window", "rebuild_threshold", "rebuild recommendation", "suppress recommendation", "read_amplification_ratio"),
      ContractRow("index_health_manager", "sb_index_splits_total", "sys.metrics.indexes", opt, Seconds(300), trusted, "trusted counter", "rate 1h", "split_pressure_threshold", "rebalance recommendation", "no split-based recommendation", "split_rate"),
      ContractRow("cluster_autoscale_manager", "sb_cluster_node_saturation_ratio", "cluster.sys.metrics.autoscale", req, Seconds(30), cluster_confirmed, "cluster confirmed", "max/window 5m", "scale_up_threshold", "scale recommendation", "scale-down denied", "saturation_snapshot"),
      ContractRow("cluster_autoscale_manager", "sb_idle_capacity_ratio", "cluster.sys.metrics.autoscale", req, Seconds(60), cluster_confirmed, "cluster confirmed", "min/window 15m", "scale_down_threshold", "scale-down recommendation", "scale-down denied", "idle_capacity_ratio"),
      ContractRow("admission_control_manager", "sb_listener_queue_depth", "sys.metrics.listener", req, Seconds(5), trusted, "trusted gauge", "latest/p95 1m", "listener_queue_threshold", "throttle/deny", "conservative throttle", "listener_queue_depth"),
      ContractRow("admission_control_manager", "sb_scheduler_queue_depth", "sys.metrics.scheduler", req, Seconds(5), trusted, "trusted gauge", "latest/p95 1m", "scheduler_queue_threshold", "admission throttle", "conservative throttle", "scheduler_queue_depth"),
      ContractRow("parser_interface_manager", "sb_parser_crashes_total", "sys.metrics.parser", req, Seconds(15), trusted, "trusted event stream", "rate 5m", "parser_crash_quarantine_threshold", "drain/quarantine", "deny new parser assignment", "parser_crash_rate"),
      ContractRow("parser_interface_manager", "sb_parser_policy_attach_latency_microseconds", "sys.metrics.parser", opt, Seconds(60), trusted, "trusted histogram", "p95 5m", "policy_attach_latency_warn_us", "recommendation", "ignore optional input", "attach_latency_p95"),
      ContractRow("transaction_pressure_manager", "sb_tx_oldest_snapshot_age_microseconds", "sys.metrics.transactions", req, Seconds(5), trusted, "trusted gauge", "latest", "oldest_snapshot_pressure_seconds", "warnings/recommendations", "warning only", "oldest_snapshot_age"),
      ContractRow("transaction_pressure_manager", "sb_cluster_limbo_transactions", "cluster.sys.metrics.transactions", cluster_req, Seconds(10), cluster_confirmed, "cluster confirmed", "latest", "limbo_pressure_threshold", "deny cleanup/route recommendations", "cluster recommendations disabled", "limbo_count"),
      ContractRow("storage_version_cleanup_agent", "sb_mga_cleanup_horizon_local_transaction_id", "sys.metrics.mga.cleanup", req, Seconds(10), trusted, "trusted gauge", "latest", "storage_version_cleanup_policy", "bounded cleanup batch", "deny cleanup", "cleanup_horizon"),
      ContractRow("storage_version_cleanup_agent", "sb_mga_cleanup_retained_row_versions", "sys.metrics.mga.cleanup", req, Seconds(10), trusted, "trusted gauge", "latest", "retained_row_pressure", "before/after pressure", "deny cleanup", "retained_versions"),
      ContractRow("storage_version_cleanup_agent", "sb_mga_cleanup_blocked_total", "sys.metrics.mga.cleanup", req, Seconds(10), trusted, "trusted counter", "rate 5m", "active_blocker_threshold", "blocked cleanup diagnostics", "deny cleanup", "blocked_rate"),
      ContractRow("cleanup_archive_manager", "sb_mga_cleanup_blocked_total", "sys.metrics.mga.cleanup", req, Seconds(10), trusted, "trusted counter", "rate 5m", "cleanup_blocked_threshold", "cleanup pressure", "cleanup denied", "cleanup_blocked_rate"),
      ContractRow("cleanup_archive_manager", "sb_archive_slice_age_microseconds", "sys.metrics.archive", req, Seconds(60), trusted, "trusted gauge", "max", "archive_slice_max_age_us", "archive verification/seal", "hold slice", "max_slice_age"),
      ContractRow("policy_recommendation_manager", "sb_workload_slo_burn_rate", "sys.metrics.workloads", req, Seconds(60), trusted, "trusted gauge", "max/window", "recommendation_burn_rate", "policy recommendation", "no recommendation", "slo_burn_rate"),
      ContractRow("distributed_query_metrics_agent", "sb_query_fragment_queue_delay_microseconds", "cluster.sys.metrics.query.fragments", req, Seconds(5), trusted, "trusted histogram", "p95/p99", "fragment_delay_warn_us", "publish fragment health", "fragment unknown", "queue_delay_p95"),
      ContractRow("remote_query_routing_agent", "sb_optimizer_remote_fragment_latency_microseconds", "cluster.sys.metrics.optimizer.remote", req, Seconds(10), trusted, "trusted histogram", "p95/window", "remote_route_latency_weight", "route recommendation", "local fallback", "remote_latency_p95"),
      ContractRow("runtime_learning_agent", "sb_optimizer_plan_estimate_error_ratio", "sys.metrics.optimizer", req, Seconds(300), trusted, "trusted gauge", "p95/window", "correction_threshold", "planner correction", "disable correction", "estimate_error_ratio"),
      ContractRow("support_bundle_triage_agent", "sb_support_bundle_completeness_ratio", "sys.metrics.supportability", req, Seconds(60), trusted, "trusted gauge", "latest", "completeness_required", "bundle readiness", "deny bundle", "completeness_ratio"),
      ContractRow("cluster_scheduler_manager", "sb_cluster_scheduler_queue_depth", "cluster.sys.metrics.scheduler", req, Seconds(10), cluster_confirmed, "cluster confirmed", "latest/p95", "scheduler_queue_threshold", "job placement", "keep queued", "queue_depth"),
      ContractRow("job_control_manager", "sb_job_control_actions_total", "sys.metrics.jobs", req, Seconds(15), trusted, "trusted counter", "event/rate", "job_control_policy", "control audit/suppression", "deny control on audit gap", "job_action_counter"),
      ContractRow("backup_manager", "sb_backup_progress_percent", "sys.metrics.backup", req, Seconds(30), trusted, "trusted gauge", "latest", "stuck_progress_window", "progress/stuck detection", "progress unknown", "backup_percent"),
      ContractRow("archive_manager", "sb_archive_queue_depth", "sys.metrics.archive", req, Seconds(30), trusted, "trusted gauge", "latest/p95", "archive_queue_pressure", "seal/verify/shed", "hold archive", "archive_queue_depth"),
      ContractRow("restore_drill_manager", "sb_restore_drill_duration_microseconds", "sys.metrics.restore", req, Seconds(300), trusted, "trusted histogram", "p95/window", "restore_drill_max_duration_us", "readiness", "deny drill", "drill_duration_p95"),
      ContractRow("pitr_manager", "sb_pitr_window_available_seconds", "sys.metrics.pitr", req, Seconds(60), trusted, "trusted gauge", "latest", "minimum_pitr_window_seconds", "PITR readiness", "report unreachable", "pitr_window_seconds"),
      ContractRow("identity_manager", "sb_identity_auth_attempts_total", "sys.metrics.identity", req, Seconds(15), trusted, "trusted event stream", "rate 5m", "auth_anomaly_policy", "lock/review recommendation", "security unknown", "auth_attempt_rate"),
      ContractRow("session_control_manager", "sb_identity_sessions_active", "sys.metrics.identity", req, Seconds(10), trusted, "trusted gauge", "latest", "session_pressure_policy", "disconnect/reauth control", "deny session action", "active_sessions"),
      ContractRow("alert_manager", "sb_alerts_fired_total", "sys.metrics.alerts", req, Seconds(60), trusted, "trusted event stream", "dedupe window", "alert_policy", "dedupe/group/silence", "local event only", "alert_counter"),
      ContractRow("export_adapter_manager", "sb_export_adapter_queue_depth", "sys.metrics.export", req, Seconds(30), trusted, "trusted gauge", "latest/p95", "export_queue_backpressure", "pause/shed export", "deny export", "export_queue_depth"),
      ContractRow("cluster_upgrade_manager", "sb_cluster_rolling_upgrade_readiness_state", "cluster.sys.metrics.version_drift", req, Seconds(300), cluster_confirmed, "cluster confirmed", "latest", "upgrade_readiness_required", "allow/deny upgrade step", "upgrade denied", "readiness_state"),
      ContractRow("filespace_capacity_manager", "sb_filespace_total_bytes", "sys.metrics.storage.filespaces", req, Seconds(15), trusted, "trusted filespace sample", "latest", "minimum_free_percent", "capacity denominator", "capacity action denied", "filespace_capacity_snapshot"),
      ContractRow("filespace_capacity_manager", "sb_filespace_used_bytes", "sys.metrics.storage.filespaces", req, Seconds(15), trusted, "trusted filespace sample", "latest", "minimum_free_percent", "capacity use", "capacity action denied", "filespace_capacity_snapshot"),
      ContractRow("filespace_capacity_manager", "sb_filespace_free_bytes", "sys.metrics.storage.filespaces", req, Seconds(15), trusted, "trusted filespace sample", "latest", "minimum_free_bytes", "expansion/shrink decision", "capacity action denied", "filespace_capacity_snapshot"),
      ContractRow("filespace_capacity_manager", "sb_filespace_reserved_bytes", "sys.metrics.storage.filespaces", req, Seconds(15), trusted, "trusted filespace sample", "latest", "reservation_pressure", "reservation pressure", "capacity action denied", "filespace_reservation_snapshot"),
      ContractRow("filespace_capacity_manager", "sb_filespace_health_state", "sys.metrics.storage.filespaces", req, Seconds(15), trusted, "trusted state", "latest", "device_health_thresholds", "lifecycle safety", "action denied", "filespace_health_snapshot"),
      ContractRow("filespace_capacity_manager", "sb_filespace_role_state", "sys.metrics.storage.filespaces", req, Seconds(15), trusted, "trusted state", "latest", "role_transition_policy", "role authority", "action denied", "filespace_role_snapshot"),
      ContractRow("filespace_capacity_manager", "sb_filespace_truncate_ready_bytes", "sys.metrics.storage.filespaces", shrink_req, Seconds(60), trusted, "page-agent proof", "latest", "truncate_allowed", "truncate proof", "truncate denied", "filespace_shrink_proof"),
      ContractRow("filespace_capacity_manager", "sb_filespace_shrink_blocker_count", "sys.metrics.storage.filespaces", shrink_req, Seconds(60), trusted, "trusted blocker sample", "latest", "shrink_allowed", "blocker proof", "shrink denied", "filespace_shrink_blockers"),
      ContractRow("page_allocation_manager", "sb_page_free_count", "sys.metrics.storage.pages", req, Seconds(15), trusted, "trusted page sample", "latest", "preallocation_allowed", "allocation decision", "preallocation denied", "page_allocation_snapshot"),
      ContractRow("page_allocation_manager", "sb_page_allocated_count", "sys.metrics.storage.pages", req, Seconds(15), trusted, "trusted page sample", "latest", "relocation_allowed", "fragmentation/capacity", "relocation denied", "page_allocation_snapshot"),
      ContractRow("page_allocation_manager", "sb_page_reserved_count", "sys.metrics.storage.pages", req, Seconds(15), trusted, "trusted page sample", "latest", "max_reserved_bytes_per_filespace", "reservation pressure", "preallocation denied", "page_reservation_snapshot"),
      ContractRow("page_allocation_manager", "sb_page_allocation_latency_microseconds", "sys.metrics.storage.pages", req, Seconds(15), trusted, "trusted histogram", "p95 5m", "allocation_latency_warn_us", "allocator health", "health unknown", "page_allocation_latency"),
      ContractRow("page_allocation_manager", "sb_page_allocation_failures_total", "sys.metrics.storage.pages", req, Seconds(15), trusted, "trusted counter", "rate 5m", "allocation_failure_pressure", "allocator pressure", "preallocation denied", "page_allocation_failures"),
      ContractRow("page_allocation_manager", "sb_page_fragmentation_ratio", "sys.metrics.storage.pages", opt, Seconds(300), trusted, "trusted scanner", "latest/window", "fragmentation_threshold", "relocation recommendation", "recommendation suppressed", "page_fragmentation_snapshot"),
      ContractRow("page_allocation_manager", "sb_page_relocation_backlog_count", "sys.metrics.storage.pages", reloc_req, Seconds(60), trusted, "trusted relocation queue", "latest", "relocation_allowed", "relocation backlog", "relocation denied", "page_relocation_backlog"),
      ContractRow("page_allocation_manager", "sb_page_relocation_ready_for_filespace_shrink", "sys.metrics.storage.pages", shrink_req, Seconds(60), trusted, "page-agent proof", "latest", "publish_shrink_ready_allowed", "shrink readiness", "shrink readiness denied", "page_shrink_readiness")};
}

std::vector<AgentMetricDependency> MetricDependenciesForAgent(const std::string& agent_type_id) {
  std::vector<AgentMetricDependency> deps;
  for (const auto& row : AgentMetricDependencyContractRegistry()) {
    if (row.agent_type_id == agent_type_id) { deps.push_back(row.dependency); }
  }
  return deps;
}

std::optional<AgentMetricDependency> FindAgentMetricDependencyContract(
    const std::string& agent_type_id,
    const std::string& metric_family) {
  for (const auto& row : AgentMetricDependencyContractRegistry()) {
    if (row.agent_type_id == agent_type_id &&
        row.dependency.metric_family == metric_family) {
      return row.dependency;
    }
  }
  return std::nullopt;
}

// SEARCH_KEY: SB_AGENT_RUNTIME_CANONICAL_REGISTRY_IMPL
std::vector<AgentTypeDescriptor> CanonicalAgentRegistry() {
  std::vector<AgentTypeDescriptor> registry;
  for (const auto& entry : CanonicalAgentManifest()) {
    registry.push_back(Agent(entry.type_id, entry.deployment, entry.scope,
                             entry.authority, entry.default_activation,
                             MetricDependenciesForAgent(entry.type_id)));
  }
  return registry;
}

std::optional<AgentTypeDescriptor> FindAgentType(const std::string& type_id) {
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (descriptor.type_id == type_id) { return descriptor; }
  }
  return std::nullopt;
}

AgentRuntimeStatus ValidateAgentType(const AgentTypeDescriptor& descriptor) {
  if (descriptor.type_id.empty()) { return AgentError("SB_AGENT_REGISTRY.TYPE_ID_REQUIRED"); }
  if (descriptor.scope.empty()) { return AgentError("SB_AGENT_REGISTRY.SCOPE_REQUIRED", descriptor.type_id); }
  if (descriptor.deployment == AgentDeployment::cluster && !descriptor.cluster_only) {
    return AgentError("SB_AGENT_REGISTRY.CLUSTER_FLAG_REQUIRED", descriptor.type_id);
  }
  if (descriptor.metric_dependencies.empty()) {
    return AgentError("SB_AGENT_REGISTRY.METRIC_DEPENDENCY_REQUIRED", descriptor.type_id);
  }
  for (const auto& dep : descriptor.metric_dependencies) {
    if (dep.metric_family.empty() || dep.namespace_prefix.empty()) {
      return AgentError("SB_AGENT_METRIC_CONTRACT.INCOMPLETE_ROW",
                        descriptor.type_id);
    }
    if (dep.max_freshness_microseconds == 0 || dep.required_quality.empty() ||
        dep.aggregation.empty() || dep.policy_field.empty() ||
        dep.decision_use.empty() || dep.fail_behavior.empty() ||
        dep.evidence_field.empty()) {
      return AgentError("SB_AGENT_METRIC_CONTRACT.INCOMPLETE_ROW",
                        descriptor.type_id + ":" + dep.metric_family);
    }
    if (dep.cluster_only &&
        dep.namespace_prefix.rfind("cluster.sys.metrics.", 0) != 0) {
      return AgentError("SB_AGENT_METRIC_CONTRACT.CLUSTER_NAMESPACE_REQUIRED",
                        descriptor.type_id + ":" + dep.metric_family);
    }
    if (!dep.cluster_only &&
        dep.namespace_prefix.rfind("cluster.sys.metrics.", 0) == 0) {
      return AgentError("SB_AGENT_METRIC_CONTRACT.CLUSTER_FLAG_REQUIRED",
                        descriptor.type_id + ":" + dep.metric_family);
    }
  }
  return AgentOk();
}

StorageSpaceAgentDefaults DefaultStorageSpaceAgentDefaults() {
  return StorageSpaceAgentDefaults{};
}

AgentRuntimeStatus ValidateStorageSpaceAgentDefaults(const StorageSpaceAgentDefaults& defaults) {
  if (defaults.filespace_min_available_pages == 0) {
    return AgentError("SB_AGENT_STORAGE_DEFAULTS.INVALID_MIN_AVAILABLE_PAGES",
                      "filespace_min_available_pages must be greater than zero");
  }
  if (defaults.filespace_target_available_pages < defaults.filespace_min_available_pages) {
    return AgentError("SB_AGENT_STORAGE_DEFAULTS.INVALID_TARGET_AVAILABLE_PAGES",
                      "filespace_target_available_pages must be greater than or equal to filespace_min_available_pages");
  }
  const u64 expected_notify_pages = defaults.filespace_target_available_pages / 2;
  if (defaults.filespace_page_allocation_notify_pages != expected_notify_pages) {
    return AgentError("SB_AGENT_STORAGE_DEFAULTS.INVALID_FILESPACE_NOTIFY_PAGES",
                      "filespace_page_allocation_notify_pages must equal half of filespace_target_available_pages");
  }
  if (defaults.page_allocation_notify_released_free_pages != expected_notify_pages) {
    return AgentError("SB_AGENT_STORAGE_DEFAULTS.INVALID_PAGE_NOTIFY_PAGES",
                      "page_allocation_notify_released_free_pages must equal half of filespace_target_available_pages");
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateCanonicalAgentRegistry() {
  const auto defaults_status = ValidateStorageSpaceAgentDefaults(DefaultStorageSpaceAgentDefaults());
  if (!defaults_status.ok) { return defaults_status; }

  std::set<std::string> seen;
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    const auto status = ValidateAgentType(descriptor);
    if (!status.ok) { return status; }
    if (!seen.insert(descriptor.type_id).second) {
      return AgentError("SB_AGENT_REGISTRY.DUPLICATE_TYPE_ID", descriptor.type_id);
    }
    const auto contract_deps = MetricDependenciesForAgent(descriptor.type_id);
    if (contract_deps.size() != descriptor.metric_dependencies.size()) {
      return AgentError("SB_AGENT_METRIC_CONTRACT.DESCRIPTOR_DRIFT",
                        descriptor.type_id);
    }
    for (const auto& dep : descriptor.metric_dependencies) {
      const auto contract_dep =
          FindAgentMetricDependencyContract(descriptor.type_id, dep.metric_family);
      if (!contract_dep.has_value() ||
          contract_dep->namespace_prefix != dep.namespace_prefix ||
          contract_dep->dependency_kind != dep.dependency_kind ||
          contract_dep->max_freshness_microseconds != dep.max_freshness_microseconds ||
          contract_dep->policy_field != dep.policy_field ||
          contract_dep->decision_use != dep.decision_use ||
          contract_dep->fail_behavior != dep.fail_behavior ||
          contract_dep->evidence_field != dep.evidence_field) {
        return AgentError("SB_AGENT_METRIC_CONTRACT.DESCRIPTOR_DRIFT",
                          descriptor.type_id + ":" + dep.metric_family);
      }
    }
  }
  return AgentOk();
}

AgentFeatureAvailability EvaluateAgentFeatureAvailability(const AgentTypeDescriptor& descriptor,
                                                          const AgentRuntimeContext& context) {
  if (descriptor.cluster_only && !context.cluster_authority_available) {
    return AgentFeatureAvailability::unavailable_cluster_authority;
  }
  if (descriptor.cluster_only && context.standalone_edition) {
    return AgentFeatureAvailability::unavailable_edition;
  }
  if ((descriptor.deployment == AgentDeployment::cluster || descriptor.deployment == AgentDeployment::both) &&
      !context.private_features_available && descriptor.cluster_only) {
    return AgentFeatureAvailability::unavailable_private_feature;
  }
  return AgentFeatureAvailability::available;
}

std::string DeterministicAgentInstanceUuid(const std::string& database_uuid,
                                           const std::string& agent_type_id,
                                           const std::string& scope,
                                           u64 policy_generation) {
  return DurableObjectUuidFromKey(database_uuid + "|" + agent_type_id + "|" + scope + "|" +
                                  std::to_string(policy_generation));
}

std::string SerializeAgentInstanceRecord(const AgentInstanceRecord& instance) {
  return instance.instance_uuid + "|" + instance.agent_type_id + "|" + instance.policy_uuid + "|" +
         instance.scope + "|" + AgentLifecycleStateName(instance.state) + "|" +
         std::to_string(instance.run_generation) + "|" + std::to_string(instance.lease_until_microseconds) + "|" +
         (instance.disabled_by_operator ? "1" : "0") + "|" + (instance.safe_mode ? "1" : "0") + "|" +
         (instance.quarantined ? "1" : "0") + "|" + std::to_string(instance.policy_generation) + "|" +
         std::to_string(instance.instance_generation) + "|" + std::to_string(instance.retired_generation) + "|" +
         instance.retirement_evidence_uuid + "|" +
         std::to_string(instance.supervision_failure_count) + "|" +
         std::to_string(instance.restart_attempts) + "|" +
         std::to_string(instance.restart_not_before_microseconds) + "|" +
         std::to_string(instance.cooldown_until_microseconds) + "|" +
         (instance.cancellation_requested ? "1" : "0") + "|" +
         instance.last_failure_diagnostic_code + "|" + instance.last_supervision_detail;
}

AgentRuntimeStatus RestoreAgentInstanceRecord(const std::string& encoded,
                                              AgentInstanceRecord* instance) {
  if (instance == nullptr) { return AgentError("SB_AGENT_INSTANCE.RESTORE_TARGET_REQUIRED"); }
  std::vector<std::string> parts;
  std::stringstream stream(encoded);
  std::string part;
  while (std::getline(stream, part, '|')) { parts.push_back(part); }
  if (parts.size() < 10) { return AgentError("SB_AGENT_INSTANCE.ENCODING_INVALID", encoded); }
  instance->instance_uuid = parts[0];
  instance->agent_type_id = parts[1];
  instance->policy_uuid = parts[2];
  instance->scope = parts[3];
  const std::string& state = parts[4];
  for (AgentLifecycleState candidate : {AgentLifecycleState::created, AgentLifecycleState::registered,
                                        AgentLifecycleState::disabled, AgentLifecycleState::observe_only,
                                        AgentLifecycleState::recommend_only, AgentLifecycleState::dry_run,
                                        AgentLifecycleState::running, AgentLifecycleState::paused,
                                        AgentLifecycleState::safe_mode, AgentLifecycleState::quarantined,
                                        AgentLifecycleState::stopping, AgentLifecycleState::stopped,
                                        AgentLifecycleState::retired, AgentLifecycleState::failed}) {
    if (state == AgentLifecycleStateName(candidate)) { instance->state = candidate; break; }
  }
  try {
    instance->run_generation = static_cast<u64>(std::stoull(parts[5]));
    instance->lease_until_microseconds = static_cast<u64>(std::stoull(parts[6]));
  } catch (...) {
    return AgentError("SB_AGENT_INSTANCE.NUMERIC_FIELD_INVALID", encoded);
  }
  instance->disabled_by_operator = parts[7] == "1";
  instance->safe_mode = parts[8] == "1";
  instance->quarantined = parts[9] == "1";
  if (parts.size() >= 13) {
    try {
      instance->policy_generation = static_cast<u64>(std::stoull(parts[10]));
      instance->instance_generation = static_cast<u64>(std::stoull(parts[11]));
      instance->retired_generation = static_cast<u64>(std::stoull(parts[12]));
    } catch (...) {
      return AgentError("SB_AGENT_INSTANCE.NUMERIC_FIELD_INVALID", encoded);
    }
  }
  if (parts.size() >= 14) { instance->retirement_evidence_uuid = parts[13]; }
  if (parts.size() >= 19) {
    try {
      instance->supervision_failure_count = static_cast<u64>(std::stoull(parts[14]));
      instance->restart_attempts = static_cast<u64>(std::stoull(parts[15]));
      instance->restart_not_before_microseconds = static_cast<u64>(std::stoull(parts[16]));
      instance->cooldown_until_microseconds = static_cast<u64>(std::stoull(parts[17]));
    } catch (...) {
      return AgentError("SB_AGENT_INSTANCE.NUMERIC_FIELD_INVALID", encoded);
    }
    instance->cancellation_requested = parts[18] == "1";
  }
  if (parts.size() >= 20) { instance->last_failure_diagnostic_code = parts[19]; }
  if (parts.size() >= 21) { instance->last_supervision_detail = parts[20]; }
  if (instance->state == AgentLifecycleState::retired && instance->retirement_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_INSTANCE.RETIREMENT_EVIDENCE_REQUIRED", instance->instance_uuid);
  }
  return AgentOk();
}

std::vector<std::string> AgentCatalogRecordLayouts() {
  return {"agent_type(type_uuid,type_id,deployment,scope,authority,default_activation)",
          "agent_instance(instance_uuid,type_id,scope,state,policy_uuid,policy_generation,run_generation,lease_until,retirement_evidence)",
          "agent_policy(policy_uuid,type_id,policy_family,scope,activation,generation,budgets,cooldown,action_mode,config_fields,invalid_behavior)",
          "agent_policy_attachment(attachment_uuid,type_id,scope,policy_family,policy_uuid,policy_generation,attachment_generation,active,evidence_uuid)",
          "agent_evidence(evidence_uuid,instance_uuid,type_id,kind,diagnostic,detail,metric_digest,policy_generation,principal_uuid,rights,scope_uuids,result_state,redaction,retention,tamper_digest,created,expires)",
          "agent_action(action_uuid,instance_uuid,owner_uuid,operation_id,actuator_provider_id,state,idempotency_key,input_evidence_digest,evidence_uuid,verification_evidence_uuid,diagnostic,generation,retry_count,outcome_verified,compensation)",
          "agent_approval(approval_uuid,action_uuid,state,requestor,approver,expires,evidence_uuid)",
          "agent_override(override_uuid,type_id,scope,principal_uuid,expires,active,evidence_uuid)",
          "agent_lease(lease_uuid,instance_uuid,owner_uuid,state,acquired_at,expires_at,heartbeat_generation,last_heartbeat,replay_generation,evidence_uuid)",
          "agent_health(instance_uuid,health_state,diagnostic,evidence_uuid,observed_at)",
          "agent_retained_history(history_uuid,subject_uuid,event_kind,diagnostic,evidence_uuid,recorded_at)",
          "agent_state_migration(record_uuid,from_version,to_version,result,evidence_uuid)"};
}

bool AgentLifecycleTransitionAllowed(AgentLifecycleState from, AgentLifecycleState to) {
  if (from == to) { return true; }
  if (from == AgentLifecycleState::retired) { return false; }
  if (from == AgentLifecycleState::created) { return to == AgentLifecycleState::registered || to == AgentLifecycleState::disabled || to == AgentLifecycleState::retired; }
  if (from == AgentLifecycleState::registered) {
    return to == AgentLifecycleState::disabled || to == AgentLifecycleState::observe_only ||
           to == AgentLifecycleState::recommend_only || to == AgentLifecycleState::dry_run ||
           to == AgentLifecycleState::running || to == AgentLifecycleState::safe_mode ||
           to == AgentLifecycleState::retired;
  }
  if (from == AgentLifecycleState::disabled) { return to == AgentLifecycleState::registered || to == AgentLifecycleState::stopped || to == AgentLifecycleState::retired; }
  if (from == AgentLifecycleState::observe_only || from == AgentLifecycleState::recommend_only ||
      from == AgentLifecycleState::dry_run || from == AgentLifecycleState::running) {
    return to == AgentLifecycleState::paused || to == AgentLifecycleState::disabled ||
           to == AgentLifecycleState::safe_mode ||
           to == AgentLifecycleState::stopping || to == AgentLifecycleState::failed ||
           to == AgentLifecycleState::quarantined || to == AgentLifecycleState::retired;
  }
  if (from == AgentLifecycleState::paused) {
    return to == AgentLifecycleState::observe_only || to == AgentLifecycleState::recommend_only ||
           to == AgentLifecycleState::dry_run || to == AgentLifecycleState::running ||
           to == AgentLifecycleState::disabled || to == AgentLifecycleState::stopping ||
           to == AgentLifecycleState::safe_mode || to == AgentLifecycleState::failed ||
           to == AgentLifecycleState::quarantined || to == AgentLifecycleState::retired;
  }
  if (from == AgentLifecycleState::safe_mode) { return to == AgentLifecycleState::paused || to == AgentLifecycleState::stopping || to == AgentLifecycleState::quarantined || to == AgentLifecycleState::retired; }
  if (from == AgentLifecycleState::stopping) { return to == AgentLifecycleState::stopped || to == AgentLifecycleState::registered || to == AgentLifecycleState::retired; }
  if (from == AgentLifecycleState::stopped) { return to == AgentLifecycleState::registered || to == AgentLifecycleState::disabled || to == AgentLifecycleState::retired; }
  if (from == AgentLifecycleState::failed) { return to == AgentLifecycleState::registered || to == AgentLifecycleState::quarantined || to == AgentLifecycleState::stopped || to == AgentLifecycleState::retired; }
  if (from == AgentLifecycleState::quarantined) { return to == AgentLifecycleState::disabled || to == AgentLifecycleState::stopped || to == AgentLifecycleState::retired; }
  return false;
}

AgentRuntimeStatus ValidateAgentLifecycleTransition(AgentLifecycleState from, AgentLifecycleState to) {
  if (AgentLifecycleTransitionAllowed(from, to)) { return AgentOk(); }
  return AgentError("SB_AGENT_LIFECYCLE.INVALID_TRANSITION",
                    std::string(AgentLifecycleStateName(from)) + "->" + AgentLifecycleStateName(to));
}

std::vector<std::string> RequiredPolicyFamiliesForAgent(const AgentTypeDescriptor& descriptor) {
  if (descriptor.type_id == "node_resource_agent") { return {"node_resource_policy"}; }
  if (descriptor.type_id == "metrics_registry_manager") { return {"metric_registry_policy"}; }
  if (descriptor.type_id == "storage_health_manager") { return {"storage_health_policy"}; }
  if (descriptor.type_id == "filespace_capacity_manager") {
    return {"filespace_capacity_policy", "filespace_shadow_promotion_policy"};
  }
  if (descriptor.type_id == "page_allocation_manager") {
    return {"page_preallocation_policy", "page_relocation_policy"};
  }
  if (descriptor.type_id == "memory_governor") { return {"memory_governor_policy"}; }
  if (descriptor.type_id == "index_health_manager") { return {"index_health_policy"}; }
  if (descriptor.type_id == "cluster_autoscale_manager") { return {"cluster_autoscale_policy"}; }
  if (descriptor.type_id == "admission_control_manager") { return {"admission_control_policy"}; }
  if (descriptor.type_id == "parser_interface_manager") { return {"parser_health_policy"}; }
  if (descriptor.type_id == "transaction_pressure_manager") { return {"long_transaction_policy"}; }
  if (descriptor.type_id == "storage_version_cleanup_agent") { return {"storage_version_cleanup_policy"}; }
  if (descriptor.type_id == "cleanup_archive_manager") { return {"cleanup_archive_policy"}; }
  if (descriptor.type_id == "policy_recommendation_manager") { return {"policy_recommendation_policy"}; }
  if (descriptor.type_id == "distributed_query_metrics_agent") { return {"distributed_query_policy"}; }
  if (descriptor.type_id == "remote_query_routing_agent") { return {"remote_routing_policy"}; }
  if (descriptor.type_id == "runtime_learning_agent") { return {"optimizer_learning_policy"}; }
  if (descriptor.type_id == "support_bundle_triage_agent") { return {"support_bundle_policy"}; }
  if (descriptor.type_id == "cluster_scheduler_manager") { return {"cluster_scheduler_policy"}; }
  if (descriptor.type_id == "job_control_manager") { return {"job_control_policy"}; }
  if (descriptor.type_id == "backup_manager") { return {"backup_policy"}; }
  if (descriptor.type_id == "archive_manager") { return {"archive_policy"}; }
  if (descriptor.type_id == "restore_drill_manager") { return {"restore_drill_policy"}; }
  if (descriptor.type_id == "pitr_manager") { return {"pitr_policy"}; }
  if (descriptor.type_id == "identity_manager") { return {"identity_lifecycle_policy"}; }
  if (descriptor.type_id == "session_control_manager") { return {"session_control_policy"}; }
  if (descriptor.type_id == "alert_manager") { return {"alerting_baseline"}; }
  if (descriptor.type_id == "export_adapter_manager") { return {"export_default_baseline"}; }
  if (descriptor.type_id == "cluster_upgrade_manager") { return {"upgrade_policy"}; }
  return {};
}

std::vector<std::string> RequiredPolicyConfigFieldsForFamily(const std::string& policy_family) {
  return RequiredAgentPolicySchemaFieldsForFamily(policy_family);
}

AgentPolicy BaselinePolicyForAgentFamily(const AgentTypeDescriptor& descriptor,
                                         const std::string& policy_family,
                                         u64 policy_generation) {
  AgentPolicy policy;
  policy.policy_uuid = DurableObjectUuidFromKey("agent_policy|" + descriptor.type_id + "|" +
                                                policy_family + "|baseline");
  policy.policy_name = descriptor.type_id + " " + policy_family + " baseline";
  policy.policy_family = policy_family;
  policy.scope = descriptor.scope;
  const auto default_mode = BaselineDefaultModeForPolicyFamily(policy_family);
  policy.activation = ActivationForBaselineDefaultMode(default_mode);
  policy.action_mode = default_mode == "request_action" ? "request_action" : default_mode;
  policy.invalid_policy_behavior = DefaultInvalidPolicyBehavior(policy_family);
  policy.policy_generation = policy_generation;
  policy.enabled = default_mode != "disabled";
  policy.allow_live_action = false;
  policy.require_manual_approval = descriptor.authority != AgentAuthorityClass::observe_only &&
                                   descriptor.authority != AgentAuthorityClass::recommend_only;
  if (descriptor.authority == AgentAuthorityClass::observe_only ||
      descriptor.authority == AgentAuthorityClass::recommend_only) {
    policy.require_dry_run_before_live = true;
  }
  for (const auto& dep : descriptor.metric_dependencies) { policy.required_metric_families.push_back(dep.metric_family); }
  policy.config_fields = DefaultAgentPolicyConfigFieldsForFamily(policy_family);
  return policy;
}

AgentPolicy BaselinePolicyForAgent(const AgentTypeDescriptor& descriptor) {
  const auto families = RequiredPolicyFamiliesForAgent(descriptor);
  return BaselinePolicyForAgentFamily(descriptor, families.empty() ? "" : families.front(), 1);
}

std::vector<AgentPolicyBootstrapRecord> BaselinePolicyBootstrapRecordsForAgent(
    const AgentTypeDescriptor& descriptor,
    u64 policy_generation) {
  std::vector<AgentPolicyBootstrapRecord> records;
  for (const auto& family : RequiredPolicyFamiliesForAgent(descriptor)) {
    const auto policy = BaselinePolicyForAgentFamily(descriptor, family, policy_generation);
    AgentPolicyBootstrapRecord record;
    record.agent_type_id = descriptor.type_id;
    record.policy_family = family;
    record.policy_uuid = policy.policy_uuid;
    record.scope = policy.scope;
    record.action_mode = policy.action_mode;
    record.invalid_policy_behavior = policy.invalid_policy_behavior;
    record.enabled = policy.enabled;
    record.activation = policy.activation;
    record.policy_generation = policy.policy_generation;
    record.run_interval_microseconds = policy.run_interval_microseconds;
    record.cooldown_microseconds = policy.cooldown_microseconds;
    record.required_fields = RequiredPolicyConfigFieldsForFamily(family);
    record.config_fields = policy.config_fields;
    records.push_back(std::move(record));
  }
  return records;
}

std::vector<AgentPolicyBootstrapRecord> DatabaseApplicableBaselinePolicyBootstrapRecords(
    u64 policy_generation) {
  std::vector<AgentPolicyBootstrapRecord> records;
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (descriptor.scope.find("database") == std::string::npos ||
        descriptor.deployment == AgentDeployment::cluster ||
        descriptor.cluster_only) {
      continue;
    }
    auto agent_records = BaselinePolicyBootstrapRecordsForAgent(descriptor, policy_generation);
    records.insert(records.end(), agent_records.begin(), agent_records.end());
  }
  return records;
}

AgentRuntimeStatus ValidateAgentPolicy(const AgentPolicy& policy,
                                       const AgentTypeDescriptor& descriptor) {
  if (policy.policy_uuid.empty()) { return AgentError("SB_AGENT_POLICY.UUID_REQUIRED", descriptor.type_id); }
  if (!policy.enabled &&
      (policy.activation != AgentActivationProfile::disabled || policy.action_mode != "disabled")) {
    return AgentError("SB_AGENT_POLICY.DISABLED_STATE_INCONSISTENT", descriptor.type_id);
  }
  if (!Contains(RequiredPolicyFamiliesForAgent(descriptor), policy.policy_family)) {
    return AgentError("SB_AGENT_POLICY.WRONG_FAMILY", descriptor.type_id + ":" + policy.policy_family);
  }
  if (policy.scope != descriptor.scope) {
    return AgentError("SB_AGENT_POLICY.SCOPE_INCOMPATIBLE", descriptor.type_id + ":" + policy.scope);
  }
  if (policy.policy_generation == 0) {
    return AgentError("SB_AGENT_POLICY.GENERATION_REQUIRED", descriptor.type_id);
  }
  const auto config_status = ValidateAgentPolicyConfigAgainstSchema(policy);
  if (!config_status.ok) { return config_status; }
  if (policy.activation == AgentActivationProfile::live_action && !policy.allow_live_action) {
    return AgentError("SB_AGENT_POLICY.LIVE_ACTION_NOT_ALLOWED", descriptor.type_id);
  }
  if (policy.activation == AgentActivationProfile::live_action && descriptor.authority == AgentAuthorityClass::observe_only) {
    return AgentError("SB_AGENT_POLICY.OBSERVE_AGENT_CANNOT_LIVE_ACT", descriptor.type_id);
  }
  if (policy.run_interval_microseconds == 0 || policy.lease_microseconds == 0 ||
      policy.max_runtime_microseconds == 0 || policy.max_restart_attempts == 0 ||
      policy.initial_backoff_microseconds == 0 || policy.max_backoff_microseconds == 0) {
    return AgentError("SB_AGENT_POLICY.INVALID_TIME_BUDGET", descriptor.type_id);
  }
  if (policy.max_backoff_microseconds < policy.initial_backoff_microseconds) {
    return AgentError("SB_AGENT_POLICY.INVALID_BACKOFF_BUDGET", descriptor.type_id);
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateAgentPolicyAttachment(const AgentPolicy& policy,
                                                 const AgentPolicyAttachmentRecord& attachment,
                                                 const AgentTypeDescriptor& descriptor,
                                                 u64 expected_policy_generation) {
  if (!attachment.active) { return AgentError("SB_AGENT_POLICY_ATTACHMENT.MISSING_ACTIVE", descriptor.type_id); }
  if (!attachment.valid) {
    return AgentError(attachment.diagnostic_code.empty() ? "SB_AGENT_POLICY_ATTACHMENT.INVALID" : attachment.diagnostic_code,
                      descriptor.type_id);
  }
  if (attachment.attachment_uuid.empty() || attachment.evidence_uuid.empty()) {
    return AgentError("SB_AGENT_POLICY_ATTACHMENT.EVIDENCE_REQUIRED", descriptor.type_id);
  }
  if (attachment.agent_type_id != descriptor.type_id) {
    return AgentError("SB_AGENT_POLICY_ATTACHMENT.AGENT_MISMATCH", attachment.agent_type_id);
  }
  if (attachment.policy_uuid != policy.policy_uuid) {
    return AgentError("SB_AGENT_POLICY_ATTACHMENT.POLICY_MISMATCH", attachment.policy_uuid);
  }
  if (attachment.policy_family != policy.policy_family ||
      !Contains(RequiredPolicyFamiliesForAgent(descriptor), attachment.policy_family)) {
    return AgentError("SB_AGENT_POLICY_ATTACHMENT.WRONG_FAMILY", attachment.policy_family);
  }
  if (attachment.scope != descriptor.scope || attachment.scope != policy.scope) {
    return AgentError("SB_AGENT_POLICY_ATTACHMENT.SCOPE_INCOMPATIBLE", attachment.scope);
  }
  if (attachment.policy_generation != policy.policy_generation ||
      attachment.policy_generation != expected_policy_generation) {
    return AgentError("SB_AGENT_POLICY_ATTACHMENT.STALE_GENERATION",
                      std::to_string(attachment.policy_generation));
  }
  return ValidateAgentPolicy(policy, descriptor);
}

AgentRuntimeStatus ValidateAgentPolicyStateForMutation(const AgentPolicy* policy,
                                                       const AgentPolicyAttachmentRecord* attachment,
                                                       const AgentTypeDescriptor& descriptor,
                                                       u64 expected_policy_generation) {
  if (policy == nullptr) {
    return AgentError("SB_AGENT_POLICY.MISSING", descriptor.type_id);
  }
  if (attachment == nullptr) {
    return AgentError("SB_AGENT_POLICY_ATTACHMENT.MISSING", descriptor.type_id);
  }
  return ValidateAgentPolicyAttachment(*policy, *attachment, descriptor, expected_policy_generation);
}

void InMemoryAgentRuntimeCatalog::BootstrapDatabasePolicies(const std::string& database_uuid,
                                                            u64 policy_generation) {
  policies_.clear();
  attachments_.clear();
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (descriptor.scope.find("database") == std::string::npos ||
        descriptor.deployment == AgentDeployment::cluster ||
        descriptor.cluster_only) {
      continue;
    }
    for (const auto& family : RequiredPolicyFamiliesForAgent(descriptor)) {
      auto policy = BaselinePolicyForAgentFamily(descriptor, family, policy_generation);
      AgentPolicyAttachmentRecord attachment;
      attachment.attachment_uuid = DurableObjectUuidFromKey(database_uuid + "|attachment|" +
                                                            descriptor.type_id + "|" + family + "|" +
                                                            std::to_string(policy_generation));
      attachment.agent_type_id = descriptor.type_id;
      attachment.policy_family = family;
      attachment.policy_uuid = policy.policy_uuid;
      attachment.scope = descriptor.scope;
      attachment.policy_generation = policy_generation;
      attachment.attachment_generation = policy_generation;
      attachment.baseline = true;
      attachment.active = true;
      attachment.valid = true;
      attachment.evidence_uuid = DurableObjectUuidFromKey("agent_policy_attach_evidence|" +
                                                          attachment.attachment_uuid);
      policies_.push_back(std::move(policy));
      attachments_.push_back(std::move(attachment));
    }
  }
}

AgentRuntimeStatus InMemoryAgentRuntimeCatalog::SaveInstances(
    const std::vector<AgentInstanceRecord>& instances) {
  for (const auto& instance : instances) {
    if (instance.state == AgentLifecycleState::retired && instance.retirement_evidence_uuid.empty()) {
      return AgentError("SB_AGENT_INSTANCE.RETIREMENT_EVIDENCE_REQUIRED", instance.instance_uuid);
    }
  }
  instances_ = instances;
  return AgentOk();
}

AgentRuntimeStatus InMemoryAgentRuntimeCatalog::RetireInstance(const std::string& instance_uuid,
                                                               std::string evidence_uuid,
                                                               u64 retired_generation) {
  if (evidence_uuid.empty()) {
    return AgentError("SB_AGENT_INSTANCE.RETIREMENT_EVIDENCE_REQUIRED", instance_uuid);
  }
  for (auto& instance : instances_) {
    if (instance.instance_uuid != instance_uuid) { continue; }
    instance.state = AgentLifecycleState::retired;
    instance.retired_generation = retired_generation;
    instance.retirement_evidence_uuid = evidence_uuid;
    AgentEvidenceRecord evidence;
    evidence.evidence_uuid = evidence_uuid;
    evidence.agent_type_id = instance.agent_type_id;
    evidence.instance_uuid = instance.instance_uuid;
    evidence.evidence_kind = "agent_instance_retired";
    evidence.diagnostic_code = "SB_AGENT_INSTANCE.RETIRED";
    evidence.detail = instance.agent_type_id;
    evidence_.push_back(std::move(evidence));
    return AgentOk();
  }
  return AgentError("SB_AGENT_INSTANCE.NOT_FOUND", instance_uuid);
}

std::vector<AgentRuntimeStatus> LintAgentPolicy(const AgentPolicy& policy,
                                                const AgentTypeDescriptor& descriptor,
                                                const scratchbird::core::metrics::MetricRegistry& registry) {
  std::vector<AgentRuntimeStatus> findings;
  const auto status = ValidateAgentPolicy(policy, descriptor);
  if (!status.ok) { findings.push_back(status); }
  std::set<std::string> deps;
  for (const auto& dep : policy.policy_dependencies) {
    if (dep == policy.policy_uuid) { findings.push_back(AgentError("SB_AGENT_POLICY.CIRCULAR_DEPENDENCY", dep)); }
    if (!deps.insert(dep).second) { findings.push_back(AgentError("SB_AGENT_POLICY.DUPLICATE_DEPENDENCY", dep)); }
  }
  for (const auto& family : policy.required_metric_families) {
    if (family.empty() || registry.FindDescriptorOrAlias(family) == nullptr) {
      findings.push_back(AgentError("SB_AGENT_POLICY.INVALID_METRIC_PATH", family));
    }
  }
  if (policy.max_label_cardinality == 0 || policy.max_evidence_fanout == 0 || policy.max_history_query_rows == 0) {
    findings.push_back(AgentError("SB_AGENT_POLICY.INVALID_CARDINALITY_BUDGET", descriptor.type_id));
  }
  if (findings.empty()) { findings.push_back(AgentOk()); }
  return findings;
}

// SEARCH_KEY: SB_AGENT_SECURITY_GRANT_MATRIX
const char* AgentSecurityRightName(AgentSecurityRight value) {
  switch (value) {
    case AgentSecurityRight::obs_agent_state_read: return "OBS_AGENT_STATE_READ";
    case AgentSecurityRight::obs_agent_evidence_read: return "OBS_AGENT_EVIDENCE_READ";
    case AgentSecurityRight::obs_agent_recommendation_read: return "OBS_AGENT_RECOMMENDATION_READ";
    case AgentSecurityRight::obs_agent_control: return "OBS_AGENT_CONTROL";
    case AgentSecurityRight::obs_agent_action_approve: return "OBS_AGENT_ACTION_APPROVE";
    case AgentSecurityRight::obs_agent_action_cancel: return "OBS_AGENT_ACTION_CANCEL";
    case AgentSecurityRight::obs_agent_override: return "OBS_AGENT_OVERRIDE";
    case AgentSecurityRight::obs_support_bundle_read: return "OBS_SUPPORT_BUNDLE_READ";
    case AgentSecurityRight::obs_policy_read: return "OBS_POLICY_READ";
    case AgentSecurityRight::obs_policy_simulate: return "OBS_POLICY_SIMULATE";
    case AgentSecurityRight::obs_policy_edit_draft: return "OBS_POLICY_EDIT_DRAFT";
    case AgentSecurityRight::obs_policy_validate: return "OBS_POLICY_VALIDATE";
    case AgentSecurityRight::obs_policy_approve: return "OBS_POLICY_APPROVE";
    case AgentSecurityRight::obs_policy_apply: return "OBS_POLICY_APPLY";
    case AgentSecurityRight::obs_policy_rollback: return "OBS_POLICY_ROLLBACK";
    case AgentSecurityRight::obs_policy_delete: return "OBS_POLICY_DELETE";
    case AgentSecurityRight::obs_cluster_health_inspect: return "OBS_CLUSTER_HEALTH_INSPECT";
    case AgentSecurityRight::obs_cluster_topology_inspect: return "OBS_CLUSTER_TOPOLOGY_INSPECT";
    case AgentSecurityRight::obs_cluster_control: return "OBS_CLUSTER_CONTROL";
    case AgentSecurityRight::sec_auth_metrics_read: return "SEC_AUTH_METRICS_READ";
    case AgentSecurityRight::sec_redaction_policy_edit: return "SEC_REDACTION_POLICY_EDIT";
    case AgentSecurityRight::sec_export_policy_approve: return "SEC_EXPORT_POLICY_APPROVE";
    case AgentSecurityRight::sec_identity_admin: return "SEC_IDENTITY_ADMIN";
    case AgentSecurityRight::internal_agent_trace: return "SB_AGENT_INTERNAL_TRACE";
    case AgentSecurityRight::external_named_right: return "EXTERNAL_NAMED_RIGHT";
  }
  return "EXTERNAL_NAMED_RIGHT";
}

std::optional<AgentSecurityRight> ParseAgentSecurityRight(const std::string& right) {
  static const std::vector<std::pair<const char*, AgentSecurityRight>> rights = {
      {"OBS_AGENT_STATE_READ", AgentSecurityRight::obs_agent_state_read},
      {"OBS_AGENT_EVIDENCE_READ", AgentSecurityRight::obs_agent_evidence_read},
      {"OBS_AGENT_RECOMMENDATION_READ", AgentSecurityRight::obs_agent_recommendation_read},
      {"OBS_AGENT_CONTROL", AgentSecurityRight::obs_agent_control},
      {"OBS_AGENT_ACTION_APPROVE", AgentSecurityRight::obs_agent_action_approve},
      {"OBS_AGENT_ACTION_CANCEL", AgentSecurityRight::obs_agent_action_cancel},
      {"OBS_AGENT_OVERRIDE", AgentSecurityRight::obs_agent_override},
      {"OBS_SUPPORT_BUNDLE_READ", AgentSecurityRight::obs_support_bundle_read},
      {"OBS_POLICY_READ", AgentSecurityRight::obs_policy_read},
      {"OBS_POLICY_SIMULATE", AgentSecurityRight::obs_policy_simulate},
      {"OBS_POLICY_EDIT_DRAFT", AgentSecurityRight::obs_policy_edit_draft},
      {"OBS_POLICY_VALIDATE", AgentSecurityRight::obs_policy_validate},
      {"OBS_POLICY_APPROVE", AgentSecurityRight::obs_policy_approve},
      {"OBS_POLICY_APPLY", AgentSecurityRight::obs_policy_apply},
      {"OBS_POLICY_ROLLBACK", AgentSecurityRight::obs_policy_rollback},
      {"OBS_POLICY_DELETE", AgentSecurityRight::obs_policy_delete},
      {"OBS_CLUSTER_HEALTH_INSPECT", AgentSecurityRight::obs_cluster_health_inspect},
      {"OBS_CLUSTER_TOPOLOGY_INSPECT", AgentSecurityRight::obs_cluster_topology_inspect},
      {"OBS_CLUSTER_CONTROL", AgentSecurityRight::obs_cluster_control},
      {"SEC_AUTH_METRICS_READ", AgentSecurityRight::sec_auth_metrics_read},
      {"SEC_REDACTION_POLICY_EDIT", AgentSecurityRight::sec_redaction_policy_edit},
      {"SEC_EXPORT_POLICY_APPROVE", AgentSecurityRight::sec_export_policy_approve},
      {"SEC_IDENTITY_ADMIN", AgentSecurityRight::sec_identity_admin},
      {"OBS_AGENT_POLICY_CONTROL", AgentSecurityRight::obs_policy_apply},
      {"SB_AGENT_INTERNAL_TRACE", AgentSecurityRight::internal_agent_trace},
      {"OBS_AGENT_INTERNAL", AgentSecurityRight::internal_agent_trace}};
  for (const auto& entry : rights) {
    if (right == entry.first) { return entry.second; }
  }
  if (right.empty()) { return std::nullopt; }
  return AgentSecurityRight::external_named_right;
}

const char* AgentSecurityCommandFamilyName(AgentSecurityCommandFamily value) {
  switch (value) {
    case AgentSecurityCommandFamily::state_read: return "state_read";
    case AgentSecurityCommandFamily::metrics_read: return "metrics_read";
    case AgentSecurityCommandFamily::evidence_read: return "evidence_read";
    case AgentSecurityCommandFamily::recommendation_read: return "recommendation_read";
    case AgentSecurityCommandFamily::control: return "control";
    case AgentSecurityCommandFamily::action_approve: return "action_approve";
    case AgentSecurityCommandFamily::action_cancel: return "action_cancel";
    case AgentSecurityCommandFamily::override: return "override";
    case AgentSecurityCommandFamily::support_bundle_read: return "support_bundle_read";
    case AgentSecurityCommandFamily::policy_read: return "policy_read";
    case AgentSecurityCommandFamily::policy_simulate: return "policy_simulate";
    case AgentSecurityCommandFamily::policy_validate: return "policy_validate";
    case AgentSecurityCommandFamily::policy_apply: return "policy_apply";
    case AgentSecurityCommandFamily::policy_rollback: return "policy_rollback";
    case AgentSecurityCommandFamily::policy_delete: return "policy_delete";
    case AgentSecurityCommandFamily::cluster_inspect: return "cluster_inspect";
    case AgentSecurityCommandFamily::cluster_control: return "cluster_control";
    case AgentSecurityCommandFamily::auth_session_metrics_read: return "auth_session_metrics_read";
    case AgentSecurityCommandFamily::redaction_policy_edit: return "redaction_policy_edit";
    case AgentSecurityCommandFamily::export_policy_approve: return "export_policy_approve";
  }
  return "state_read";
}

const char* AgentSecurityDenialKindName(AgentSecurityDenialKind value) {
  switch (value) {
    case AgentSecurityDenialKind::none: return "none";
    case AgentSecurityDenialKind::missing_security_context: return "missing_security_context";
    case AgentSecurityDenialKind::missing_right: return "missing_right";
    case AgentSecurityDenialKind::hidden_or_restricted_scope: return "hidden_or_restricted_scope";
    case AgentSecurityDenialKind::redacted_payload: return "redacted_payload";
    case AgentSecurityDenialKind::action_permission_denied: return "action_permission_denied";
    case AgentSecurityDenialKind::cluster_authority_required: return "cluster_authority_required";
  }
  return "missing_right";
}

namespace {

AgentSecurityGrantRequirement Req(AgentSecurityRight right,
                                  bool internal_trace_allowed = false,
                                  int alternative_group = 0) {
  AgentSecurityGrantRequirement req;
  req.right = right;
  req.right_name = AgentSecurityRightName(right);
  req.internal_trace_allowed = internal_trace_allowed;
  req.alternative_group = alternative_group;
  return req;
}

AgentSecurityGrantRequirement NamedReq(std::string right, int alternative_group = 0) {
  AgentSecurityGrantRequirement req;
  req.right = ParseAgentSecurityRight(right).value_or(AgentSecurityRight::external_named_right);
  req.right_name = std::move(right);
  req.alternative_group = alternative_group;
  return req;
}

bool HasInternalTraceGrant(const AgentRuntimeContext& context) {
  return Contains(context.trace_tags, "security.bootstrap") ||
         Contains(context.trace_tags, "engine.internal") ||
         Contains(context.trace_tags, "agent.internal") ||
         Contains(context.trace_tags, "internal") ||
         Contains(context.rights, "OBS_AGENT_INTERNAL") ||
         Contains(context.rights, "SB_AGENT_INTERNAL_TRACE");
}

bool GroupGrantsRight(const AgentRuntimeContext& context, AgentSecurityRight right) {
  if (Contains(context.groups, "ROOT")) { return true; }
  const bool ops = Contains(context.groups, "OPS");
  const bool sup = Contains(context.groups, "SUP");
  const bool aud = Contains(context.groups, "AUD");
  const bool dba = Contains(context.groups, "DBA");
  const bool sec = Contains(context.groups, "SEC");
  const bool etl = Contains(context.groups, "ETL");
  const bool sch = Contains(context.groups, "SCH");

  switch (right) {
    case AgentSecurityRight::obs_agent_state_read:
      return ops || sup || aud || dba;
    case AgentSecurityRight::obs_agent_evidence_read:
      return ops || sup || aud || dba || sec;
    case AgentSecurityRight::obs_agent_recommendation_read:
      return ops || sup || dba || etl || sch;
    case AgentSecurityRight::obs_agent_control:
      return ops || sch;
    case AgentSecurityRight::obs_agent_action_approve:
    case AgentSecurityRight::obs_agent_action_cancel:
      return ops || dba;
    case AgentSecurityRight::obs_agent_override:
      return ops;
    case AgentSecurityRight::obs_support_bundle_read:
      return ops || sup || aud;
    case AgentSecurityRight::obs_policy_read:
      return ops || sup || aud || dba || sec;
    case AgentSecurityRight::obs_policy_simulate:
      return ops || dba;
    case AgentSecurityRight::obs_policy_edit_draft:
    case AgentSecurityRight::obs_policy_validate:
    case AgentSecurityRight::obs_policy_approve:
    case AgentSecurityRight::obs_policy_apply:
    case AgentSecurityRight::obs_policy_rollback:
    case AgentSecurityRight::obs_policy_delete:
      return ops;
    case AgentSecurityRight::obs_cluster_health_inspect:
    case AgentSecurityRight::obs_cluster_topology_inspect:
      return ops || sup;
    case AgentSecurityRight::obs_cluster_control:
      return ops;
    case AgentSecurityRight::sec_auth_metrics_read:
    case AgentSecurityRight::sec_redaction_policy_edit:
    case AgentSecurityRight::sec_export_policy_approve:
    case AgentSecurityRight::sec_identity_admin:
      return sec;
    case AgentSecurityRight::internal_agent_trace:
      return HasInternalTraceGrant(context);
    case AgentSecurityRight::external_named_right:
      return false;
  }
  return false;
}

bool HasRequirement(const AgentRuntimeContext& context,
                    const AgentSecurityGrantRequirement& req) {
  if (!context.security_context_present) { return false; }
  if (Contains(context.groups, "ROOT")) { return true; }
  if (req.internal_trace_allowed && HasInternalTraceGrant(context)) { return true; }
  if (req.right == AgentSecurityRight::internal_agent_trace) {
    return HasInternalTraceGrant(context);
  }
  if (Contains(context.rights, req.right_name) ||
      Contains(context.trace_tags, "right:" + req.right_name)) {
    return true;
  }
  if (req.right != AgentSecurityRight::external_named_right &&
      Contains(context.rights, AgentSecurityRightName(req.right))) {
    return true;
  }
  return GroupGrantsRight(context, req.right);
}

std::vector<std::string> ExtractRightTokens(const std::string& text) {
  static const std::vector<std::string> known = {
      "OBS_AGENT_STATE_READ", "OBS_AGENT_EVIDENCE_READ",
      "OBS_AGENT_RECOMMENDATION_READ", "OBS_AGENT_CONTROL",
      "OBS_AGENT_ACTION_APPROVE", "OBS_AGENT_ACTION_CANCEL",
      "OBS_AGENT_OVERRIDE", "OBS_SUPPORT_BUNDLE_READ", "OBS_POLICY_READ",
      "OBS_POLICY_SIMULATE", "OBS_POLICY_EDIT_DRAFT", "OBS_POLICY_VALIDATE",
      "OBS_POLICY_APPROVE", "OBS_POLICY_APPLY", "OBS_POLICY_ROLLBACK",
      "OBS_POLICY_DELETE", "OBS_CLUSTER_HEALTH_INSPECT",
      "OBS_CLUSTER_TOPOLOGY_INSPECT", "OBS_CLUSTER_CONTROL",
      "SEC_AUTH_METRICS_READ", "SEC_REDACTION_POLICY_EDIT",
      "SEC_EXPORT_POLICY_APPROVE", "SEC_IDENTITY_ADMIN"};
  std::vector<std::string> out;
  for (const auto& token : known) {
    if (text.find(token) != std::string::npos && !Contains(out, token)) {
      out.push_back(token);
    }
  }
  return out;
}

}  // namespace

std::vector<AgentSecurityGrantRequirement> RequiredAgentSecurityRightsForCommand(
    AgentSecurityCommandFamily command_family) {
  switch (command_family) {
    case AgentSecurityCommandFamily::state_read:
      return {Req(AgentSecurityRight::obs_agent_state_read)};
    case AgentSecurityCommandFamily::metrics_read:
      return {Req(AgentSecurityRight::obs_agent_state_read)};
    case AgentSecurityCommandFamily::evidence_read:
      return {Req(AgentSecurityRight::obs_agent_evidence_read)};
    case AgentSecurityCommandFamily::recommendation_read:
      return {Req(AgentSecurityRight::obs_agent_recommendation_read)};
    case AgentSecurityCommandFamily::control:
      return {Req(AgentSecurityRight::obs_agent_control)};
    case AgentSecurityCommandFamily::action_approve:
      return {Req(AgentSecurityRight::obs_agent_action_approve)};
    case AgentSecurityCommandFamily::action_cancel:
      return {Req(AgentSecurityRight::obs_agent_action_cancel)};
    case AgentSecurityCommandFamily::override:
      return {Req(AgentSecurityRight::obs_agent_override)};
    case AgentSecurityCommandFamily::support_bundle_read:
      return {Req(AgentSecurityRight::obs_support_bundle_read)};
    case AgentSecurityCommandFamily::policy_read:
      return {Req(AgentSecurityRight::obs_policy_read)};
    case AgentSecurityCommandFamily::policy_simulate:
      return {Req(AgentSecurityRight::obs_policy_simulate)};
    case AgentSecurityCommandFamily::policy_validate:
      return {Req(AgentSecurityRight::obs_policy_validate)};
    case AgentSecurityCommandFamily::policy_apply:
      return {Req(AgentSecurityRight::obs_agent_control),
              Req(AgentSecurityRight::obs_policy_apply)};
    case AgentSecurityCommandFamily::policy_rollback:
      return {Req(AgentSecurityRight::obs_agent_control),
              Req(AgentSecurityRight::obs_policy_rollback)};
    case AgentSecurityCommandFamily::policy_delete:
      return {Req(AgentSecurityRight::obs_policy_delete)};
    case AgentSecurityCommandFamily::cluster_inspect:
      return {Req(AgentSecurityRight::obs_cluster_health_inspect),
              Req(AgentSecurityRight::obs_agent_state_read)};
    case AgentSecurityCommandFamily::cluster_control:
      return {Req(AgentSecurityRight::obs_cluster_control),
              Req(AgentSecurityRight::obs_agent_control)};
    case AgentSecurityCommandFamily::auth_session_metrics_read:
      return {Req(AgentSecurityRight::sec_auth_metrics_read)};
    case AgentSecurityCommandFamily::redaction_policy_edit:
      return {Req(AgentSecurityRight::sec_redaction_policy_edit)};
    case AgentSecurityCommandFamily::export_policy_approve:
      return {Req(AgentSecurityRight::sec_export_policy_approve)};
  }
  return {Req(AgentSecurityRight::obs_agent_state_read)};
}

std::vector<AgentSecurityGrantRequirement> RequiredAgentSecurityRightsForActionContract(
    const AgentActionContractDescriptor& contract) {
  std::vector<AgentSecurityGrantRequirement> requirements;
  const auto tokens = ExtractRightTokens(contract.permission);
  const bool internal_permission =
      contract.permission.find("internal") != std::string::npos ||
      contract.permission.find("internal agent") != std::string::npos ||
      contract.permission.find("internal alert authority") != std::string::npos;
  const bool job_execute_or_admin =
      contract.permission.find("JOB EXECUTE or job admin") != std::string::npos;
  const bool identity_or_session =
      contract.permission.find("SEC_IDENTITY_ADMIN or session control right") !=
      std::string::npos;
  const bool internal_alert_or_override =
      contract.permission.find("internal alert authority or OBS_AGENT_OVERRIDE") !=
      std::string::npos;
  for (const auto& token : tokens) {
    int token_alternative_group = 0;
    if (internal_permission && token == "OBS_AGENT_CONTROL") {
      token_alternative_group = 1;
    }
    if (internal_alert_or_override && token == "OBS_AGENT_OVERRIDE") {
      token_alternative_group = 1;
    }
    if (identity_or_session && token == "SEC_IDENTITY_ADMIN") {
      token_alternative_group = 1;
    }
    requirements.push_back(NamedReq(token, token_alternative_group));
  }
  if (internal_permission) {
    requirements.push_back(Req(AgentSecurityRight::internal_agent_trace, true, 1));
  }
  if (contract.permission.find("redaction rights") != std::string::npos) {
    requirements.push_back(Req(AgentSecurityRight::sec_redaction_policy_edit));
  }
  if (contract.permission.find("export control right") != std::string::npos) {
    requirements.push_back(NamedReq("EXPORT_CONTROL"));
  }
  if (job_execute_or_admin) {
    requirements.push_back(NamedReq("JOB_EXECUTE", 1));
    requirements.push_back(NamedReq("JOB_ADMIN", 1));
  }
  if (contract.permission.find("backup control right") != std::string::npos) {
    requirements.push_back(NamedReq("BACKUP_CONTROL"));
  }
  if (contract.permission.find("restore drill control right") != std::string::npos) {
    requirements.push_back(NamedReq("RESTORE_DRILL_CONTROL"));
  }
  if (contract.permission.find("restore planning right") != std::string::npos) {
    requirements.push_back(NamedReq("RESTORE_PLAN_CONTROL"));
  }
  if (contract.permission.find("filespace lifecycle right") != std::string::npos) {
    requirements.push_back(NamedReq("FILESPACE_LIFECYCLE_CONTROL"));
  }
  if (contract.permission.find("lifecycle truncate right") != std::string::npos) {
    requirements.push_back(NamedReq("FILESPACE_LIFECYCLE_TRUNCATE"));
  }
  if (contract.permission.find("session kill right") != std::string::npos) {
    requirements.push_back(NamedReq("SESSION_KILL"));
  }
  if (identity_or_session) {
    requirements.push_back(NamedReq("SESSION_CONTROL", 1));
  } else if (contract.permission.find("session control right") != std::string::npos) {
    requirements.push_back(NamedReq("SESSION_CONTROL"));
  }
  if (contract.permission.find("cluster") != std::string::npos ||
      contract.risk_class.find("cluster") != std::string::npos ||
      contract.owning_agent.find("cluster_") == 0) {
    requirements.push_back(Req(AgentSecurityRight::obs_cluster_control));
  }
  if (requirements.empty()) {
    if (contract.action_class == AgentActionClass::recommendation) {
      requirements.push_back(Req(AgentSecurityRight::obs_agent_recommendation_read));
    } else if (contract.action_class == AgentActionClass::none) {
      requirements.push_back(Req(AgentSecurityRight::obs_agent_state_read));
    } else {
      requirements.push_back(Req(AgentSecurityRight::obs_agent_control));
    }
  }
  return requirements;
}

AgentSecurityGrantDecision EvaluateAgentSecurityGrant(
    const AgentRuntimeContext& context,
    const std::vector<AgentSecurityGrantRequirement>& requirements,
    std::string detail,
    bool action_permission,
    bool restricted_scope,
    bool cluster_scope) {
  AgentSecurityGrantDecision decision;
  decision.required_rights = requirements;
  decision.detail = std::move(detail);
  decision.evidence_uuid =
      DurableObjectUuidFromKey("agent_security_grant|" + decision.detail + "|" +
                               std::to_string(requirements.size()));
  if (!context.security_context_present) {
    decision.denial = AgentSecurityDenialKind::missing_security_context;
    decision.diagnostic_code = "AGENT.SECURITY_CONTEXT_REQUIRED";
    decision.hides_candidate_rows = action_permission;
    return decision;
  }
  if (restricted_scope) {
    decision.denial = AgentSecurityDenialKind::hidden_or_restricted_scope;
    decision.diagnostic_code = "AGENT.SCOPE_HIDDEN";
    decision.hides_scope = true;
    decision.hides_candidate_rows = true;
    return decision;
  }
  if (cluster_scope && !context.cluster_authority_available) {
    decision.denial = AgentSecurityDenialKind::cluster_authority_required;
    decision.diagnostic_code = "CLUSTER.NOT_AVAILABLE";
    decision.hides_candidate_rows = action_permission;
    return decision;
  }

  std::map<int, bool> alternative_satisfied;
  std::map<int, std::string> alternative_first_missing;
  for (const auto& req : requirements) {
    if (req.alternative_group == 0) {
      if (!HasRequirement(context, req)) {
        decision.denial = action_permission
            ? AgentSecurityDenialKind::action_permission_denied
            : AgentSecurityDenialKind::missing_right;
        decision.diagnostic_code = action_permission
            ? "ACTION.PERMISSION_DENIED"
            : "AGENT.PERMISSION_DENIED";
        decision.missing_right = req.right_name.empty()
            ? AgentSecurityRightName(req.right)
            : req.right_name;
        decision.hides_candidate_rows = action_permission;
        return decision;
      }
      continue;
    }
    const bool has = HasRequirement(context, req);
    alternative_satisfied[req.alternative_group] =
        alternative_satisfied[req.alternative_group] || has;
    if (!has && alternative_first_missing[req.alternative_group].empty()) {
      alternative_first_missing[req.alternative_group] =
          req.right_name.empty() ? AgentSecurityRightName(req.right) : req.right_name;
    }
  }
  for (const auto& entry : alternative_satisfied) {
    if (!entry.second) {
      decision.denial = action_permission
          ? AgentSecurityDenialKind::action_permission_denied
          : AgentSecurityDenialKind::missing_right;
      decision.diagnostic_code = action_permission
          ? "ACTION.PERMISSION_DENIED"
          : "AGENT.PERMISSION_DENIED";
      decision.missing_right = alternative_first_missing[entry.first];
      decision.hides_candidate_rows = action_permission;
      return decision;
    }
  }
  decision.allowed = true;
  decision.denial = AgentSecurityDenialKind::none;
  decision.diagnostic_code = "AGENT.PERMISSION_GRANTED";
  return decision;
}

AgentSecurityGrantDecision EvaluateAgentCommandGrant(
    const AgentRuntimeContext& context,
    AgentSecurityCommandFamily command_family,
    std::string detail,
    bool restricted_scope,
    bool cluster_scope) {
  return EvaluateAgentSecurityGrant(
      context,
      RequiredAgentSecurityRightsForCommand(command_family),
      detail.empty() ? AgentSecurityCommandFamilyName(command_family) : std::move(detail),
      false,
      restricted_scope,
      cluster_scope);
}

AgentSecurityGrantDecision EvaluateAgentActionContractGrant(
    const AgentRuntimeContext& context,
    const AgentActionContractDescriptor& contract,
    bool restricted_scope) {
  const bool cluster_scope = contract.cluster_scoped ||
                             contract.risk_class.find("cluster") != std::string::npos ||
                             contract.owning_agent.find("cluster_") == 0;
  return EvaluateAgentSecurityGrant(
      context,
      RequiredAgentSecurityRightsForActionContract(contract),
      contract.owning_agent + ":" + contract.action_id,
      true,
      restricted_scope,
      cluster_scope);
}

bool AgentContextHasRight(const AgentRuntimeContext& context, const std::string& right) {
  if (right.empty()) { return false; }
  return EvaluateAgentSecurityGrant(context, {NamedReq(right)}, right).allowed;
}

AgentRuntimeStatus ValidateAgentSecurity(const AgentRuntimeContext& context,
                                         const AgentTypeDescriptor& descriptor,
                                         const std::string& operation_class) {
  if (!context.security_context_present) {
    return AgentError("SB_AGENT_SECURITY.CONTEXT_REQUIRED", descriptor.type_id);
  }
  std::vector<AgentSecurityGrantRequirement> requirements;
  if (operation_class == "inspect") {
    requirements = RequiredAgentSecurityRightsForCommand(
        AgentSecurityCommandFamily::state_read);
  } else if (operation_class == "evidence") {
    requirements = RequiredAgentSecurityRightsForCommand(
        AgentSecurityCommandFamily::evidence_read);
  } else if (operation_class == "policy") {
    requirements = RequiredAgentSecurityRightsForCommand(
        AgentSecurityCommandFamily::policy_read);
  } else if (operation_class == "override") {
    requirements = RequiredAgentSecurityRightsForCommand(
        AgentSecurityCommandFamily::override);
  } else if (operation_class == "recommendation") {
    requirements = RequiredAgentSecurityRightsForCommand(
        AgentSecurityCommandFamily::recommendation_read);
  } else {
    for (const auto& right : descriptor.required_rights) {
      requirements.push_back(NamedReq(right));
    }
  }
  const auto decision = EvaluateAgentSecurityGrant(
      context, requirements, descriptor.type_id + ":" + operation_class);
  if (!decision.allowed) {
    if (decision.denial == AgentSecurityDenialKind::cluster_authority_required) {
      return AgentError("SB_AGENT_SECURITY.CLUSTER_AUTHORITY_REQUIRED",
                        descriptor.type_id);
    }
    return AgentError("SB_AGENT_SECURITY.RIGHT_REQUIRED", decision.missing_right);
  }
  return AgentOk();
}

AgentTimeAuthorityDecision ResolveAgentTimeAuthority(const AgentRuntimeContext& context,
                                                     bool cluster_scoped_action) {
  AgentTimeAuthorityDecision decision;
  decision.monotonic_reference_microseconds = context.monotonic_now_microseconds;
  decision.wall_reference_microseconds = context.wall_now_microseconds ? context.wall_now_microseconds : NowMicros();
  if (!cluster_scoped_action) {
    decision.mode = AgentTimeAuthorityMode::single_node_os_time;
    decision.clock_quality = "os_wall_clock_plus_monotonic";
    decision.status = AgentOk();
    return decision;
  }
  decision.mode = AgentTimeAuthorityMode::cluster_majority_time;
  decision.cluster_reference_microseconds = context.cluster_now_microseconds;
  if (!context.cluster_authority_available || !context.cluster_time_majority_available || decision.cluster_reference_microseconds == 0) {
    decision.clock_quality = "cluster_time_unavailable";
    decision.status = AgentError("SB_AGENT_TIME.CLUSTER_MAJORITY_TIME_REQUIRED");
    return decision;
  }
  decision.clock_quality = "cluster_majority_coordinated";
  decision.status = AgentOk();
  return decision;
}

// SEARCH_KEY: SB_AGENT_METRIC_DEPENDENCY_FAIL_CLOSED
AgentDependencyEvaluation EvaluateAgentMetricDependencies(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    const std::vector<AgentMetricObservation>& observations,
    const std::vector<AgentPolicyDependencyState>& policy_state,
    bool enforce_policy_state,
    const scratchbird::core::metrics::MetricRegistry& registry) {
  AgentDependencyEvaluation evaluation;
  evaluation.status = AgentOk();

  bool local_dependency_seen = false;
  bool cluster_dependency_seen = false;
  bool local_required_ok = false;

  for (const auto& dep : descriptor.metric_dependencies) {
    if (dep.cluster_only) {
      cluster_dependency_seen = true;
      if (!context.cluster_authority_available) {
        evaluation.cluster_path_failed_closed = true;
        evaluation.diagnostics.push_back(MakeMetricDependencyDiagnostic(
            descriptor,
            "SB_AGENT_METRICS.CLUSTER_AUTHORITY_REQUIRED",
            dep,
            {},
            {},
            false,
            true));
        continue;
      }
    } else {
      local_dependency_seen = true;
    }

    const auto* observation = FindMetricObservation(observations, dep.metric_family);
    if (observation == nullptr || !observation->present) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.REQUIRED_METRIC_MISSING",
            dep,
            observation == nullptr ? std::string{} : observation->evidence_uuid,
            observation == nullptr ? std::string{} : observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }

    if (!dep.namespace_prefix.empty() &&
        observation->namespace_path.rfind(dep.namespace_prefix, 0) != 0) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.NAMESPACE_SCHEMA_INCOMPATIBLE",
            dep,
            observation->evidence_uuid,
            observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }
    if (!observation->schema_compatible) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.NAMESPACE_SCHEMA_INCOMPATIBLE",
            dep,
            observation->evidence_uuid,
            observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }
    if (!observation->scope_compatible) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.SCOPE_INCOMPATIBLE",
            dep,
            observation->evidence_uuid,
            observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }
    if (!observation->trusted) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.REQUIRED_METRIC_UNTRUSTED",
            dep,
            observation->evidence_uuid,
            observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }
    if (dep.required_source_quality == AgentMetricSourceQuality::cluster_confirmed &&
        observation->source_quality != AgentMetricSourceQuality::cluster_confirmed) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.REQUIRED_METRIC_QUALITY_INSUFFICIENT",
            dep,
            observation->evidence_uuid,
            observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }
    if (observation->source_quality == AgentMetricSourceQuality::unknown) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.REQUIRED_METRIC_QUALITY_INSUFFICIENT",
            dep,
            observation->evidence_uuid,
            observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }
    if (dep.max_freshness_microseconds > 0 &&
        observation->age_microseconds > dep.max_freshness_microseconds) {
      if (dep.required) {
        return FailMetricDependencyEvaluation(
            descriptor,
            "SB_AGENT_METRICS.REQUIRED_METRIC_STALE",
            dep,
            observation->evidence_uuid,
            observation->snapshot_id);
      }
      AddOptionalSuppressedDiagnostic(&evaluation, descriptor, dep, observation);
      continue;
    }

    if (!dep.cluster_only) { local_required_ok = local_required_ok || dep.required; }
  }

  if (!local_dependency_seen && cluster_dependency_seen &&
      evaluation.cluster_path_failed_closed) {
    evaluation.status = AgentError("SB_AGENT_METRICS.CLUSTER_AUTHORITY_REQUIRED",
                                   descriptor.type_id);
    evaluation.failed_closed = true;
    return evaluation;
  }
  evaluation.local_projection_valid = local_dependency_seen &&
                                      (!cluster_dependency_seen ||
                                       !evaluation.cluster_path_failed_closed ||
                                       local_required_ok);

  if (enforce_policy_state) {
    for (const auto& policy_family : RequiredPolicyFamiliesForAgent(descriptor)) {
      const auto* state = FindPolicyDependencyState(policy_state, policy_family);
      if (state == nullptr || !state->present) {
        return FailDependencyEvaluation(
            descriptor,
            "SB_AGENT_POLICY_DEPENDENCY.MISSING",
            "policy",
            policy_family,
            state == nullptr ? std::string{} : state->evidence_uuid);
      }
      if (!state->valid) {
        return FailDependencyEvaluation(
            descriptor,
            "SB_AGENT_POLICY_DEPENDENCY.INVALID",
            "policy",
            policy_family,
            state->evidence_uuid);
      }
      if (!state->scope_compatible || state->scope != descriptor.scope) {
        return FailDependencyEvaluation(
            descriptor,
            "SB_AGENT_POLICY_DEPENDENCY.SCOPE_INCOMPATIBLE",
            "policy",
            policy_family,
            state->evidence_uuid);
      }
    }
  }

  (void)registry;
  return evaluation;
}

AgentRuntimeStatus ResolveAgentMetricDependencies(const AgentTypeDescriptor& descriptor,
                                                  const AgentRuntimeContext& context,
                                                  const scratchbird::core::metrics::MetricRegistry& registry) {
  for (const auto& dep : descriptor.metric_dependencies) {
    const auto* metric = registry.FindDescriptorOrAlias(dep.metric_family);
    if (dep.required && metric == nullptr) { return AgentError("SB_AGENT_METRICS.REQUIRED_METRIC_MISSING", dep.metric_family); }
    if (metric != nullptr && dep.cluster_only && !context.cluster_authority_available) {
      return AgentError("SB_AGENT_METRICS.CLUSTER_AUTHORITY_REQUIRED", dep.metric_family);
    }
    if (metric != nullptr && !dep.namespace_prefix.empty() && metric->namespace_path.rfind(dep.namespace_prefix, 0) != 0) {
      return AgentError("SB_AGENT_METRICS.NAMESPACE_MISMATCH", dep.metric_family);
    }
  }
  return AgentOk();
}

// SEARCH_KEY: SB_AGENT_NONCLUSTER_TICK_HEALTH_COVERAGE
AgentTickHealthResult BuildNonClusterAgentTickHealthSnapshot(
    const AgentTickHealthRequest& request,
    const scratchbird::core::metrics::MetricRegistry& registry) {
  AgentTickHealthResult result;
  const auto registry_status = ValidateCanonicalAgentRegistry();
  if (!registry_status.ok) {
    result.status = registry_status;
    return result;
  }

  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (!IsNonClusterRuntimeAgent(descriptor)) { continue; }

    AgentPolicy baseline = BaselinePolicyForAgent(descriptor);
    baseline.policy_generation = request.policy_generation;
    const AgentPolicy* policy = &baseline;
    if (request.use_explicit_policy_state) {
      policy = FindExplicitPolicyForAgent(descriptor, request);
      if (policy == nullptr) {
        AgentPolicy missing_policy = baseline;
        missing_policy.policy_uuid =
            DurableObjectUuidFromKey("agent_policy_missing|" + descriptor.type_id + "|" +
                                     descriptor.scope);
        result.records.push_back(FailedClosedTickHealthRecord(
            descriptor, missing_policy, request.context,
            AgentError("SB_AGENT_POLICY.MISSING", descriptor.type_id)));
        continue;
      }
    }

    const bool suppressed = Contains(request.suppressed_agent_type_ids,
                                     descriptor.type_id);
    if (suppressed) {
      auto record = BaseTickHealthRecord(
          descriptor, *policy, request.context,
          AgentTickHealthClass::suppressed,
          "SB_AGENT_TICK_HEALTH.SUPPRESSED",
          descriptor.type_id);
      record.selected = true;
      record.runnable = false;
      record.failed_closed = false;
      record.suppressed = true;
      record.lifecycle_state = AgentLifecycleState::paused;
      record.action_class = AgentActionClass::refusal;
      record.action_result_class = AgentActionResultClass::suppressed;
      record.action_evidence_published = true;
      result.records.push_back(std::move(record));
      continue;
    }

    const auto policy_status = ValidateAgentPolicy(*policy, descriptor);
    if (!policy_status.ok) {
      result.records.push_back(FailedClosedTickHealthRecord(
          descriptor, *policy, request.context, policy_status));
      continue;
    }

    const auto security_status =
        ValidateAgentSecurity(request.context, descriptor, "inspect");
    if (!security_status.ok) {
      result.records.push_back(FailedClosedTickHealthRecord(
          descriptor, *policy, request.context, security_status));
      continue;
    }

    bool cluster_path_failed_closed = false;
    std::vector<AgentDependencyDiagnostic> dependency_diagnostics;
    const auto metric_status = ResolveNonClusterTickMetricDependencies(
        descriptor, request, registry, &cluster_path_failed_closed,
        &dependency_diagnostics);
    if (!metric_status.ok) {
      result.records.push_back(FailedClosedTickHealthRecord(
          descriptor, *policy, request.context, metric_status,
          std::move(dependency_diagnostics)));
      continue;
    }

    if (request.resource_budget.has_value()) {
      const auto budget_decision = EvaluateAgentResourceBudget(
          descriptor, *policy, request.context, *request.resource_budget);
      if (budget_decision.decision != AgentResourceBudgetDecisionKind::allow) {
        auto record = ResourceBudgetTickHealthRecord(
            descriptor, *policy, request.context, budget_decision);
        record.dependency_diagnostics = std::move(dependency_diagnostics);
        record.cluster_path_failed_closed = cluster_path_failed_closed;
        result.records.push_back(std::move(record));
        continue;
      }
    }

    auto record = ClassifyRunnableTickHealthRecord(
        descriptor, *policy, request.context, cluster_path_failed_closed);
    record.dependency_diagnostics = std::move(dependency_diagnostics);
    result.records.push_back(std::move(record));
  }

  result.status = AgentOk();
  return result;
}

AgentEvidenceRecord MakeAgentEvidence(const AgentRuntimeContext& context,
                                      const AgentTypeDescriptor& descriptor,
                                      const AgentInstanceRecord& instance,
                                      std::string evidence_kind,
                                      std::string diagnostic_code,
                                      std::string detail) {
  AgentEvidenceRecord evidence;
  evidence.evidence_uuid = DurableObjectUuidFromKey(
      "agent_evidence|" + descriptor.type_id + "|" +
      std::to_string(context.wall_now_microseconds ? context.wall_now_microseconds : NowMicros()));
  evidence.agent_type_id = descriptor.type_id;
  evidence.instance_uuid = instance.instance_uuid;
  evidence.evidence_kind = std::move(evidence_kind);
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.detail = std::move(detail);
  evidence.created_at_microseconds = context.wall_now_microseconds ? context.wall_now_microseconds : NowMicros();
  evidence.expires_at_microseconds = evidence.created_at_microseconds + 86400000000ull;
  return evidence;
}

AgentRuntimeStatus ValidateEvidenceVisibility(const AgentRuntimeContext& context,
                                              const AgentEvidenceRecord& evidence) {
  if (evidence.redaction_class == "restricted" && !AgentContextHasRight(context, "OBS_AGENT_EVIDENCE_READ")) {
    return AgentError("SB_AGENT_EVIDENCE.VISIBILITY_RIGHT_REQUIRED", evidence.evidence_uuid);
  }
  return AgentOk();
}

AgentEvidenceRecord RedactAgentEvidence(const AgentEvidenceRecord& evidence,
                                        const AgentRuntimeContext& context) {
  AgentEvidenceRecord redacted = evidence;
  if (ValidateEvidenceVisibility(context, evidence).ok) { return redacted; }
  redacted.detail = "redacted";
  redacted.diagnostic_code = "SB_AGENT_EVIDENCE.REDACTED";
  return redacted;
}

AgentEvidenceRedactionDecision RedactAgentEvidenceForSecurity(
    const AgentEvidenceRecord& evidence,
    const AgentRuntimeContext& context,
    bool support_bundle_view,
    bool restricted_scope) {
  AgentEvidenceRedactionDecision decision;
  decision.evidence = evidence;
  const auto family = support_bundle_view
      ? AgentSecurityCommandFamily::support_bundle_read
      : AgentSecurityCommandFamily::evidence_read;
  decision.grant = EvaluateAgentCommandGrant(
      context, family, evidence.evidence_uuid, restricted_scope);
  if (!decision.grant.allowed) {
    decision.visible = false;
    decision.redacted = true;
    decision.evidence.detail.clear();
    decision.evidence.diagnostic_code = support_bundle_view
        ? "SUPPORT_BUNDLE.REDACTED"
        : "AGENT.EVIDENCE_REDACTED";
    decision.grant.denial = AgentSecurityDenialKind::redacted_payload;
    decision.grant.payload_redacted = true;
    decision.grant.redaction_evidence.push_back(evidence.evidence_uuid);
    decision.grant.diagnostic_code = support_bundle_view
        ? "SUPPORT_BUNDLE.REDACTED"
        : "AGENT.EVIDENCE_REDACTED";
    return decision;
  }
  const bool restricted_payload =
      evidence.redaction_class == "restricted" ||
      evidence.redaction_class == "secret" ||
      evidence.redaction_class == "security_sensitive";
  const bool support_safe =
      support_bundle_view && evidence.redaction_class == "support_safe";
  if (restricted_payload && !support_safe &&
      !AgentContextHasRight(context, "SEC_REDACTION_POLICY_EDIT")) {
    decision.visible = true;
    decision.redacted = true;
    decision.evidence.detail = "redacted";
    decision.evidence.diagnostic_code = support_bundle_view
        ? "SUPPORT_BUNDLE.REDACTED"
        : "AGENT.EVIDENCE_REDACTED";
    decision.grant.allowed = false;
    decision.grant.denial = AgentSecurityDenialKind::redacted_payload;
    decision.grant.diagnostic_code = decision.evidence.diagnostic_code;
    decision.grant.payload_redacted = true;
    decision.grant.redaction_evidence.push_back(evidence.evidence_uuid);
    return decision;
  }
  decision.visible = true;
  decision.redacted = false;
  return decision;
}

AgentPolicyRedactionDecision RedactAgentPolicyForSecurity(
    const AgentPolicyInspectionRecord& policy,
    const AgentRuntimeContext& context,
    bool restricted_scope) {
  AgentPolicyRedactionDecision decision;
  decision.policy = policy;
  decision.grant = EvaluateAgentCommandGrant(
      context, AgentSecurityCommandFamily::policy_read, policy.policy_uuid,
      restricted_scope || policy.restricted_scope);
  if (!decision.grant.allowed) {
    decision.visible = false;
    decision.redacted = true;
    decision.policy.policy_body.clear();
    return decision;
  }
  decision.visible = true;
  decision.redacted = !AgentContextHasRight(context, "OBS_POLICY_READ_BODY");
  if (decision.redacted) {
    decision.policy.policy_body = "redacted";
    decision.grant.denial = AgentSecurityDenialKind::redacted_payload;
    decision.grant.diagnostic_code = "POLICY.BODY_REDACTED";
    decision.grant.payload_redacted = true;
    decision.grant.redaction_evidence.push_back(policy.policy_uuid);
  }
  return decision;
}

AgentMetricRedactionDecision RedactAgentMetricForSecurity(
    const AgentMetricInspectionRecord& metric,
    const AgentRuntimeContext& context) {
  AgentMetricRedactionDecision decision;
  decision.metric = metric;
  AgentSecurityCommandFamily family = AgentSecurityCommandFamily::metrics_read;
  if (metric.security_sensitive) {
    family = AgentSecurityCommandFamily::auth_session_metrics_read;
  }
  decision.grant = EvaluateAgentCommandGrant(
      context, family, metric.metric_family, metric.restricted_scope);
  if (!decision.grant.allowed) {
    decision.visible = false;
    decision.redacted = true;
    decision.metric.raw_value.clear();
    decision.metric.redacted_value = "redacted";
    decision.grant.payload_redacted = true;
    decision.grant.redaction_evidence.push_back(metric.metric_family);
    return decision;
  }
  decision.visible = true;
  decision.redacted = metric.security_sensitive &&
                      !AgentContextHasRight(context, "SEC_AUTH_METRICS_READ");
  if (decision.redacted) {
    decision.metric.raw_value.clear();
    decision.metric.redacted_value = "redacted";
    decision.grant.denial = AgentSecurityDenialKind::redacted_payload;
    decision.grant.diagnostic_code = "METRIC.REDACTED";
    decision.grant.payload_redacted = true;
    decision.grant.redaction_evidence.push_back(metric.metric_family);
  }
  return decision;
}

AgentActionRedactionDecision RedactAgentActionForSecurity(
    const AgentActionInspectionRecord& action,
    const AgentRuntimeContext& context) {
  AgentActionRedactionDecision decision;
  decision.action = action;
  decision.grant = EvaluateAgentCommandGrant(
      context, AgentSecurityCommandFamily::recommendation_read,
      action.owning_agent + ":" + action.action_id, action.restricted_scope);
  if (!decision.grant.allowed) {
    decision.visible = false;
    decision.redacted = true;
    decision.action.action_uuid.clear();
    decision.action.actor_principal_uuid.clear();
    decision.action.detail.clear();
    decision.grant.hides_candidate_rows = true;
    return decision;
  }
  decision.visible = true;
  decision.redacted = action.security_sensitive &&
                      !AgentContextHasRight(context, "SEC_AUTH_METRICS_READ");
  if (decision.redacted) {
    decision.action.actor_principal_uuid = "redacted";
    decision.action.detail = "redacted";
    decision.grant.denial = AgentSecurityDenialKind::redacted_payload;
    decision.grant.diagnostic_code = "ACTION.REDACTED";
    decision.grant.payload_redacted = true;
    decision.grant.redaction_evidence.push_back(action.action_uuid);
  }
  return decision;
}

std::vector<AgentActuatorCapability> DefaultActuatorCapabilities() {
  return {{"metrics_registry", true, false, false, {"repair_descriptor_cache", "reload_retention"}, {"OBS_AGENT_CONTROL"}},
          {"memory_governor", true, false, false, {"tighten_budget", "release_idle"}, {"OBS_AGENT_CONTROL"}},
          {"filespace_capacity_manager", true, false, false, {"filespace_growth_request"}, {"OBS_AGENT_CONTROL"}},
          {"page_allocation_manager", true, false, false, {"page_preallocation_request", "page_relocation_request", "filespace_shrink_readiness_notification"}, {"OBS_AGENT_CONTROL"}},
          {"index_health_manager", true, false, false, {"index_delta_merge_request", "index_rebuild_request", "index_shadow_build_request"}, {"OBS_AGENT_CONTROL"}},
          {"archive_runtime", true, false, false, {"advance_archive", "throttle_archive"}, {"OBS_AGENT_CONTROL"}},
          {"alert_runtime", true, false, false, {"fire_alert", "suppress_alert"}, {"OBS_AGENT_CONTROL"}},
          {"cluster_autoscale", false, true, true, {"request_scale_out", "request_scale_in"}, {"OBS_CLUSTER_CONTROL"}},
          {"remote_query_router", false, true, true, {"adjust_route_weight"}, {"OBS_CLUSTER_CONTROL"}}};
}

std::optional<AgentActuatorCapability> FindActuatorCapability(const std::string& actuator_id) {
  for (const auto& capability : DefaultActuatorCapabilities()) {
    if (capability.actuator_id == actuator_id) { return capability; }
  }
  return std::nullopt;
}

AgentRuntimeStatus ValidateActuatorCapability(const AgentRuntimeContext& context,
                                              const AgentActionRequest& action) {
  const auto capability = FindActuatorCapability(action.actuator_id);
  if (!capability.has_value() || !capability->registered) { return AgentError("SB_AGENT_ACTUATOR.UNAVAILABLE", action.actuator_id); }
  if (capability->degraded) { return AgentError("SB_AGENT_ACTUATOR.DEGRADED", action.actuator_id); }
  if (capability->cluster_only && !context.cluster_authority_available) {
    return AgentError("SB_AGENT_ACTUATOR.CLUSTER_AUTHORITY_REQUIRED", action.actuator_id);
  }
  if (!Contains(capability->operations, action.operation_id)) {
    return AgentError("SB_AGENT_ACTUATOR.OPERATION_UNSUPPORTED", action.operation_id);
  }
  for (const auto& right : capability->required_rights) {
    if (!AgentContextHasRight(context, right)) { return AgentError("SB_AGENT_ACTUATOR.RIGHT_REQUIRED", right); }
  }
  return AgentOk();
}

// SEARCH_KEY: SB_AGENT_ACTION_ACTUATOR_AUTHORITY
AgentActuatorAuthorityDomain ActuatorAuthorityDomainForId(const std::string& actuator_id) {
  if (actuator_id == "metrics_registry" || actuator_id == "metrics_rollup") {
    return AgentActuatorAuthorityDomain::metrics;
  }
  if (actuator_id == "storage_manager" || actuator_id == "storage_health_evidence") {
    return AgentActuatorAuthorityDomain::storage;
  }
  if (actuator_id == "filespace_lifecycle" ||
      actuator_id == "filespace_capacity_manager") {
    return AgentActuatorAuthorityDomain::filespace_lifecycle;
  }
  if (actuator_id == "page_manager" || actuator_id == "page_allocation_manager") {
    return AgentActuatorAuthorityDomain::page;
  }
  if (actuator_id == "memory_allocator" || actuator_id == "executor_memory" ||
      actuator_id == "cache_manager") {
    return AgentActuatorAuthorityDomain::memory;
  }
  if (actuator_id == "optimizer_cost_registry" ||
      actuator_id == "optimizer_feedback_registry") {
    return AgentActuatorAuthorityDomain::optimizer;
  }
  if (actuator_id == "admission_gate") {
    return AgentActuatorAuthorityDomain::admission;
  }
  if (actuator_id == "parser_supervisor") {
    return AgentActuatorAuthorityDomain::parser;
  }
  if (actuator_id == "transaction_manager") {
    return AgentActuatorAuthorityDomain::transaction;
  }
  if (actuator_id == "cleanup_manager") {
    return AgentActuatorAuthorityDomain::cleanup;
  }
  if (actuator_id == "policy_registry" || actuator_id == "route_policy_registry") {
    return AgentActuatorAuthorityDomain::policy;
  }
  if (actuator_id == "support_bundle_subsystem") {
    return AgentActuatorAuthorityDomain::support;
  }
  if (actuator_id == "job_control_manager" || actuator_id == "job_manager" ||
      actuator_id == "scheduler") {
    return AgentActuatorAuthorityDomain::job;
  }
  if (actuator_id == "backup_subsystem") {
    return AgentActuatorAuthorityDomain::backup;
  }
  if (actuator_id == "archive_manager" || actuator_id == "archive_subsystem") {
    return AgentActuatorAuthorityDomain::archive;
  }
  if (actuator_id == "restore_subsystem") {
    return AgentActuatorAuthorityDomain::restore;
  }
  if (actuator_id == "pitr_subsystem") {
    return AgentActuatorAuthorityDomain::pitr;
  }
  if (actuator_id == "security_manager" || actuator_id == "security_audit") {
    return AgentActuatorAuthorityDomain::security;
  }
  if (actuator_id == "session_control_manager" || actuator_id == "session_manager") {
    return AgentActuatorAuthorityDomain::session;
  }
  if (actuator_id == "alert_delivery") {
    return AgentActuatorAuthorityDomain::alert;
  }
  if (actuator_id == "export_subsystem") {
    return AgentActuatorAuthorityDomain::export_subsystem;
  }
  if (actuator_id == "cluster_manager" ||
      actuator_id == "cluster_upgrade_subsystem") {
    return AgentActuatorAuthorityDomain::cluster_provider;
  }
  if (actuator_id == "operator_manager") {
    return AgentActuatorAuthorityDomain::operator_control;
  }
  if (actuator_id.find("evidence") != std::string::npos) {
    return AgentActuatorAuthorityDomain::evidence;
  }
  return AgentActuatorAuthorityDomain::unknown;
}

bool IsDirectForbiddenEngineAuthorityActuator(const std::string& actuator_id) {
  static const std::set<std::string> forbidden = {
      "transaction_finality",
      "catalog_truth",
      "catalog_manager",
      "parser_admission",
      "sblr_executor",
      "security_authority"};
  return forbidden.find(actuator_id) != forbidden.end();
}

AgentActuatorAuthorityDescriptor AuthorityFromContract(
    const AgentActionContractDescriptor& contract,
    bool route_registered = true) {
  AgentActuatorAuthorityDescriptor descriptor;
  descriptor.action_id = contract.action_id;
  descriptor.owning_agent = contract.owning_agent;
  descriptor.actuator_id = contract.actuator;
  descriptor.domain = ActuatorAuthorityDomainForId(contract.actuator);
  descriptor.cluster_scoped = contract.cluster_scoped;
  descriptor.route_registered = route_registered;
  descriptor.owns_forbidden_engine_authority =
      IsDirectForbiddenEngineAuthorityActuator(contract.actuator);
  return descriptor;
}

AgentActuatorAuthorityDescriptor ClusterAuthority(
    std::string owning_agent,
    std::string action_id,
    std::string actuator_id) {
  AgentActionContractDescriptor contract;
  contract.owning_agent = std::move(owning_agent);
  contract.action_id = std::move(action_id);
  contract.actuator = std::move(actuator_id);
  contract.cluster_scoped = true;
  return AuthorityFromContract(contract, true);
}

const char* AgentActuatorAuthorityDomainName(AgentActuatorAuthorityDomain value) {
  switch (value) {
    case AgentActuatorAuthorityDomain::metrics: return "metrics";
    case AgentActuatorAuthorityDomain::storage: return "storage";
    case AgentActuatorAuthorityDomain::filespace_lifecycle: return "filespace_lifecycle";
    case AgentActuatorAuthorityDomain::page: return "page";
    case AgentActuatorAuthorityDomain::memory: return "memory";
    case AgentActuatorAuthorityDomain::optimizer: return "optimizer";
    case AgentActuatorAuthorityDomain::admission: return "admission";
    case AgentActuatorAuthorityDomain::parser: return "parser";
    case AgentActuatorAuthorityDomain::transaction: return "transaction";
    case AgentActuatorAuthorityDomain::cleanup: return "cleanup";
    case AgentActuatorAuthorityDomain::policy: return "policy";
    case AgentActuatorAuthorityDomain::support: return "support";
    case AgentActuatorAuthorityDomain::job: return "job";
    case AgentActuatorAuthorityDomain::backup: return "backup";
    case AgentActuatorAuthorityDomain::archive: return "archive";
    case AgentActuatorAuthorityDomain::restore: return "restore";
    case AgentActuatorAuthorityDomain::pitr: return "pitr";
    case AgentActuatorAuthorityDomain::security: return "security";
    case AgentActuatorAuthorityDomain::session: return "session";
    case AgentActuatorAuthorityDomain::alert: return "alert";
    case AgentActuatorAuthorityDomain::export_subsystem: return "export_subsystem";
    case AgentActuatorAuthorityDomain::cluster_provider: return "cluster_provider";
    case AgentActuatorAuthorityDomain::operator_control: return "operator_control";
    case AgentActuatorAuthorityDomain::evidence: return "evidence";
    case AgentActuatorAuthorityDomain::unknown: return "unknown";
  }
  return "unknown";
}

std::vector<AgentActuatorAuthorityDescriptor> AgentActuatorAuthorityRegistry() {
  std::vector<AgentActuatorAuthorityDescriptor> registry;
  for (const auto& contract : AgentActionContractRegistry()) {
    registry.push_back(AuthorityFromContract(contract, true));
  }
  registry.push_back(ClusterAuthority(
      "cluster_autoscale_manager", "recommend_scale_up", "cluster_manager"));
  registry.push_back(ClusterAuthority(
      "cluster_autoscale_manager", "recommend_scale_down", "cluster_manager"));
  registry.push_back(ClusterAuthority(
      "distributed_query_metrics_agent", "publish_fragment_metrics",
      "metrics_registry"));
  registry.push_back(ClusterAuthority(
      "remote_query_routing_agent", "recommend_route_weight",
      "route_policy_registry"));
  registry.push_back(ClusterAuthority(
      "cluster_scheduler_manager", "recommend_job_placement", "scheduler"));
  registry.push_back(ClusterAuthority(
      "cluster_scheduler_manager", "route_cluster_job", "scheduler"));
  registry.push_back(ClusterAuthority(
      "cluster_upgrade_manager", "approve_upgrade_step",
      "cluster_upgrade_subsystem"));
  registry.push_back(ClusterAuthority(
      "cluster_upgrade_manager", "block_upgrade_step",
      "cluster_upgrade_subsystem"));
  return registry;
}

std::optional<AgentActuatorAuthorityDescriptor> FindAgentActuatorAuthority(
    const std::string& owning_agent,
    const std::string& action_id) {
  for (const auto& authority : AgentActuatorAuthorityRegistry()) {
    if (authority.owning_agent == owning_agent &&
        authority.action_id == action_id) {
      return authority;
    }
  }
  return std::nullopt;
}

AgentRuntimeStatus ValidateAgentActuatorAuthorityRegistry() {
  std::set<std::string> seen;
  for (const auto& authority : AgentActuatorAuthorityRegistry()) {
    if (authority.owning_agent.empty() || authority.action_id.empty() ||
        authority.actuator_id.empty()) {
      return AgentError(
          "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.DESCRIPTOR_INCOMPLETE",
          authority.owning_agent + ":" + authority.action_id);
    }
    const std::string key = authority.owning_agent + "|" + authority.action_id;
    if (!seen.insert(key).second) {
      return AgentError("SB_AGENT_ACTION_ACTUATOR_AUTHORITY.DUPLICATE", key);
    }
    if (authority.domain == AgentActuatorAuthorityDomain::unknown) {
      return AgentError(
          "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.DOMAIN_UNKNOWN",
          authority.actuator_id);
    }
    if (!authority.route_registered) {
      return AgentError(
          "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ROUTE_UNREGISTERED",
          authority.actuator_id);
    }
    if (authority.owns_forbidden_engine_authority) {
      return AgentError(
          "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.FORBIDDEN_DIRECT_ENGINE_AUTHORITY",
          authority.actuator_id);
    }
  }
  for (const auto& contract : AgentActionContractRegistry()) {
    const auto authority =
        FindAgentActuatorAuthority(contract.owning_agent, contract.action_id);
    if (!authority.has_value()) {
      return AgentError("SB_AGENT_ACTION_ACTUATOR_AUTHORITY.CONTRACT_MISSING",
                        contract.owning_agent + ":" + contract.action_id);
    }
    if (authority->actuator_id != contract.actuator) {
      return AgentError("SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ACTUATOR_MISMATCH",
                        contract.owning_agent + ":" + contract.action_id);
    }
  }
  return AgentOk();
}

AgentActuatorAuthorityDecision EvaluateAgentActionActuatorAuthority(
    const AgentActionContractDescriptor& contract,
    const AgentActionContractEvaluationRequest& request) {
  AgentActuatorAuthorityDecision decision;
  decision.requested_actuator_id =
      request.actuator_route_id.empty() ? contract.actuator : request.actuator_route_id;

  const auto authority =
      FindAgentActuatorAuthority(contract.owning_agent, contract.action_id);
  if (!authority.has_value()) {
    decision.status = AgentError(
        "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.CONTRACT_MISSING",
        contract.owning_agent + ":" + contract.action_id);
    return decision;
  }

  decision.expected_actuator_id = authority->actuator_id;
  decision.domain = authority->domain;
  decision.cluster_scoped = authority->cluster_scoped;
  decision.route_registered = authority->route_registered;

  if (authority->cluster_scoped &&
      (!request.context.cluster_authority_available ||
       request.context.standalone_edition)) {
    decision.status = AgentError("SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
                                 authority->owning_agent + ":" + authority->action_id);
    return decision;
  }
  if (!authority->cluster_scoped) {
    const auto canonical =
        FindAgentActionContract(contract.owning_agent, contract.action_id);
    if (!canonical.has_value()) {
      decision.status = AgentError(
          "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.CONTRACT_MISSING",
          contract.owning_agent + ":" + contract.action_id);
      return decision;
    }
    if (canonical->owning_agent != contract.owning_agent ||
        canonical->actuator != contract.actuator ||
        canonical->policy_gate != contract.policy_gate ||
        canonical->evidence_kind != contract.evidence_kind) {
      decision.status = AgentError(
          "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.CONTRACT_MISMATCH",
          contract.owning_agent + ":" + contract.action_id);
      return decision;
    }
  }
  if (IsDirectForbiddenEngineAuthorityActuator(decision.requested_actuator_id)) {
    decision.status = AgentError(
        "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.FORBIDDEN_DIRECT_ENGINE_AUTHORITY",
        decision.requested_actuator_id);
    return decision;
  }
  if (!request.arbitration_passed || !request.arbitration_evidence_present) {
    decision.status = AgentError(
        "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ARBITRATION_REQUIRED",
        contract.owning_agent + ":" + contract.action_id);
    return decision;
  }
  if (decision.requested_actuator_id != authority->actuator_id) {
    const auto requested_domain =
        ActuatorAuthorityDomainForId(decision.requested_actuator_id);
    decision.status = AgentError(
        requested_domain == AgentActuatorAuthorityDomain::unknown
            ? "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ACTUATOR_UNKNOWN"
            : "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ACTUATOR_MISMATCH",
        decision.requested_actuator_id + "!=" + authority->actuator_id);
    return decision;
  }
  if (!authority->route_registered) {
    decision.status = AgentError(
        "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ROUTE_UNREGISTERED",
        authority->actuator_id);
    return decision;
  }
  if (!request.actuator_route_available) {
    decision.status = AgentError(
        "SB_AGENT_ACTION_CONTRACT.LIVE_ROUTE_UNIMPLEMENTED",
        authority->actuator_id + ":" + authority->action_id);
    return decision;
  }
  decision.status = AgentOk();
  return decision;
}

AgentActionDecision BuildDryRunDecision(const AgentTypeDescriptor& descriptor,
                                        const AgentActionRequest& action) {
  AgentActionDecision decision;
  decision.result_class = AgentActionResultClass::dry_run_only;
  decision.diagnostic_code = "SB_AGENT_ACTION.DRY_RUN_ONLY";
  decision.detail = descriptor.type_id + ":" + action.operation_id;
  decision.evidence_uuid = DurableObjectUuidFromKey("agent_action_dry_run|" + action.action_uuid);
  decision.mutates_state = false;
  return decision;
}

AgentRuntimeStatus ValidateActionSafetyBudget(const AgentPolicy& policy,
                                              const AgentActionRequest& action,
                                              u64 actions_used_in_window) {
  if (action.idempotency_key.empty()) { return AgentError("SB_AGENT_ACTION.IDEMPOTENCY_KEY_REQUIRED", action.action_uuid); }
  if (actions_used_in_window >= policy.action_budget_per_window) { return AgentError("SB_AGENT_ACTION.BUDGET_EXCEEDED", action.action_uuid); }
  if (!action.dry_run && policy.require_dry_run_before_live) { return AgentError("SB_AGENT_ACTION.DRY_RUN_REQUIRED", action.action_uuid); }
  return AgentOk();
}

AgentRuntimeStatus ValidateManualApproval(const AgentPolicy& policy,
                                          const AgentActionRequest& action) {
  if (policy.require_manual_approval && !action.manual_approval_present && !action.dry_run) {
    return AgentError("SB_AGENT_APPROVAL.REQUIRED", action.action_uuid);
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateOperatorOverride(const AgentRuntimeContext& context,
                                            const AgentActionRequest& action) {
  if (!action.operator_override) { return AgentOk(); }
  if (!AgentContextHasRight(context, "OBS_AGENT_OVERRIDE")) { return AgentError("SB_AGENT_OVERRIDE.RIGHT_REQUIRED", action.action_uuid); }
  return AgentOk();
}

AgentRuntimeStatus ValidateAgentSafeMode(const AgentInstanceRecord& instance,
                                         const AgentActionRequest& action) {
  if (instance.quarantined || instance.state == AgentLifecycleState::quarantined) {
    return AgentError("SB_AGENT_SAFE_MODE.QUARANTINED", instance.instance_uuid);
  }
  if (instance.state == AgentLifecycleState::failed) {
    return AgentError("SB_AGENT_SAFE_MODE.FAILED", instance.instance_uuid);
  }
  if ((instance.safe_mode || instance.disabled_by_operator ||
       instance.state == AgentLifecycleState::safe_mode ||
       instance.state == AgentLifecycleState::disabled) && !action.dry_run) {
    return AgentError("SB_AGENT_SAFE_MODE.ACTION_BLOCKED", instance.instance_uuid);
  }
  return AgentOk();
}

AgentActionDecision EvaluateAgentAction(const AgentRuntimeContext& context,
                                        const AgentTypeDescriptor& descriptor,
                                        const AgentPolicy& policy,
                                        const AgentActionRequest& action) {
  const auto policy_status = ValidateAgentPolicy(policy, descriptor);
  if (!policy_status.ok) {
    return {AgentActionResultClass::failed_closed, policy_status.diagnostic_code,
            policy_status.detail, DurableObjectUuidFromKey("agent_action_policy|" + action.action_uuid), false};
  }
  if (policy.activation == AgentActivationProfile::disabled) {
    return {AgentActionResultClass::refused, "SB_AGENT_ACTION.AGENT_DISABLED", descriptor.type_id,
            DurableObjectUuidFromKey("agent_action_refused|" + action.action_uuid), false};
  }
  if (action.dry_run || policy.activation == AgentActivationProfile::dry_run || !policy.allow_live_action) {
    return BuildDryRunDecision(descriptor, action);
  }
  const auto security = ValidateAgentSecurity(context, descriptor, "control");
  if (!security.ok) {
    return {AgentActionResultClass::failed_closed, security.diagnostic_code, security.detail,
            DurableObjectUuidFromKey("agent_action_security|" + action.action_uuid), false};
  }
  const auto approval = ValidateManualApproval(policy, action);
  if (!approval.ok) {
    return {AgentActionResultClass::approval_required, approval.diagnostic_code, approval.detail,
            DurableObjectUuidFromKey("agent_action_approval|" + action.action_uuid), false};
  }
  const auto actuator = ValidateActuatorCapability(context, action);
  if (!actuator.ok) {
    return {AgentActionResultClass::failed_closed, actuator.diagnostic_code, actuator.detail,
            DurableObjectUuidFromKey("agent_action_actuator|" + action.action_uuid), false};
  }
  return {AgentActionResultClass::accepted, "SB_AGENT_ACTION.ACCEPTED", action.operation_id,
          DurableObjectUuidFromKey("agent_action_accepted|" + action.action_uuid), true};
}

// SEARCH_KEY: SB_AGENT_ACTION_CONTRACT_MATRIX_RUNTIME
std::vector<AgentActionContractDescriptor> AgentActionContractRegistry() {
  return {
      Contract("publish_node_capability", "node_resource_agent", "metrics_registry",
               "observe_notify", "async", "OBS_AGENT_CONTROL internal",
               "node_resource_policy", "node capability metrics fresh",
               {"sb_cluster_node_cpu_feature_available", "sb_cluster_node_page_size_support"},
               "agent_action_evidence", "60s/node", "mark suitability unknown"),
      Contract("publish_role_suitability", "node_resource_agent", "metrics_registry",
               "observe_notify", "async", "OBS_AGENT_CONTROL internal",
               "node_resource_policy", "capability and load metrics fresh",
               {"sb_cluster_node_role_suitability_score"},
               "node_role_suitability_evidence", "60s/node", "suitability unknown"),
      Contract("reject_metric_sample", "metrics_registry_manager", "metrics_registry",
               "protect_integrity", "sync", "OBS_AGENT_CONTROL internal",
               "metric_registry_policy", "schema/cardinality violation proof",
               {"sb_metric_samples_rejected_total"},
               "metric_reject_evidence", "none", "reject and continue"),
      Contract("rollup_metrics", "metrics_registry_manager", "metrics_rollup",
               "maintenance", "async", "OBS_AGENT_CONTROL internal",
               "metric_registry_policy", "rollup backlog fresh",
               {"sb_metric_samples_rejected_total"},
               "metric_rollup_evidence", "5m", "pause rollup"),
      Contract("shed_export", "metrics_registry_manager", "export_subsystem",
               "protect_availability", "sync", "OBS_AGENT_CONTROL",
               "metric_registry_policy", "export queue pressure fresh",
               {"sb_metric_samples_rejected_total"},
               "export_shed_evidence", "60s/adapter", "deny export"),
      Contract("request_filespace_quarantine", "storage_health_manager", "storage_manager",
               "protect_durability", "async",
               "OBS_AGENT_ACTION_APPROVE unless critical automatic policy",
               "storage_health_policy", "checksum or unknown-page evidence fresh",
               {"sb_storage_checksum_failures_total", "sb_storage_unknown_pages_total"},
               "filespace_quarantine_evidence", "blocked while unresolved", "operator review"),
      Contract("update_storage_cost", "storage_health_manager", "optimizer_cost_registry",
               "optimize_performance", "async", "OBS_AGENT_CONTROL",
               "storage_health_policy", "storage latency histograms fresh",
               {"sb_storage_fsync_latency_microseconds"},
               "storage_cost_evidence", "5m/filespace", "keep prior cost"),
      Contract("emit_storage_health_summary", "storage_health_manager", "storage_health_evidence",
               "observe_notify", "async", "OBS_AGENT_STATE_READ",
               "storage_health_policy", "filespace/page/storage health metrics fresh",
               {"sb_filespace_health_state", "sb_storage_unknown_pages_total"},
               "storage_health_summary_evidence", "per health transition",
               "mark health unknown and route recommendation to owning agent"),
      Contract("request_filespace_expand", "filespace_capacity_manager", "filespace_lifecycle",
               "storage_capacity", "async",
               "OBS_AGENT_ACTION_APPROVE plus filespace lifecycle right",
               "filespace_capacity_policy.expand_allowed",
               "capacity metrics and device health fresh",
               {"sb_filespace_free_bytes", "sb_filespace_health_state"},
               "filespace_expand_evidence", "policy cooldown", "deny expansion request"),
      Contract("request_filespace_move", "filespace_capacity_manager", "filespace_lifecycle",
               "storage_migration", "async",
               "OBS_AGENT_ACTION_APPROVE plus filespace lifecycle right",
               "filespace_capacity_policy.move_allowed",
               "source/target filespace metrics fresh and startup safe",
               {"sb_filespace_role_state", "sb_filespace_health_state"},
               "filespace_move_evidence", "policy cooldown", "recommend only"),
      Contract("request_filespace_shrink", "filespace_capacity_manager", "page_allocation_manager",
               "storage_capacity", "async", "OBS_AGENT_ACTION_APPROVE",
               "filespace_capacity_policy.shrink_allowed",
               "shrink candidate metrics fresh",
               {"sb_filespace_free_bytes", "sb_page_released_free_count"},
               "filespace_shrink_request_evidence", "policy cooldown", "deny shrink"),
      Contract("request_filespace_truncate", "filespace_capacity_manager", "filespace_lifecycle",
               "destructive_storage", "async",
               "OBS_AGENT_ACTION_APPROVE plus lifecycle truncate right",
               "filespace_capacity_policy.truncate_allowed",
               "publish_shrink_ready evidence fresh and blocker count zero",
               {"sb_filespace_truncate_ready_bytes", "sb_page_relocation_ready_for_filespace_shrink"},
               "filespace_truncate_evidence", "no retry on uncertainty", "deny truncate"),
      Contract("request_filespace_quarantine", "filespace_capacity_manager", "filespace_lifecycle",
               "protect_durability", "async",
               "OBS_AGENT_ACTION_APPROVE unless critical automatic policy",
               "filespace_capacity_policy", "device/checksum/unknown-page evidence",
               {"sb_filespace_device_error_total", "sb_filespace_health_state"},
               "filespace_quarantine_evidence", "blocked while unresolved", "operator review"),
      Contract("recommend_primary_shadow_promotion", "filespace_capacity_manager", "operator_manager",
               "recommendation", "async", "OBS_AGENT_RECOMMENDATION_READ",
               "filespace_shadow_promotion_policy",
               "primary degradation proof and candidate readiness proof fresh",
               {"sb_filespace_health_state", "sb_filespace_role_state"},
               "filespace_shadow_promotion_evidence", "policy cooldown",
               "no promotion recommendation"),
      Contract("preallocate_page_family", "page_allocation_manager", "page_manager",
               "performance_capacity", "async",
               "OBS_AGENT_CONTROL internal unless approval policy requires",
               "page_preallocation_policy.preallocation_allowed",
               "page and filespace free metrics fresh",
               {"sb_page_free_count", "sb_page_reserved_count"},
               "page_preallocation_evidence", "policy throttle", "recommendation only"),
      Contract("relocate_pages", "page_allocation_manager", "page_manager",
               "storage_rewrite", "async", "OBS_AGENT_ACTION_APPROVE",
               "page_relocation_policy.relocation_allowed",
               "page scan fresh; holds clear",
               {"sb_page_allocated_count", "sb_page_relocation_backlog_count"},
               "page_relocation_evidence", "policy throttle", "deny relocation"),
      Contract("defragment_page_family", "page_allocation_manager", "page_manager",
               "storage_rewrite", "async", "OBS_AGENT_ACTION_APPROVE",
               "page_relocation_policy.defragment_allowed",
               "fragmentation metrics fresh",
               {"sb_page_fragmentation_ratio", "sb_page_relocation_backlog_count"},
               "page_defrag_evidence", "policy throttle", "recommendation only"),
      Contract("publish_shrink_ready", "page_allocation_manager", "filespace_capacity_manager",
               "evidence_publish", "async", "internal agent evidence right",
               "page_relocation_policy.publish_shrink_ready_allowed",
               "relocation scan fresh and blockers zero",
               {"sb_page_relocation_ready_for_filespace_shrink", "sb_page_relocation_blocked_total"},
               "page_shrink_ready_evidence", "per scan generation",
               "publish blocker report instead"),
      Contract("request_filespace_capacity", "page_allocation_manager", "filespace_capacity_manager",
               "recommendation", "async", "internal recommendation",
               "page_preallocation_policy", "page demand forecast fresh",
               {"sb_page_free_count", "sb_page_preallocation_deficit_count"},
               "page_capacity_request_evidence", "policy throttle", "preallocation suppressed"),
      Contract("deny_large_grant", "memory_governor", "memory_allocator",
               "protect_availability", "sync", "OBS_AGENT_CONTROL internal",
               "memory_governor_policy", "emergency reserve below threshold",
               {"sb_memory_emergency_reserve_bytes"},
               "memory_grant_evidence", "30s/workload", "conservative grants"),
      Contract("force_spill", "memory_governor", "executor_memory",
               "protect_availability", "sync", "OBS_AGENT_CONTROL internal",
               "memory_governor_policy", "spill target and reserve proof fresh",
               {"sb_memory_emergency_reserve_bytes", "sb_memory_allocated_bytes"},
               "memory_spill_evidence", "30s/query", "deny new grant"),
      Contract("shrink_cache", "memory_governor", "cache_manager",
               "protect_availability", "async", "OBS_AGENT_CONTROL internal",
               "memory_governor_policy", "cache pressure fresh",
               {"sb_memory_allocated_bytes"},
               "cache_shrink_evidence", "60s/cache", "no-op"),
      Contract("recommend_index_rebuild", "index_health_manager", "job_control_manager",
               "optimize_performance", "async", "OBS_AGENT_RECOMMENDATION_READ",
               "index_health_policy", "read amplification or split pressure above threshold",
               {"sb_index_read_amplification_ratio", "sb_index_splits_total"},
               "index_recommendation_evidence", "24h/index", "no action"),
      Contract("recommend_index_drop", "index_health_manager", "job_control_manager",
               "optimize_storage", "async", "OBS_AGENT_RECOMMENDATION_READ",
               "index_health_policy", "unused index window satisfied",
               {"sb_index_lookup_latency_microseconds"},
               "index_recommendation_evidence", "7d/index", "no action"),
      Contract("request_fast_filespace_for_index_rebuild", "index_health_manager",
               "filespace_capacity_manager", "optimize_performance", "async",
               "OBS_AGENT_RECOMMENDATION_READ",
               "index_health_policy and filespace_capacity_policy",
               "index size estimate and filespace latency metrics fresh",
               {"sb_index_read_amplification_ratio", "sb_filespace_fsync_latency_microseconds"},
               "index_storage_request_evidence", "24h/index", "no storage recommendation"),
      Contract("throttle_admission", "admission_control_manager", "admission_gate",
               "protect_availability", "sync", "OBS_AGENT_CONTROL",
               "admission_control_policy", "pressure metrics fresh",
               {"sb_memory_emergency_reserve_bytes", "sb_listener_queue_depth"},
               "admission_throttle_evidence", "5s", "conservative throttle"),
      Contract("deny_admission", "admission_control_manager", "admission_gate",
               "protect_availability", "sync", "OBS_AGENT_CONTROL",
               "admission_control_policy", "hard-limit proof fresh",
               {"sb_memory_emergency_reserve_bytes", "sb_scheduler_queue_depth"},
               "admission_deny_evidence", "5s", "deny rather than overload"),
      Contract("downgrade_admission", "admission_control_manager", "admission_gate",
               "protect_availability", "sync", "OBS_AGENT_CONTROL",
               "admission_control_policy", "degraded-mode policy and pressure proof fresh",
               {"sb_memory_emergency_reserve_bytes", "sb_workload_slo_burn_rate"},
               "admission_downgrade_evidence", "5s", "throttle instead"),
      Contract("drain_parser_family", "parser_interface_manager", "parser_supervisor",
               "protect_availability", "async", "OBS_AGENT_CONTROL",
               "parser_health_policy", "parser crash threshold fresh",
               {"sb_parser_crashes_total", "sb_parser_sessions_active"},
               "parser_drain_evidence", "10m", "quarantine on repeat"),
      Contract("quarantine_parser_package", "parser_interface_manager", "parser_supervisor",
               "protect_security", "async",
               "OBS_AGENT_CONTROL and SEC_REDACTION_POLICY_EDIT for security cases",
               "parser_health_policy", "crash/security evidence fresh",
               {"sb_parser_crashes_total"},
               "parser_quarantine_evidence", "none while quarantined", "deny parser assignment"),
      Contract("warn_long_tx", "transaction_pressure_manager", "session_control_manager",
               "protect_cleanup", "async", "OBS_AGENT_RECOMMENDATION_READ",
               "long_transaction_policy", "transaction age/blocker metrics fresh",
               {"sb_tx_active_transactions", "sb_tx_oldest_snapshot_age_microseconds"},
               "transaction_pressure_evidence", "15m/session", "recommendation only"),
      Contract("request_session_reauth", "transaction_pressure_manager", "session_control_manager",
               "security_control", "async", "OBS_AGENT_ACTION_APPROVE",
               "long_transaction_policy", "idle/long session proof fresh",
               {"sb_tx_oldest_snapshot_age_microseconds", "sb_identity_sessions_idle"},
               "session_control_evidence", "policy cooldown", "deny action"),
      Contract("recommend_cancel_tx", "transaction_pressure_manager", "transaction_manager",
               "protect_cleanup", "async", "OBS_AGENT_ACTION_APPROVE",
               "long_transaction_policy", "blocker proof fresh",
               {"sb_tx_active_transactions", "sb_tx_oldest_snapshot_age_microseconds"},
               "transaction_pressure_evidence", "policy cooldown", "no rollback of committed work"),
      Contract("cleanup_storage_versions", "storage_version_cleanup_agent", "storage_manager",
               "protect_storage", "sync", "OBS_AGENT_CONTROL",
               "storage_version_cleanup_policy", "authoritative cleanup horizon and bounded batch budget fresh",
               {"sb_mga_cleanup_horizon_local_transaction_id", "sb_mga_cleanup_retained_row_versions"},
               "storage_version_cleanup_evidence", "bounded batch", "deny cleanup"),
      Contract("advance_cleanup_lwm", "cleanup_archive_manager", "cleanup_manager",
               "protect_storage", "async", "OBS_AGENT_CONTROL",
               "cleanup_archive_policy", "authoritative low-water mark fresh",
               {"sb_mga_cleanup_blocked_total"},
               "cleanup_lwm_evidence", "no retry on uncertainty", "deny cleanup"),
      Contract("request_archive_verify", "cleanup_archive_manager", "archive_manager",
               "protect_recovery", "async", "OBS_AGENT_CONTROL",
               "cleanup_archive_policy", "archive lag or slice age proof fresh",
               {"sb_archive_lag_bytes", "sb_archive_slice_age_microseconds"},
               "archive_verify_evidence", "1h/slice", "hold slice"),
      Contract("create_policy_recommendation", "policy_recommendation_manager", "policy_registry",
               "recommendation", "async", "OBS_AGENT_RECOMMENDATION_READ",
               "policy_recommendation_policy", "source metrics fresh",
               {"sb_policy_evaluations_total", "sb_workload_slo_burn_rate"},
               "policy_recommendation_evidence", "24h/policy", "no recommendation"),
      Contract("recommend_planner_correction", "runtime_learning_agent",
               "optimizer_feedback_registry", "optimize_performance", "async",
               "OBS_AGENT_RECOMMENDATION_READ", "optimizer_learning_policy",
               "estimate error metrics fresh",
               {"sb_optimizer_plan_estimate_error_ratio"},
               "planner_learning_evidence", "1h/query-shape", "disable correction"),
      Contract("prepare_redacted_bundle", "support_bundle_triage_agent",
               "support_bundle_subsystem", "support", "async",
               "OBS_SUPPORT_BUNDLE_READ and redaction rights", "support_bundle_policy",
               "evidence availability and redaction proof fresh",
               {"sb_support_bundle_completeness_ratio", "sb_agent_actions_total"},
               "support_bundle_evidence", "30m/scope", "deny bundle"),
      Contract("recommend_support_bundle", "support_bundle_triage_agent",
               "support_bundle_subsystem", "support", "async", "OBS_SUPPORT_BUNDLE_READ",
               "support_bundle_policy", "incident/evidence completeness threshold met",
               {"sb_support_bundle_completeness_ratio"},
               "support_bundle_evidence", "30m/scope", "no recommendation"),
      Contract("cancel_job", "job_control_manager", "job_manager",
               "operator_control", "async",
               "JOB EXECUTE or job admin plus OBS_AGENT_ACTION_APPROVE",
               "job_control_policy", "job state cancellable",
               {"sb_job_control_actions_total", "sb_jobs_running"},
               "job_control_evidence", "no repeated cancel spam", "deny"),
      Contract("retry_job", "job_control_manager", "job_manager",
               "operator_control", "async",
               "JOB EXECUTE or job admin plus OBS_AGENT_ACTION_APPROVE",
               "job_control_policy", "retry policy and failure state valid",
               {"sb_job_control_actions_total", "sb_job_run_progress_percent"},
               "job_control_evidence", "policy cooldown", "deny"),
      Contract("suppress_job", "job_control_manager", "job_manager",
               "operator_control", "async",
               "JOB EXECUTE or job admin plus OBS_AGENT_OVERRIDE",
               "job_control_policy", "job suppression scope and expiry valid",
               {"sb_job_control_actions_total"},
               "job_suppression_evidence", "policy cooldown", "deny suppression"),
      Contract("start_backup", "backup_manager", "backup_subsystem",
               "protect_recovery", "async",
               "backup control right plus OBS_AGENT_ACTION_APPROVE",
               "backup_policy", "blockers clear",
               {"sb_backup_progress_percent", "sb_backup_blocker_state"},
               "backup_action_evidence", "policy cooldown", "deny start"),
      Contract("cancel_backup", "backup_manager", "backup_subsystem",
               "operator_control", "async",
               "backup control right plus OBS_AGENT_ACTION_CANCEL",
               "backup_policy", "backup cancellable",
               {"sb_backup_progress_percent"},
               "backup_action_evidence", "none", "deny cancel"),
      Contract("verify_backup", "backup_manager", "backup_subsystem",
               "protect_recovery", "async", "OBS_AGENT_CONTROL",
               "backup_policy", "backup manifest available",
               {"sb_backup_progress_percent", "sb_backup_verification_failures_total"},
               "backup_verify_evidence", "1h/backup", "mark unknown"),
      Contract("seal_archive_slice", "archive_manager", "archive_subsystem",
               "protect_recovery", "async", "OBS_AGENT_CONTROL",
               "archive_policy", "slice complete and not protected-incomplete",
               {"sb_archive_slice_age_microseconds", "sb_archive_queue_depth"},
               "archive_slice_evidence", "per slice", "hold slice"),
      Contract("request_verify_slice", "archive_manager", "archive_subsystem",
               "protect_recovery", "async", "OBS_AGENT_CONTROL",
               "archive_policy", "slice available",
               {"sb_archive_slice_age_microseconds"},
               "archive_verify_evidence", "1h/slice", "hold slice"),
      Contract("run_restore_drill", "restore_drill_manager", "restore_subsystem",
               "support", "async",
               "restore drill control right plus OBS_AGENT_ACTION_APPROVE",
               "restore_drill_policy", "target isolated and resources available",
               {"sb_restore_drill_operations_total", "sb_restore_drill_duration_microseconds"},
               "restore_drill_evidence", "policy cooldown", "deny drill"),
      Contract("estimate_pitr", "pitr_manager", "pitr_subsystem",
               "observe_notify", "async", "OBS_AGENT_STATE_READ",
               "pitr_policy", "archive window metrics fresh",
               {"sb_pitr_window_available_seconds", "sb_pitr_target_reachable"},
               "pitr_estimate_evidence", "15m/target", "report unreachable"),
      Contract("request_restore_plan", "pitr_manager", "restore_subsystem",
               "support", "async",
               "restore planning right plus OBS_AGENT_ACTION_APPROVE",
               "pitr_policy", "target reachable proof",
               {"sb_pitr_target_reachable"},
               "pitr_restore_plan_evidence", "policy cooldown", "deny plan"),
      Contract("lock_user", "identity_manager", "security_manager",
               "protect_security", "async", "SEC_IDENTITY_ADMIN",
               "identity_lifecycle_policy", "auth anomaly or explicit admin request",
               {"sb_identity_auth_attempts_total", "sb_identity_account_lifecycle_total"},
               "identity_lifecycle_evidence", "cooldown by principal", "deny"),
      Contract("require_reauth", "identity_manager", "security_manager",
               "protect_security", "async", "SEC_IDENTITY_ADMIN or session control right",
               "identity_lifecycle_policy", "session/auth proof fresh",
               {"sb_identity_sessions_active", "sb_identity_auth_attempts_total"},
               "identity_lifecycle_evidence", "cooldown by principal", "deny"),
      Contract("emit_identity_evidence", "identity_manager", "security_audit",
               "protect_security", "async",
               "SEC_AUTH_METRICS_READ or SEC_IDENTITY_ADMIN",
               "identity_lifecycle_policy",
               "identity event visible and redaction policy valid",
               {"sb_identity_account_lifecycle_total"},
               "identity_lifecycle_evidence", "per event", "redact and emit minimal evidence"),
      Contract("force_disconnect", "session_control_manager", "session_manager",
               "operator_control", "async",
               "session kill right plus OBS_AGENT_ACTION_APPROVE",
               "session_control_policy", "target session visible and disconnect allowed",
               {"sb_identity_sessions_active"},
               "session_control_evidence", "no repeated disconnect spam", "deny"),
      Contract("require_reauth", "session_control_manager", "security_manager",
               "protect_security", "async", "SEC_IDENTITY_ADMIN or session control right",
               "session_control_policy", "target session visible",
               {"sb_identity_sessions_active", "sb_identity_sessions_idle"},
               "session_control_evidence", "cooldown by principal", "deny"),
      Contract("revoke_session", "session_control_manager", "security_manager",
               "protect_security", "async", "SEC_IDENTITY_ADMIN or session control right",
               "session_control_policy", "target token/session visible",
               {"sb_identity_sessions_active"},
               "session_control_evidence", "cooldown by principal", "deny"),
      Contract("fire_alert", "alert_manager", "alert_delivery",
               "observe_notify", "async", "internal alert authority",
               "alerting_baseline", "health transition evidence",
               {"sb_alerts_fired_total"},
               "alert_fire_evidence", "dedupe window", "local event only"),
      Contract("silence_alert", "alert_manager", "alert_delivery",
               "observe_notify", "async", "OBS_AGENT_OVERRIDE",
               "alerting_baseline", "alert visible and silence valid",
               {"sb_alerts_fired_total"},
               "alert_silence_evidence", "max silence policy", "deny silence"),
      Contract("clear_alert", "alert_manager", "alert_delivery",
               "observe_notify", "async",
               "internal alert authority or OBS_AGENT_OVERRIDE",
               "alerting_baseline", "alert clear condition or operator silence expiry",
               {"sb_alerts_fired_total"},
               "alert_clear_evidence", "dedupe window", "local event only"),
      Contract("enable_export", "export_adapter_manager", "export_subsystem",
               "integration", "async", "export control right",
               "export_default_baseline", "adapter config valid and redaction/residency pass",
               {"sb_export_adapter_queue_depth"},
               "export_adapter_evidence", "policy cooldown", "deny export"),
      Contract("disable_export", "export_adapter_manager", "export_subsystem",
               "integration", "async", "export control right",
               "export_default_baseline", "adapter visible",
               {"sb_export_adapter_queue_depth"},
               "export_adapter_evidence", "none", "deny export"),
      Contract("shed_export", "export_adapter_manager", "export_subsystem",
               "protect_availability", "sync", "OBS_AGENT_CONTROL",
               "export_default_baseline", "export queue pressure fresh",
               {"sb_export_adapter_queue_depth"},
               "export_shed_evidence", "60s/adapter", "deny export")};
}

std::optional<AgentActionContractDescriptor> FindAgentActionContract(
    const std::string& owning_agent,
    const std::string& action_id) {
  for (const auto& descriptor : AgentActionContractRegistry()) {
    if (descriptor.owning_agent == owning_agent && descriptor.action_id == action_id) {
      return descriptor;
    }
  }
  return std::nullopt;
}

std::vector<std::string> CanonicalAgentAllowedActionIds(const std::string& agent_type_id) {
  if (agent_type_id == "node_resource_agent") { return {"publish_node_capability", "publish_role_suitability"}; }
  if (agent_type_id == "metrics_registry_manager") { return {"reject_metric_sample", "rollup_metrics", "shed_export"}; }
  if (agent_type_id == "storage_health_manager") { return {"request_filespace_quarantine", "update_storage_cost", "emit_storage_health_summary"}; }
  if (agent_type_id == "filespace_capacity_manager") { return {"request_filespace_expand", "request_filespace_move", "request_filespace_shrink", "request_filespace_truncate", "request_filespace_quarantine", "recommend_primary_shadow_promotion"}; }
  if (agent_type_id == "page_allocation_manager") { return {"preallocate_page_family", "relocate_pages", "defragment_page_family", "publish_shrink_ready", "request_filespace_capacity"}; }
  if (agent_type_id == "memory_governor") { return {"deny_large_grant", "force_spill", "shrink_cache"}; }
  if (agent_type_id == "index_health_manager") { return {"recommend_index_rebuild", "recommend_index_drop", "request_fast_filespace_for_index_rebuild"}; }
  if (agent_type_id == "cluster_autoscale_manager") { return {"recommend_scale_up", "recommend_scale_down"}; }
  if (agent_type_id == "admission_control_manager") { return {"throttle_admission", "deny_admission", "downgrade_admission"}; }
  if (agent_type_id == "parser_interface_manager") { return {"drain_parser_family", "quarantine_parser_package"}; }
  if (agent_type_id == "transaction_pressure_manager") { return {"warn_long_tx", "request_session_reauth", "recommend_cancel_tx"}; }
  if (agent_type_id == "storage_version_cleanup_agent") { return {"cleanup_storage_versions"}; }
  if (agent_type_id == "cleanup_archive_manager") { return {"advance_cleanup_lwm", "request_archive_verify"}; }
  if (agent_type_id == "policy_recommendation_manager") { return {"create_policy_recommendation"}; }
  if (agent_type_id == "distributed_query_metrics_agent") { return {"publish_fragment_metrics"}; }
  if (agent_type_id == "remote_query_routing_agent") { return {"recommend_route_weight"}; }
  if (agent_type_id == "runtime_learning_agent") { return {"recommend_planner_correction"}; }
  if (agent_type_id == "support_bundle_triage_agent") { return {"recommend_support_bundle", "prepare_redacted_bundle"}; }
  if (agent_type_id == "cluster_scheduler_manager") { return {"recommend_job_placement", "route_cluster_job"}; }
  if (agent_type_id == "job_control_manager") { return {"cancel_job", "retry_job", "suppress_job"}; }
  if (agent_type_id == "backup_manager") { return {"start_backup", "cancel_backup", "verify_backup"}; }
  if (agent_type_id == "archive_manager") { return {"seal_archive_slice", "request_verify_slice"}; }
  if (agent_type_id == "restore_drill_manager") { return {"run_restore_drill"}; }
  if (agent_type_id == "pitr_manager") { return {"estimate_pitr", "request_restore_plan"}; }
  if (agent_type_id == "identity_manager") { return {"lock_user", "require_reauth", "emit_identity_evidence"}; }
  if (agent_type_id == "session_control_manager") { return {"force_disconnect", "require_reauth", "revoke_session"}; }
  if (agent_type_id == "alert_manager") { return {"fire_alert", "silence_alert", "clear_alert"}; }
  if (agent_type_id == "export_adapter_manager") { return {"enable_export", "disable_export", "shed_export"}; }
  if (agent_type_id == "cluster_upgrade_manager") { return {"approve_upgrade_step", "block_upgrade_step"}; }
  return {};
}

namespace {

std::string ContractEvidenceUuid(const AgentActionContractDescriptor& contract,
                                 const std::string& diagnostic_code) {
  return DurableObjectUuidFromKey("agent_action_contract|" + contract.owning_agent + "|" +
                                  contract.action_id + "|" + diagnostic_code);
}

AgentActionDecision ContractDecision(const AgentActionContractDescriptor& contract,
                                     AgentActionResultClass result_class,
                                     std::string diagnostic_code,
                                     std::string detail,
                                     bool mutates_state = false) {
  const std::string evidence_uuid = ContractEvidenceUuid(contract, diagnostic_code);
  return {result_class, std::move(diagnostic_code), std::move(detail),
          evidence_uuid, mutates_state};
}

std::string PolicyFamilyForGate(const std::string& policy_gate) {
  const auto and_pos = policy_gate.find(" and ");
  std::string first = and_pos == std::string::npos
      ? policy_gate
      : policy_gate.substr(0, and_pos);
  const auto dot_pos = first.find('.');
  if (dot_pos != std::string::npos) { first = first.substr(0, dot_pos); }
  return first;
}

std::string PolicyFieldForGate(const std::string& policy_gate) {
  const auto and_pos = policy_gate.find(" and ");
  const std::string first = and_pos == std::string::npos
      ? policy_gate
      : policy_gate.substr(0, and_pos);
  const auto dot_pos = first.find('.');
  if (dot_pos == std::string::npos || dot_pos + 1 >= first.size()) { return {}; }
  return first.substr(dot_pos + 1);
}

bool PolicyMatchesContractGate(const AgentPolicy& policy,
                               const AgentActionContractDescriptor& contract) {
  if (contract.policy_gate.find(policy.policy_family) != std::string::npos) {
    return true;
  }
  return PolicyFamilyForGate(contract.policy_gate) == policy.policy_family;
}

}  // namespace

AgentActionDecision EvaluateAgentActionContract(
    const AgentActionContractDescriptor& contract,
    const AgentActionContractEvaluationRequest& request) {
  if (contract.action_id.empty() || contract.owning_agent.empty() ||
      contract.actuator.empty()) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            "SB_AGENT_ACTION_CONTRACT.DESCRIPTOR_INCOMPLETE",
                            contract.owning_agent + ":" + contract.action_id);
  }
  if (contract.policy_gate.empty() || !request.policy_gate_present) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            "SB_AGENT_ACTION_CONTRACT.POLICY_GATE_REQUIRED",
                            contract.owning_agent + ":" + contract.action_id);
  }
  if (contract.evidence_kind.empty() || !request.evidence_store_available) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            "SB_AGENT_ACTION_CONTRACT.EVIDENCE_REQUIRED",
                            contract.owning_agent + ":" + contract.action_id);
  }
  if (contract.metric_precondition_text.empty() || contract.metric_families.empty()) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            "SB_AGENT_ACTION_CONTRACT.METRIC_PRECONDITION_REQUIRED",
                            contract.owning_agent + ":" + contract.action_id);
  }
  const auto owner = FindAgentType(contract.owning_agent);
  if (!owner.has_value()) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            "SB_AGENT_ACTION_CONTRACT.OWNER_UNKNOWN",
                            contract.owning_agent);
  }
  if (owner->cluster_only || owner->deployment == AgentDeployment::cluster ||
      contract.cluster_scoped) {
    return ContractDecision(contract, AgentActionResultClass::refused,
                            "SB_AGENT_ACTION_CONTRACT.CLUSTER_ONLY_EXCLUDED",
                            contract.owning_agent + ":" + contract.action_id);
  }
  if (!request.policy_present || request.policy == nullptr) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            "SB_AGENT_ACTION_CONTRACT.POLICY_REQUIRED",
                            contract.owning_agent + ":" + contract.action_id);
  }
  const AgentPolicy& policy = *request.policy;
  const auto policy_status = ValidateAgentPolicy(policy, *owner);
  if (!policy_status.ok) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            policy_status.diagnostic_code, policy_status.detail);
  }
  if (!PolicyMatchesContractGate(policy, contract)) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            "SB_AGENT_ACTION_CONTRACT.POLICY_GATE_MISMATCH",
                            contract.policy_gate + ":" + policy.policy_family);
  }
  if (!policy.enabled || policy.activation == AgentActivationProfile::disabled ||
      policy.action_mode == "disabled") {
    return ContractDecision(contract, AgentActionResultClass::refused,
                            "SB_AGENT_ACTION_CONTRACT.POLICY_DISABLED",
                            contract.owning_agent + ":" + contract.action_id);
  }
  const std::string policy_field = PolicyFieldForGate(contract.policy_gate);
  if (!policy_field.empty()) {
    const auto found = policy.config_fields.find(policy_field);
    if (found == policy.config_fields.end() || found->second.empty()) {
      return ContractDecision(contract, AgentActionResultClass::failed_closed,
                              "SB_AGENT_ACTION_CONTRACT.POLICY_GATE_REQUIRED",
                              contract.policy_gate);
    }
    if (IsFalseValue(found->second)) {
      return ContractDecision(contract, AgentActionResultClass::refused,
                              "SB_AGENT_ACTION_CONTRACT.POLICY_DEFAULT_DENY",
                              contract.policy_gate);
    }
  }
  const auto grant = EvaluateAgentActionContractGrant(request.context, contract);
  if (!grant.allowed) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            grant.diagnostic_code, grant.missing_right.empty()
                                ? grant.detail
                                : grant.missing_right);
  }
  if (request.resource_budget.has_value()) {
    const auto budget_decision = EvaluateAgentResourceBudget(
        *owner, policy, request.context, *request.resource_budget);
    if (budget_decision.decision != AgentResourceBudgetDecisionKind::allow) {
      AgentActionResultClass result_class = AgentActionResultClass::suppressed;
      if (budget_decision.decision == AgentResourceBudgetDecisionKind::fail_closed) {
        result_class = AgentActionResultClass::failed_closed;
      } else if (budget_decision.decision == AgentResourceBudgetDecisionKind::shed_refuse ||
                 budget_decision.decision ==
                     AgentResourceBudgetDecisionKind::foreground_protection) {
        result_class = AgentActionResultClass::refused;
      }
      return {result_class,
              budget_decision.status.diagnostic_code,
              budget_decision.status.detail,
              budget_decision.evidence_uuid,
              false};
    }
  }
  if (request.enforce_metric_observation_dependencies ||
      request.enforce_policy_dependency_state) {
    AgentTypeDescriptor contract_dependencies = *owner;
    contract_dependencies.metric_dependencies.clear();
    for (const auto& family : contract.metric_families) {
      auto contract_dep = FindAgentMetricDependencyContract(contract.owning_agent, family);
      AgentMetricDependency dep;
      if (contract_dep.has_value()) {
        dep = *contract_dep;
        dep.required = true;
      } else {
        dep.metric_family = family;
        dep.required = true;
        dep.max_freshness_microseconds = 300000000;
        const auto* metric =
            scratchbird::core::metrics::DefaultMetricRegistry().FindDescriptorOrAlias(family);
        if (metric != nullptr) {
          dep.namespace_prefix = metric->namespace_path;
          dep.cluster_only = metric->cluster_only;
        }
      }
      contract_dependencies.metric_dependencies.push_back(std::move(dep));
    }
    const auto dependency_evaluation = EvaluateAgentMetricDependencies(
        contract_dependencies,
        request.context,
        request.metric_observations,
        request.policy_dependency_states,
        request.enforce_policy_dependency_state);
    if (!dependency_evaluation.status.ok) {
      if (!dependency_evaluation.diagnostics.empty()) {
        return {AgentActionResultClass::failed_closed,
                dependency_evaluation.status.diagnostic_code,
                dependency_evaluation.status.detail,
                dependency_evaluation.diagnostics.front().evidence_uuid,
                false};
      }
      return ContractDecision(contract, AgentActionResultClass::failed_closed,
                              dependency_evaluation.status.diagnostic_code,
                              dependency_evaluation.status.detail);
    }
  }
  if (!request.enforce_metric_observation_dependencies) {
    for (const auto& family : contract.metric_families) {
      if (!Contains(request.available_metric_families, family)) {
        return ContractDecision(contract, AgentActionResultClass::failed_closed,
                                "SB_AGENT_ACTION_CONTRACT.METRIC_REQUIRED",
                                family);
      }
    }
  }
  if (request.suppressed) {
    return ContractDecision(contract, AgentActionResultClass::suppressed,
                            "SB_AGENT_ACTION_CONTRACT.SUPPRESSED",
                            contract.owning_agent + ":" + contract.action_id);
  }
  if (!request.arbitration_passed || !request.arbitration_evidence_present) {
    return ContractDecision(
        contract, AgentActionResultClass::failed_closed,
        "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ARBITRATION_REQUIRED",
        contract.owning_agent + ":" + contract.action_id);
  }
  if (contract.action_class == AgentActionClass::none) {
    return ContractDecision(contract, AgentActionResultClass::accepted,
                            "SB_AGENT_ACTION_CONTRACT.OBSERVE_ACCEPTED",
                            contract.evidence_kind);
  }
  if (contract.action_class == AgentActionClass::recommendation) {
    return ContractDecision(contract, AgentActionResultClass::accepted,
                            "SB_AGENT_ACTION_CONTRACT.RECOMMENDATION_ACCEPTED",
                            contract.evidence_kind);
  }
  if (contract.manual_approval_required || contract.operator_approval_required) {
    return ContractDecision(contract, AgentActionResultClass::approval_required,
                            "SB_AGENT_ACTION_CONTRACT.APPROVAL_REQUIRED",
                            contract.permission);
  }
  if (!request.live_prerequisites_enabled) {
    return ContractDecision(contract, AgentActionResultClass::dry_run_only,
                            "SB_AGENT_ACTION_CONTRACT.DRY_RUN_ONLY",
                            contract.failure_behavior);
  }
  const auto authority = EvaluateAgentActionActuatorAuthority(contract, request);
  if (!authority.status.ok) {
    return ContractDecision(contract, AgentActionResultClass::failed_closed,
                            authority.status.diagnostic_code,
                            authority.status.detail);
  }
  return ContractDecision(contract, AgentActionResultClass::accepted,
                          "SB_AGENT_ACTION_CONTRACT.LIVE_DISPATCH_READY",
                          contract.actuator + ":" + contract.action_id);
}

AgentActionDecision EvaluateAgentActionContract(
    const std::string& owning_agent,
    const std::string& action_id,
    const AgentActionContractEvaluationRequest& request) {
  const auto contract = FindAgentActionContract(owning_agent, action_id);
  if (contract.has_value()) {
    return EvaluateAgentActionContract(*contract, request);
  }
  const auto owner = FindAgentType(owning_agent);
  AgentActionContractDescriptor synthetic;
  synthetic.owning_agent = owning_agent;
  synthetic.action_id = action_id;
  synthetic.actuator = "none";
  synthetic.policy_gate = "none";
  synthetic.metric_precondition_text = "none";
  synthetic.metric_families = {"none"};
  synthetic.evidence_kind = "agent_action_contract_refusal";
  if (owner.has_value() &&
      (owner->cluster_only || owner->deployment == AgentDeployment::cluster)) {
    return ContractDecision(synthetic, AgentActionResultClass::refused,
                            "SB_AGENT_ACTION_CONTRACT.CLUSTER_ONLY_EXCLUDED",
                            owning_agent + ":" + action_id);
  }
  return ContractDecision(synthetic, AgentActionResultClass::failed_closed,
                          "SB_AGENT_ACTION_CONTRACT.MISSING",
                          owning_agent + ":" + action_id);
}

// SEARCH_KEY: SB_AGENT_RESOURCE_BUDGET_BACKPRESSURE
AgentResourceBudget DefaultAgentResourceBudgetForPolicy(const AgentPolicy& policy) {
  auto configured_u64 = [&policy](const std::string& key, u64 fallback) {
    const auto found = policy.config_fields.find(key);
    if (found == policy.config_fields.end() || found->second.empty()) {
      return fallback;
    }
    try {
      return static_cast<u64>(std::stoull(found->second));
    } catch (...) {
      return fallback;
    }
  };
  auto configured_bool = [&policy](const std::string& key, bool fallback) {
    const auto found = policy.config_fields.find(key);
    if (found == policy.config_fields.end() || found->second.empty()) {
      return fallback;
    }
    if (found->second == "1" || found->second == "true" ||
        found->second == "yes" || found->second == "enabled") {
      return true;
    }
    if (found->second == "0" || found->second == "false" ||
        found->second == "no" || found->second == "disabled") {
      return false;
    }
    return fallback;
  };

  AgentResourceBudget budget;
  budget.protect_foreground_work =
      configured_bool("protect_foreground_work", true);
  budget.max_cpu_time_microseconds =
      configured_u64("max_cpu_time_microseconds", policy.max_runtime_microseconds);
  budget.max_memory_bytes =
      configured_u64("max_memory_bytes", 64ull * 1024ull * 1024ull);
  budget.max_io_bytes =
      configured_u64("max_io_bytes", 16ull * 1024ull * 1024ull);
  budget.max_io_ops = configured_u64("max_io_ops", 4096);
  budget.max_thread_slots = configured_u64("max_thread_slots", 1);
  budget.max_queue_depth = configured_u64("max_queue_depth",
      policy.action_budget_per_window == 0
      ? 1
      : policy.action_budget_per_window);
  budget.min_run_interval_microseconds =
      configured_u64("min_run_interval_microseconds", policy.run_interval_microseconds);
  budget.retry_backoff_microseconds =
      configured_u64("retry_backoff_microseconds", policy.cooldown_microseconds);
  budget.watchdog_timeout_microseconds =
      configured_u64("watchdog_timeout_microseconds", policy.max_runtime_microseconds);
  budget.max_history_query_rows =
      configured_u64("max_history_query_rows", policy.max_history_query_rows);
  budget.max_evidence_fanout =
      configured_u64("max_evidence_fanout", policy.max_evidence_fanout);
  budget.max_label_cardinality =
      configured_u64("max_label_cardinality", policy.max_label_cardinality);
  return budget;
}

namespace {

std::string ResourceBudgetEvidenceUuid(const AgentTypeDescriptor& descriptor,
                                       const AgentPolicy& policy,
                                       const std::string& code) {
  return DurableObjectUuidFromKey("agent_resource_budget|" + descriptor.type_id + "|" +
                                  policy.policy_uuid + "|" + code);
}

AgentResourceBudgetDecision BuildResourceBudgetDecision(
    const AgentTypeDescriptor& descriptor,
    const AgentPolicy& policy,
    AgentResourceBudgetDecisionKind kind,
    AgentResourceBudgetDimension dimension,
    std::string code,
    std::string detail,
    bool health_publish_allowed = true) {
  AgentResourceBudgetDecision decision;
  decision.decision = kind;
  decision.evidence_uuid = ResourceBudgetEvidenceUuid(descriptor, policy, code);
  decision.status = kind == AgentResourceBudgetDecisionKind::allow
      ? AgentRuntimeStatus{true, code, detail}
      : AgentError(code, detail);
  decision.action_allowed = kind == AgentResourceBudgetDecisionKind::allow;
  decision.mutation_allowed = kind == AgentResourceBudgetDecisionKind::allow;
  decision.health_publish_allowed = health_publish_allowed;
  decision.failed_closed = kind == AgentResourceBudgetDecisionKind::fail_closed;

  AgentResourceBudgetDiagnostic diagnostic;
  diagnostic.decision = kind;
  diagnostic.dimension = dimension;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  diagnostic.evidence_uuid = decision.evidence_uuid;
  diagnostic.suppresses_mutation = !decision.mutation_allowed;
  diagnostic.protects_foreground =
      kind == AgentResourceBudgetDecisionKind::foreground_protection ||
      dimension == AgentResourceBudgetDimension::foreground_protection;
  decision.diagnostics.push_back(std::move(diagnostic));
  return decision;
}

bool OverLimit(u64 limit, u64 value) {
  return limit != 0 && value > limit;
}

}  // namespace

AgentResourceBudgetDecision EvaluateAgentResourceBudget(
    const AgentTypeDescriptor& descriptor,
    const AgentPolicy& policy,
    const AgentRuntimeContext& context,
    const AgentResourceBudgetEvaluationInput& input) {
  const auto policy_status = ValidateAgentPolicy(policy, descriptor);
  if (!policy_status.ok) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::fail_closed,
        AgentResourceBudgetDimension::foreground_protection,
        policy_status.diagnostic_code, policy_status.detail, false);
  }
  if (input.cancellation_requested || input.drain_requested ||
      context.shutdown_requested) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::cancel_drain,
        AgentResourceBudgetDimension::cancellation_drain,
        "SB_AGENT_RESOURCE_BUDGET.CANCEL_DRAIN_REQUESTED",
        descriptor.type_id);
  }
  if (input.budget.protect_foreground_work &&
      input.foreground_database_work_active &&
      descriptor.authority != AgentAuthorityClass::observe_only) {
    return BuildResourceBudgetDecision(
        descriptor, policy,
        AgentResourceBudgetDecisionKind::foreground_protection,
        AgentResourceBudgetDimension::foreground_protection,
        "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION",
        descriptor.type_id);
  }
  if (OverLimit(input.budget.watchdog_timeout_microseconds,
                input.usage.runtime_microseconds)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::fail_closed,
        AgentResourceBudgetDimension::runtime_timeout,
        "SB_AGENT_RESOURCE_BUDGET.RUNTIME_TIMEOUT_EXCEEDED",
        std::to_string(input.usage.runtime_microseconds));
  }
  if (OverLimit(input.budget.max_cpu_time_microseconds,
                input.usage.cpu_time_microseconds)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::throttle_defer,
        AgentResourceBudgetDimension::cpu_time,
        "SB_AGENT_RESOURCE_BUDGET.CPU_TIME_EXCEEDED",
        std::to_string(input.usage.cpu_time_microseconds));
  }
  if (OverLimit(input.budget.max_memory_bytes, input.usage.memory_bytes)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::shed_refuse,
        AgentResourceBudgetDimension::memory_bytes,
        "SB_AGENT_RESOURCE_BUDGET.MEMORY_BYTES_EXCEEDED",
        std::to_string(input.usage.memory_bytes));
  }
  if (OverLimit(input.budget.max_io_bytes, input.usage.io_bytes)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::throttle_defer,
        AgentResourceBudgetDimension::io_bytes,
        "SB_AGENT_RESOURCE_BUDGET.IO_BYTES_EXCEEDED",
        std::to_string(input.usage.io_bytes));
  }
  if (OverLimit(input.budget.max_io_ops, input.usage.io_ops)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::throttle_defer,
        AgentResourceBudgetDimension::io_ops,
        "SB_AGENT_RESOURCE_BUDGET.IO_OPS_EXCEEDED",
        std::to_string(input.usage.io_ops));
  }
  if (OverLimit(input.budget.max_thread_slots, input.usage.thread_slots)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::shed_refuse,
        AgentResourceBudgetDimension::thread_slots,
        "SB_AGENT_RESOURCE_BUDGET.THREAD_SLOTS_EXHAUSTED",
        std::to_string(input.usage.thread_slots));
  }
  if (OverLimit(input.budget.max_queue_depth, input.usage.queue_depth)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::shed_refuse,
        AgentResourceBudgetDimension::queue_depth,
        "SB_AGENT_RESOURCE_BUDGET.QUEUE_DEPTH_EXCEEDED",
        std::to_string(input.usage.queue_depth));
  }
  if (input.budget.min_run_interval_microseconds != 0 &&
      input.usage.last_run_end_microseconds != 0 &&
      context.monotonic_now_microseconds > input.usage.last_run_end_microseconds &&
      context.monotonic_now_microseconds - input.usage.last_run_end_microseconds <
          input.budget.min_run_interval_microseconds) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::throttle_defer,
        AgentResourceBudgetDimension::cadence,
        "SB_AGENT_RESOURCE_BUDGET.CADENCE_NOT_DUE",
        std::to_string(input.usage.last_run_end_microseconds));
  }
  if (input.budget.retry_backoff_microseconds != 0 &&
      input.usage.last_failure_microseconds != 0 &&
      context.monotonic_now_microseconds > input.usage.last_failure_microseconds &&
      context.monotonic_now_microseconds - input.usage.last_failure_microseconds <
          input.budget.retry_backoff_microseconds) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::throttle_defer,
        AgentResourceBudgetDimension::retry_backoff,
        "SB_AGENT_RESOURCE_BUDGET.RETRY_BACKOFF_ACTIVE",
        std::to_string(input.usage.last_failure_microseconds));
  }
  if (OverLimit(input.budget.max_history_query_rows,
                input.usage.history_query_rows)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::fail_closed,
        AgentResourceBudgetDimension::history_rows,
        "SB_AGENT_RESOURCE_BUDGET.HISTORY_ROWS_EXCEEDED",
        std::to_string(input.usage.history_query_rows));
  }
  if (OverLimit(input.budget.max_evidence_fanout,
                input.usage.evidence_fanout)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::fail_closed,
        AgentResourceBudgetDimension::evidence_fanout,
        "SB_AGENT_RESOURCE_BUDGET.EVIDENCE_FANOUT_EXCEEDED",
        std::to_string(input.usage.evidence_fanout));
  }
  if (OverLimit(input.budget.max_label_cardinality,
                input.usage.label_cardinality)) {
    return BuildResourceBudgetDecision(
        descriptor, policy, AgentResourceBudgetDecisionKind::fail_closed,
        AgentResourceBudgetDimension::label_cardinality,
        "SB_AGENT_RESOURCE_BUDGET.LABEL_CARDINALITY_EXCEEDED",
        std::to_string(input.usage.label_cardinality));
  }
  return BuildResourceBudgetDecision(
      descriptor, policy, AgentResourceBudgetDecisionKind::allow,
      AgentResourceBudgetDimension::foreground_protection,
      "SB_AGENT_RESOURCE_BUDGET.ALLOW", descriptor.type_id);
}

namespace {

bool AgentIsDmlPreworkCapacityTarget(const std::string& agent_type_id) {
  return agent_type_id == "page_allocation_manager" ||
         agent_type_id == "filespace_capacity_manager" ||
         agent_type_id == "storage_health_manager" ||
         agent_type_id == "transaction_pressure_manager" ||
         agent_type_id == "storage_version_cleanup_agent";
}

bool AgentHasClusterWorkerCapacityDependency(const AgentTypeDescriptor& descriptor) {
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (dependency.cluster_only) {
      return true;
    }
  }
  return descriptor.scope.find("cluster") != std::string::npos;
}

u64 ObservedCpuCountOrHost(const AgentWorkerCapacityConfig& config) {
  if (config.observed_cpu_count != 0) {
    return config.observed_cpu_count;
  }
  const auto hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : static_cast<u64>(hardware);
}

std::string WorkerCapacityEvidenceUuid(const std::string& agent_type_id,
                                       const std::string& code,
                                       u64 effective_cpu_count,
                                       u64 background_worker_slots) {
  return DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_worker_capacity|" + agent_type_id + "|" + code + "|" +
      std::to_string(effective_cpu_count) + "|" +
      std::to_string(background_worker_slots));
}

AgentWorkerCapacityAssignment WorkerCapacityRefusal(
    const AgentWorkerCapacityCandidate& candidate,
    std::string code,
    std::string detail,
    u64 effective_cpu_count,
    u64 background_worker_slots,
    bool cluster_path_failed_closed = false) {
  AgentWorkerCapacityAssignment assignment;
  assignment.agent_type_id = candidate.agent_type_id;
  assignment.dml_prework_agent = candidate.dml_prework_agent ||
                                 AgentIsDmlPreworkCapacityTarget(candidate.agent_type_id);
  assignment.requested_worker_slots = candidate.requested_worker_slots;
  assignment.cluster_path_failed_closed = cluster_path_failed_closed;
  assignment.diagnostic_code = std::move(code);
  assignment.detail = std::move(detail);
  assignment.evidence_uuid = WorkerCapacityEvidenceUuid(
      assignment.agent_type_id, assignment.diagnostic_code, effective_cpu_count,
      background_worker_slots);
  return assignment;
}

}  // namespace

std::vector<AgentWorkerCapacityCandidate> DefaultDmlPreworkAgentWorkerCandidates(
    u64 policy_generation) {
  std::vector<AgentWorkerCapacityCandidate> candidates;
  for (const char* agent_type_id : {"page_allocation_manager",
                                    "filespace_capacity_manager",
                                    "storage_health_manager",
                                    "transaction_pressure_manager",
                                    "storage_version_cleanup_agent"}) {
    const auto descriptor = FindAgentType(agent_type_id);
    if (!descriptor.has_value()) {
      continue;
    }
    AgentWorkerCapacityCandidate candidate;
    candidate.agent_type_id = descriptor->type_id;
    candidate.policy = BaselinePolicyForAgent(*descriptor);
    candidate.policy.policy_generation = policy_generation == 0 ? 1 : policy_generation;
    candidate.policy.config_fields["max_thread_slots"] = "1";
    candidate.requested_worker_slots = 1;
    candidate.dml_prework_agent = true;
    candidate.may_precede_foreground_demand = true;
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

AgentWorkerCapacitySnapshot PlanAgentWorkerCapacity(
    const AgentWorkerCapacityConfig& config,
    const AgentRuntimeContext& context,
    const std::vector<AgentWorkerCapacityCandidate>& candidates) {
  AgentWorkerCapacitySnapshot snapshot;
  snapshot.observed_cpu_count = ObservedCpuCountOrHost(config);
  snapshot.configured_cpu_count = config.configured_cpu_count;
  snapshot.effective_cpu_count = config.configured_cpu_count != 0
      ? config.configured_cpu_count
      : snapshot.observed_cpu_count;
  if (snapshot.effective_cpu_count == 0) {
    snapshot.effective_cpu_count = 1;
  }
  snapshot.foreground_reserved_capacity =
      std::min(config.foreground_reserved_capacity, snapshot.effective_cpu_count);
  snapshot.foreground_capacity_reserved = snapshot.foreground_reserved_capacity != 0;
  snapshot.background_worker_slots =
      snapshot.effective_cpu_count > snapshot.foreground_reserved_capacity
          ? snapshot.effective_cpu_count - snapshot.foreground_reserved_capacity
          : 0;
  if (config.max_background_worker_slots != 0) {
    snapshot.background_worker_slots =
        std::min(snapshot.background_worker_slots, config.max_background_worker_slots);
  }
  snapshot.background_capacity_available = snapshot.background_worker_slots != 0;
  snapshot.foreground_work_active = config.foreground_database_work_active;
  snapshot.status = AgentOk();

  u64 next_slot = 1;
  for (const auto& candidate : candidates) {
    const auto descriptor = FindAgentType(candidate.agent_type_id);
    if (!descriptor.has_value()) {
      auto assignment = WorkerCapacityRefusal(
          candidate, "ENGINE.AGENT_WORKER_CAPACITY.AGENT_NOT_FOUND",
          candidate.agent_type_id, snapshot.effective_cpu_count,
          snapshot.background_worker_slots);
      assignment.resource_decision = AgentResourceBudgetDecisionKind::fail_closed;
      snapshot.resource_bounds_blocked_work = true;
      snapshot.diagnostics.push_back(assignment.diagnostic_code + ":" + candidate.agent_type_id);
      snapshot.assignments.push_back(std::move(assignment));
      continue;
    }

    if ((descriptor->cluster_only ||
         descriptor->deployment == AgentDeployment::cluster) &&
        (config.standalone_edition || !config.cluster_authority_available)) {
      auto assignment = WorkerCapacityRefusal(
          candidate, "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED",
          "cluster_only_agent", snapshot.effective_cpu_count,
          snapshot.background_worker_slots, true);
      assignment.resource_decision = AgentResourceBudgetDecisionKind::fail_closed;
      snapshot.cluster_paths_failed_closed = true;
      snapshot.resource_bounds_blocked_work = true;
      snapshot.diagnostics.push_back(assignment.diagnostic_code + ":" + candidate.agent_type_id);
      snapshot.assignments.push_back(std::move(assignment));
      continue;
    }
    const bool local_projection_with_cluster_path_failed_closed =
        descriptor->deployment == AgentDeployment::both &&
        (config.standalone_edition || !config.cluster_authority_available) &&
        AgentHasClusterWorkerCapacityDependency(*descriptor);

    AgentResourceBudgetEvaluationInput budget_input;
    budget_input.budget = DefaultAgentResourceBudgetForPolicy(candidate.policy);
    budget_input.usage = candidate.usage;
    budget_input.foreground_database_work_active =
        config.foreground_database_work_active;
    budget_input.usage.thread_slots =
        std::max(budget_input.usage.thread_slots, candidate.requested_worker_slots);

    const auto budget = EvaluateAgentResourceBudget(
        *descriptor, candidate.policy, context, budget_input);
    AgentWorkerCapacityAssignment assignment;
    assignment.agent_type_id = candidate.agent_type_id;
    assignment.dml_prework_agent = candidate.dml_prework_agent ||
                                   AgentIsDmlPreworkCapacityTarget(candidate.agent_type_id);
    assignment.cluster_path_failed_closed =
        local_projection_with_cluster_path_failed_closed;
    if (assignment.cluster_path_failed_closed) {
      snapshot.cluster_paths_failed_closed = true;
    }
    assignment.requested_worker_slots = candidate.requested_worker_slots;
    assignment.resource_decision = budget.decision;
    if (!budget.diagnostics.empty()) {
      assignment.resource_dimension = budget.diagnostics.front().dimension;
    }
    assignment.diagnostic_code = budget.status.diagnostic_code;
    assignment.detail = budget.status.detail;
    assignment.evidence_uuid = budget.evidence_uuid.empty()
        ? WorkerCapacityEvidenceUuid(candidate.agent_type_id,
                                     assignment.diagnostic_code,
                                     snapshot.effective_cpu_count,
                                     snapshot.background_worker_slots)
        : budget.evidence_uuid;

    if (budget.decision != AgentResourceBudgetDecisionKind::allow) {
      snapshot.resource_bounds_blocked_work = true;
      snapshot.diagnostics.push_back(assignment.diagnostic_code + ":" + candidate.agent_type_id);
      snapshot.assignments.push_back(std::move(assignment));
      continue;
    }
    if (candidate.requested_worker_slots == 0 ||
        next_slot + candidate.requested_worker_slots - 1 >
            snapshot.background_worker_slots) {
      assignment.diagnostic_code = "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION";
      assignment.detail = "foreground_reserved_capacity_blocks_background_slot";
      assignment.evidence_uuid = WorkerCapacityEvidenceUuid(
          candidate.agent_type_id, assignment.diagnostic_code,
          snapshot.effective_cpu_count, snapshot.background_worker_slots);
      assignment.resource_decision =
          AgentResourceBudgetDecisionKind::foreground_protection;
      assignment.resource_dimension =
          AgentResourceBudgetDimension::foreground_protection;
      snapshot.resource_bounds_blocked_work = true;
      snapshot.diagnostics.push_back(assignment.diagnostic_code + ":" + candidate.agent_type_id);
      snapshot.assignments.push_back(std::move(assignment));
      continue;
    }

    assignment.selected = true;
    assignment.assigned = true;
    assignment.worker_slot_index = next_slot;
    assignment.can_run_before_foreground_demand =
        candidate.may_precede_foreground_demand &&
        !config.foreground_database_work_active &&
        snapshot.foreground_capacity_reserved;
    assignment.diagnostic_code = "ENGINE.AGENT_WORKER_CAPACITY.ASSIGNED";
    assignment.detail = "background_worker_slot";
    assignment.evidence_uuid = WorkerCapacityEvidenceUuid(
        candidate.agent_type_id, assignment.diagnostic_code,
        snapshot.effective_cpu_count, snapshot.background_worker_slots);
    next_slot += candidate.requested_worker_slots;
    snapshot.assignments.push_back(std::move(assignment));
  }

  snapshot.diagnostics.push_back("ENGINE.AGENT_WORKER_CAPACITY.SNAPSHOT");
  return snapshot;
}

AgentActionDecision VerifyActionOutcome(const AgentActionRequest& action,
                                        bool subsystem_reported_success,
                                        bool intended_state_observed) {
  if (action.dry_run) {
    return {AgentActionResultClass::dry_run_only, "SB_AGENT_ACTION.OUTCOME_DRY_RUN",
            action.action_uuid, DurableObjectUuidFromKey("agent_outcome|" + action.action_uuid), false};
  }
  if (subsystem_reported_success && intended_state_observed) {
    return {AgentActionResultClass::accepted, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
            action.action_uuid, DurableObjectUuidFromKey("agent_outcome|" + action.action_uuid), true};
  }
  return {AgentActionResultClass::failed_closed, "SB_AGENT_ACTION.OUTCOME_UNVERIFIED",
          action.action_uuid, DurableObjectUuidFromKey("agent_outcome_failed|" + action.action_uuid), false};
}

int AgentArbitrationActionClassPriority(AgentArbitrationActionClass action_class) {
  switch (action_class) {
    case AgentArbitrationActionClass::protect_correctness: return 100;
    case AgentArbitrationActionClass::protect_security: return 95;
    case AgentArbitrationActionClass::protect_durability: return 90;
    case AgentArbitrationActionClass::protect_availability: return 80;
    case AgentArbitrationActionClass::reduce_pressure: return 60;
    case AgentArbitrationActionClass::optimize_performance: return 40;
    case AgentArbitrationActionClass::reduce_cost: return 20;
  }
  return 0;
}

AgentArbitrationCandidate NormalizeAgentActionForArbitration(
    const AgentRuntimeContext& context,
    const AgentActionRequest& action) {
  AgentArbitrationCandidate candidate;
  candidate.action_uuid = action.action_uuid;
  candidate.agent_type_id = action.agent_type_id;
  candidate.instance_uuid = action.instance_uuid;
  candidate.policy_uuid = InputOr(action, "policy_uuid", InputOr(action, "policy"));
  candidate.scope_uuid = InputOr(action, "scope_uuid", InputOr(action, "scope", context.database_uuid));
  candidate.actuator_id = action.actuator_id;
  candidate.operation_id = action.operation_id;
  candidate.action_class = ParseArbitrationActionClass(
      InputOr(action, "arbitration_action_class", InputOr(action, "action_class")),
      action.action_class);
  candidate.risk = ParseArbitrationRisk(InputOr(action, "risk", "medium"));
  candidate.reversibility = ParseArbitrationReversibility(
      InputOr(action, "reversibility", "reversible"));
  candidate.evidence_quality = ParseU64Input(action, "evidence_quality", 0);
  candidate.evidence_uuid =
      InputOr(action, "evidence_uuid", DurableObjectUuidFromKey("agent_action_evidence|" +
                                                                action.action_uuid));
  candidate.dry_run = action.dry_run;
  const auto safety = InputOr(action, "safety_preconditions_passed",
                             InputOr(action, "safety_precondition", "true"));
  candidate.safety_preconditions_passed = !IsFalseValue(safety);
  candidate.safety_diagnostic_code =
      InputOr(action, "safety_diagnostic_code",
              "SB_AGENT_ARBITRATION.SAFETY_PRECONDITION_FAILED");
  return candidate;
}

AgentArbitrationRecord ArbitrateAgentActionCandidates(
    const AgentRuntimeContext& context,
    const std::vector<AgentArbitrationCandidate>& candidates,
    const std::vector<AgentArbitrationOverride>& overrides) {
  AgentArbitrationRecord record;
  record.normalized_actions = candidates;
  record.policy_uuid = FirstPolicyUuid(candidates);

  auto append_losing = [&record](const std::string& action_uuid) {
    if (action_uuid.empty()) { return; }
    if (!Contains(record.losing_action_uuids, action_uuid)) {
      record.losing_action_uuids.push_back(action_uuid);
    }
  };
  auto append_losing_candidates = [&append_losing](const std::vector<AgentArbitrationCandidate>& values) {
    for (const auto& value : values) { append_losing(value.action_uuid); }
  };
  auto finish = [&record, &context](AgentArbitrationOutcome outcome,
                                   AgentArbitrationPriorityRule priority_rule,
                                   std::string diagnostic_code,
                                   std::string detail) {
    record.outcome = outcome;
    record.priority_rule = priority_rule;
    record.diagnostic_code = std::move(diagnostic_code);
    record.detail = std::move(detail);
    std::vector<std::string> evidence_actions = record.losing_action_uuids;
    if (!record.winning_action_uuid.empty()) {
      evidence_actions.push_back(record.winning_action_uuid);
    }
    record.evidence_uuid = ArbitrationEvidenceUuid(
        context, AgentArbitrationOutcomeName(record.outcome), evidence_actions);
    if (record.policy_uuid.empty()) {
      const auto* winner = FindCandidateByUuid(record.normalized_actions,
                                               record.winning_action_uuid);
      if (winner != nullptr) { record.policy_uuid = winner->policy_uuid; }
    }
    return record;
  };

  if (candidates.empty()) {
    return finish(AgentArbitrationOutcome::both_denied,
                  AgentArbitrationPriorityRule::no_actions,
                  "SB_AGENT_ARBITRATION.NO_ACTIONS",
                  "no-actions");
  }

  std::vector<AgentArbitrationCandidate> valid;
  for (const auto& candidate : candidates) {
    if (!candidate.safety_preconditions_passed) {
      append_losing(candidate.action_uuid);
      if (record.detail.empty()) { record.detail = candidate.safety_diagnostic_code; }
      continue;
    }
    valid.push_back(candidate);
  }

  if (valid.empty()) {
    return finish(AgentArbitrationOutcome::both_denied,
                  AgentArbitrationPriorityRule::safety_precondition_failed,
                  "SB_AGENT_ARBITRATION.SAFETY_PRECONDITION_FAILED",
                  record.detail.empty() ? "safety-precondition-failed" : record.detail);
  }

  bool suppressed_by_override = false;
  for (const auto& override_record : overrides) {
    if (!OverrideIsActiveAt(override_record, context.wall_now_microseconds)) { continue; }

    bool override_matches = false;
    for (const auto& candidate : valid) {
      if (!ScopeOverlaps(override_record.scope_uuid, candidate.scope_uuid)) { continue; }
      if (OverrideListsAction(override_record.suppressed_action_uuids, candidate) ||
          OverrideListsAction(override_record.allowed_action_uuids, candidate)) {
        override_matches = true;
        break;
      }
    }
    if (!override_matches) { continue; }

    if (!AgentContextHasRight(context, "OBS_AGENT_OVERRIDE")) {
      append_losing_candidates(valid);
      record.override_uuid = override_record.override_uuid;
      return finish(AgentArbitrationOutcome::both_denied,
                    AgentArbitrationPriorityRule::override_right_required,
                    "SB_AGENT_OVERRIDE.RIGHT_REQUIRED",
                    override_record.override_uuid);
    }

    for (const auto& candidate : valid) {
      if (!ScopeOverlaps(override_record.scope_uuid, candidate.scope_uuid)) { continue; }
      if (OverrideListsAction(override_record.allowed_action_uuids, candidate) &&
          CandidateTouchesForbiddenOverrideAuthority(candidate)) {
        append_losing_candidates(valid);
        record.override_uuid = override_record.override_uuid;
        return finish(AgentArbitrationOutcome::both_denied,
                      AgentArbitrationPriorityRule::override_authority_forbidden,
                      "SB_AGENT_OVERRIDE.AUTHORITY_GRANT_FORBIDDEN",
                      candidate.action_uuid);
      }
    }

    std::vector<AgentArbitrationCandidate> unsuppressed;
    for (const auto& candidate : valid) {
      const bool scope_match = ScopeOverlaps(override_record.scope_uuid, candidate.scope_uuid);
      const bool explicitly_allowed = scope_match &&
          OverrideListsAction(override_record.allowed_action_uuids, candidate);
      const bool explicitly_suppressed = scope_match &&
          OverrideListsAction(override_record.suppressed_action_uuids, candidate);
      if (explicitly_suppressed && !explicitly_allowed) {
        append_losing(candidate.action_uuid);
        suppressed_by_override = true;
        if (record.override_uuid.empty()) { record.override_uuid = override_record.override_uuid; }
        continue;
      }
      unsuppressed.push_back(candidate);
    }
    valid = std::move(unsuppressed);
  }

  if (valid.empty()) {
    return finish(AgentArbitrationOutcome::suppressed_by_override,
                  AgentArbitrationPriorityRule::override_suppression,
                  "SB_AGENT_ARBITRATION.SUPPRESSED_BY_OVERRIDE",
                  record.override_uuid);
  }

  if (suppressed_by_override && valid.size() == 1) {
    record.winning_action_uuid = valid.front().action_uuid;
    record.policy_uuid = valid.front().policy_uuid;
    return finish(AgentArbitrationOutcome::suppressed_by_override,
                  AgentArbitrationPriorityRule::override_suppression,
                  "SB_AGENT_ARBITRATION.SUPPRESSED_BY_OVERRIDE",
                  record.override_uuid);
  }

  if (valid.size() == 1) {
    record.winning_action_uuid = valid.front().action_uuid;
    record.policy_uuid = valid.front().policy_uuid;
    return finish(AgentArbitrationOutcome::winner_executes,
                  AgentArbitrationPriorityRule::single_action,
                  "SB_AGENT_ARBITRATION.WINNER_EXECUTES",
                  valid.front().action_uuid);
  }

  int highest_priority = -1;
  for (const auto& candidate : valid) {
    highest_priority = std::max(highest_priority,
                                AgentArbitrationActionClassPriority(candidate.action_class));
  }

  std::vector<AgentArbitrationCandidate> priority_tied;
  for (const auto& candidate : valid) {
    if (AgentArbitrationActionClassPriority(candidate.action_class) == highest_priority) {
      priority_tied.push_back(candidate);
    }
  }

  if (priority_tied.size() == 1) {
    record.winning_action_uuid = priority_tied.front().action_uuid;
    record.policy_uuid = priority_tied.front().policy_uuid;
    for (const auto& candidate : valid) {
      if (candidate.action_uuid != record.winning_action_uuid) {
        append_losing(candidate.action_uuid);
      }
    }
    return finish(AgentArbitrationOutcome::winner_executes,
                  AgentArbitrationPriorityRule::action_class_priority,
                  "SB_AGENT_ARBITRATION.WINNER_EXECUTES",
                  AgentArbitrationActionClassName(priority_tied.front().action_class));
  }

  u64 highest_evidence_quality = 0;
  for (const auto& candidate : priority_tied) {
    highest_evidence_quality = std::max(highest_evidence_quality, candidate.evidence_quality);
  }

  std::vector<AgentArbitrationCandidate> evidence_tied;
  for (const auto& candidate : priority_tied) {
    if (candidate.evidence_quality == highest_evidence_quality) {
      evidence_tied.push_back(candidate);
    }
  }

  if (evidence_tied.size() == 1) {
    record.winning_action_uuid = evidence_tied.front().action_uuid;
    record.policy_uuid = evidence_tied.front().policy_uuid;
    for (const auto& candidate : valid) {
      if (candidate.action_uuid != record.winning_action_uuid) {
        append_losing(candidate.action_uuid);
      }
    }
    return finish(AgentArbitrationOutcome::winner_executes,
                  AgentArbitrationPriorityRule::evidence_quality,
                  "SB_AGENT_ARBITRATION.WINNER_EXECUTES",
                  evidence_tied.front().evidence_uuid);
  }

  append_losing_candidates(valid);
  record.operator_review_action_created = true;
  record.operator_review_action_uuid =
      "agent-action:operator-review:" +
      StableUuidFromKey(Join(CandidateUuids(evidence_tied), ","));
  return finish(AgentArbitrationOutcome::operator_review_required,
                AgentArbitrationPriorityRule::exact_tie_operator_review,
                "SB_AGENT_ARBITRATION.OPERATOR_REVIEW_REQUIRED",
                record.operator_review_action_uuid);
}

AgentArbitrationRecord ArbitrateAgentActionsDetailed(
    const AgentRuntimeContext& context,
    const std::vector<AgentActionRequest>& actions,
    const std::vector<AgentArbitrationOverride>& overrides) {
  std::vector<AgentArbitrationCandidate> candidates;
  candidates.reserve(actions.size());
  for (const auto& action : actions) {
    candidates.push_back(NormalizeAgentActionForArbitration(context, action));
  }

  for (const auto& action : actions) {
    if (!action.operator_override) { continue; }
    AgentArbitrationRecord record;
    record.normalized_actions = candidates;
    const auto* candidate = FindCandidateByUuid(candidates, action.action_uuid);
    if (candidate != nullptr) { record.policy_uuid = candidate->policy_uuid; }
    auto append_losing = [&record](const std::string& action_uuid) {
      if (!Contains(record.losing_action_uuids, action_uuid)) {
        record.losing_action_uuids.push_back(action_uuid);
      }
    };
    auto finish = [&record, &context](AgentArbitrationOutcome outcome,
                                     AgentArbitrationPriorityRule priority_rule,
                                     std::string diagnostic_code,
                                     std::string detail) {
      record.outcome = outcome;
      record.priority_rule = priority_rule;
      record.diagnostic_code = std::move(diagnostic_code);
      record.detail = std::move(detail);
      std::vector<std::string> evidence_actions = record.losing_action_uuids;
      if (!record.winning_action_uuid.empty()) {
        evidence_actions.push_back(record.winning_action_uuid);
      }
      record.evidence_uuid = ArbitrationEvidenceUuid(
          context, AgentArbitrationOutcomeName(record.outcome), evidence_actions);
      return record;
    };

    const auto override_status = ValidateOperatorOverride(context, action);
    if (!override_status.ok) {
      append_losing(action.action_uuid);
      return finish(AgentArbitrationOutcome::both_denied,
                    AgentArbitrationPriorityRule::override_right_required,
                    override_status.diagnostic_code,
                    override_status.detail);
    }
    if (candidate != nullptr && CandidateTouchesForbiddenOverrideAuthority(*candidate)) {
      append_losing(action.action_uuid);
      return finish(AgentArbitrationOutcome::both_denied,
                    AgentArbitrationPriorityRule::override_authority_forbidden,
                    "SB_AGENT_OVERRIDE.AUTHORITY_GRANT_FORBIDDEN",
                    action.action_uuid);
    }
    record.winning_action_uuid = action.action_uuid;
    return finish(AgentArbitrationOutcome::winner_executes,
                  AgentArbitrationPriorityRule::override_suppression,
                  "SB_AGENT_ARBITRATION.WINNER_EXECUTES",
                  action.action_uuid);
  }

  return ArbitrateAgentActionCandidates(context, candidates, overrides);
}

AgentActionDecision ArbitrateAgentActions(const AgentRuntimeContext& context,
                                          const std::vector<AgentActionRequest>& actions) {
  const auto record = ArbitrateAgentActionsDetailed(context, actions);
  switch (record.outcome) {
    case AgentArbitrationOutcome::winner_executes: {
      bool mutates_state = false;
      for (const auto& candidate : record.normalized_actions) {
        if (candidate.action_uuid == record.winning_action_uuid) {
          mutates_state = !candidate.dry_run;
          break;
        }
      }
      return {AgentActionResultClass::accepted, record.diagnostic_code,
              record.winning_action_uuid, record.evidence_uuid, mutates_state};
    }
    case AgentArbitrationOutcome::suppressed_by_override:
      return {AgentActionResultClass::suppressed, record.diagnostic_code,
              record.winning_action_uuid.empty() ? Join(record.losing_action_uuids, ",")
                                                 : record.winning_action_uuid,
              record.evidence_uuid, false};
    case AgentArbitrationOutcome::operator_review_required:
      return {AgentActionResultClass::approval_required, record.diagnostic_code,
              record.operator_review_action_uuid, record.evidence_uuid, false};
    case AgentArbitrationOutcome::both_denied:
      return {AgentActionResultClass::refused, record.diagnostic_code,
              Join(record.losing_action_uuids, ","), record.evidence_uuid, false};
  }
  return {AgentActionResultClass::failed_closed, "SB_AGENT_ARBITRATION.BOTH_DENIED",
          {}, record.evidence_uuid, false};
}

AgentRuntimeStatus ValidateHumanCommandPrecedence(const std::string& human_command_class,
                                                  const AgentActionRequest& agent_action) {
  if (human_command_class == "emergency_shutdown" || human_command_class == "security_control" ||
      human_command_class == "restore" || human_command_class == "backup") {
    if (!agent_action.dry_run) { return AgentError("SB_AGENT_PRECEDENCE.HUMAN_COMMAND_WINS", human_command_class); }
  }
  return AgentOk();
}

AgentRuntimeStatus AcquireAgentRunLease(AgentInstanceRecord* instance,
                                        const AgentPolicy& policy,
                                        u64 now_microseconds) {
  if (instance == nullptr) { return AgentError("SB_AGENT_SCHEDULER.INSTANCE_REQUIRED"); }
  const auto enforce = EnforceAgentSupervisionSafety(instance);
  if (!enforce.ok) { return enforce; }
  if (!AgentInstanceAllowsRunLease(*instance)) {
    instance->lease_until_microseconds = 0;
    return AgentError("SB_AGENT_SCHEDULER.INSTANCE_NOT_RUNNABLE", instance->instance_uuid);
  }
  if (instance->restart_not_before_microseconds > now_microseconds) {
    return AgentError("SB_AGENT_RESTART.BACKOFF_ACTIVE", instance->instance_uuid);
  }
  if (instance->cooldown_until_microseconds > now_microseconds) {
    return AgentError("SB_AGENT_ACTION.RETRY_COOLDOWN_ACTIVE", instance->instance_uuid);
  }
  if (instance->lease_until_microseconds > now_microseconds) { return AgentError("SB_AGENT_SCHEDULER.LEASE_HELD", instance->instance_uuid); }
  instance->lease_until_microseconds = now_microseconds + policy.lease_microseconds;
  instance->last_run_start_microseconds = now_microseconds;
  ++instance->run_generation;
  return {true, "SB_AGENT_SCHEDULER.LEASE_ACQUIRED", instance->instance_uuid};
}

u64 AgentRestartBackoffMicroseconds(const AgentPolicy& policy, u64 restart_attempt) {
  if (restart_attempt == 0) { return 0; }
  u64 backoff = policy.initial_backoff_microseconds;
  for (u64 i = 1; i < restart_attempt && backoff < policy.max_backoff_microseconds; ++i) {
    if (backoff > policy.max_backoff_microseconds / 2) {
      backoff = policy.max_backoff_microseconds;
    } else {
      backoff *= 2;
    }
  }
  return std::min(backoff, policy.max_backoff_microseconds);
}

bool AgentInstanceAllowsRunLease(const AgentInstanceRecord& instance) {
  if (instance.quarantined || instance.disabled_by_operator || instance.safe_mode) {
    return false;
  }
  return IsSupervisionRunnableState(instance.state);
}

AgentRuntimeStatus EnforceAgentSupervisionSafety(AgentInstanceRecord* instance) {
  if (instance == nullptr) {
    return AgentError("SB_AGENT_SUPERVISION.INSTANCE_REQUIRED");
  }
  if (instance->quarantined) {
    instance->state = AgentLifecycleState::quarantined;
  } else if (instance->disabled_by_operator) {
    instance->state = AgentLifecycleState::disabled;
  } else if (instance->safe_mode) {
    instance->state = AgentLifecycleState::safe_mode;
  }
  if (IsUnsafeLeaseState(instance->state)) {
    instance->lease_until_microseconds = 0;
  }
  return AgentOk();
}

AgentSupervisionDecision RecordAgentSupervisionFailure(AgentInstanceRecord* instance,
                                                       const AgentPolicy& policy,
                                                       AgentSupervisionFailureKind failure_kind,
                                                       u64 now_microseconds,
                                                       std::string detail) {
  AgentSupervisionDecision decision;
  if (instance == nullptr) {
    decision.status = AgentError("SB_AGENT_SUPERVISION.INSTANCE_REQUIRED");
    return decision;
  }
  if (instance->state == AgentLifecycleState::quarantined || instance->quarantined) {
    ApplySupervisionState(instance, AgentLifecycleState::quarantined);
    decision.status = AgentError("SB_AGENT_RESTART.QUARANTINED", instance->instance_uuid);
    decision.state = instance->state;
    decision.quarantined = true;
    decision.lease_cleared = true;
    return decision;
  }

  const std::string diagnostic = SupervisionFailureDiagnostic(failure_kind);
  const u64 next_restart_attempt = instance->restart_attempts + 1;
  const bool restart_limit_exhausted = next_restart_attempt > policy.max_restart_attempts;
  const u64 previous_lease = instance->lease_until_microseconds;

  instance->lease_until_microseconds = 0;
  instance->last_run_end_microseconds = now_microseconds;
  instance->cancellation_requested = false;
  instance->last_failure_diagnostic_code = diagnostic;
  instance->last_supervision_detail = detail.empty()
      ? AgentSupervisionFailureKindName(failure_kind)
      : std::move(detail);
  ++instance->supervision_failure_count;
  ++instance->crash_loop_count;
  instance->restart_attempts = next_restart_attempt;

  decision.failure_count = instance->supervision_failure_count;
  decision.restart_attempts = instance->restart_attempts;
  decision.lease_cleared = previous_lease != 0 || instance->lease_until_microseconds == 0;

  if (restart_limit_exhausted) {
    ApplySupervisionState(instance, AgentLifecycleState::quarantined);
    instance->restart_not_before_microseconds = 0;
    instance->cooldown_until_microseconds = 0;
    instance->last_failure_diagnostic_code = "SB_AGENT_RESTART.LIMIT_EXHAUSTED";
    decision.status = AgentError("SB_AGENT_RESTART.LIMIT_EXHAUSTED", instance->instance_uuid);
    decision.state = instance->state;
    decision.quarantined = true;
    return decision;
  }

  const u64 backoff = AgentRestartBackoffMicroseconds(policy, instance->restart_attempts);
  ApplySupervisionState(instance, AgentLifecycleState::failed);
  instance->restart_not_before_microseconds = now_microseconds + backoff;
  if (failure_kind == AgentSupervisionFailureKind::action_failed) {
    instance->cooldown_until_microseconds = now_microseconds + policy.cooldown_microseconds;
  }

  decision.status = AgentError(diagnostic, instance->instance_uuid);
  decision.state = instance->state;
  decision.backoff_microseconds = backoff;
  decision.restart_not_before_microseconds = instance->restart_not_before_microseconds;
  decision.cooldown_until_microseconds = instance->cooldown_until_microseconds;
  decision.restart_allowed = false;
  return decision;
}

AgentSupervisionDecision EvaluateAgentSupervisionTick(AgentInstanceRecord* instance,
                                                      const AgentPolicy& policy,
                                                      u64 now_microseconds) {
  AgentSupervisionDecision decision;
  if (instance == nullptr) {
    decision.status = AgentError("SB_AGENT_SUPERVISION.INSTANCE_REQUIRED");
    return decision;
  }
  const auto enforce = EnforceAgentSupervisionSafety(instance);
  if (!enforce.ok) {
    decision.status = enforce;
    return decision;
  }
  decision.state = instance->state;
  decision.failure_count = instance->supervision_failure_count;
  decision.restart_attempts = instance->restart_attempts;
  decision.restart_not_before_microseconds = instance->restart_not_before_microseconds;
  decision.cooldown_until_microseconds = instance->cooldown_until_microseconds;

  if (instance->lease_until_microseconds != 0 &&
      now_microseconds > instance->lease_until_microseconds) {
    return RecordAgentSupervisionFailure(instance, policy,
                                         AgentSupervisionFailureKind::tick_timeout,
                                         now_microseconds,
                                         "lease_expired");
  }
  if (instance->last_run_start_microseconds != 0 &&
      now_microseconds > instance->last_run_start_microseconds &&
      now_microseconds - instance->last_run_start_microseconds >
          policy.max_runtime_microseconds) {
    return RecordAgentSupervisionFailure(instance, policy,
                                         AgentSupervisionFailureKind::runtime_timeout,
                                         now_microseconds,
                                         "max_runtime_exceeded");
  }

  decision.status = {true, "SB_AGENT_SUPERVISION.TICK_OK", instance->instance_uuid};
  decision.restart_allowed = instance->restart_not_before_microseconds == 0 ||
                             instance->restart_not_before_microseconds <= now_microseconds;
  return decision;
}

AgentRuntimeStatus RequestAgentSupervisionRestart(AgentInstanceRecord* instance,
                                                  const AgentPolicy& policy,
                                                  u64 now_microseconds) {
  if (instance == nullptr) {
    return AgentError("SB_AGENT_SUPERVISION.INSTANCE_REQUIRED");
  }
  const auto enforce = EnforceAgentSupervisionSafety(instance);
  if (!enforce.ok) { return enforce; }
  if (instance->state == AgentLifecycleState::quarantined || instance->quarantined) {
    instance->lease_until_microseconds = 0;
    return AgentError("SB_AGENT_RESTART.QUARANTINED", instance->instance_uuid);
  }
  if (instance->restart_attempts > policy.max_restart_attempts) {
    ApplySupervisionState(instance, AgentLifecycleState::quarantined);
    instance->last_failure_diagnostic_code = "SB_AGENT_RESTART.LIMIT_EXHAUSTED";
    return AgentError("SB_AGENT_RESTART.LIMIT_EXHAUSTED", instance->instance_uuid);
  }
  if (instance->restart_not_before_microseconds > now_microseconds) {
    return AgentError("SB_AGENT_RESTART.BACKOFF_ACTIVE", instance->instance_uuid);
  }
  if (instance->state != AgentLifecycleState::failed &&
      instance->state != AgentLifecycleState::registered &&
      instance->state != AgentLifecycleState::paused) {
    return AgentError("SB_AGENT_RESTART.INVALID_STATE", instance->instance_uuid);
  }
  ApplySupervisionState(instance, AgentLifecycleState::registered);
  instance->restart_not_before_microseconds = 0;
  instance->cooldown_until_microseconds = 0;
  instance->last_failure_diagnostic_code.clear();
  return {true, "SB_AGENT_RESTART.ACCEPTED", instance->instance_uuid};
}

AgentRuntimeStatus CancelAgentRun(AgentInstanceRecord* instance,
                                  u64 now_microseconds,
                                  std::string reason) {
  if (instance == nullptr) {
    return AgentError("SB_AGENT_SUPERVISION.INSTANCE_REQUIRED");
  }
  if (instance->state == AgentLifecycleState::quarantined || instance->quarantined) {
    instance->lease_until_microseconds = 0;
    return AgentError("SB_AGENT_SUPERVISION.CANCEL_DENIED_QUARANTINED",
                      instance->instance_uuid);
  }
  instance->lease_until_microseconds = 0;
  instance->last_run_end_microseconds = now_microseconds;
  instance->cancellation_requested = false;
  instance->last_failure_diagnostic_code = "SB_AGENT_SUPERVISION.CANCELLED";
  instance->last_supervision_detail = std::move(reason);
  if (instance->state == AgentLifecycleState::running ||
      instance->state == AgentLifecycleState::dry_run ||
      instance->state == AgentLifecycleState::recommend_only ||
      instance->state == AgentLifecycleState::observe_only) {
    ApplySupervisionState(instance, AgentLifecycleState::paused);
  }
  return {true, "SB_AGENT_SUPERVISION.CANCELLED", instance->instance_uuid};
}

AgentRuntimeStatus QuarantineAgentInstance(AgentInstanceRecord* instance,
                                           u64 now_microseconds,
                                           std::string reason) {
  if (instance == nullptr) {
    return AgentError("SB_AGENT_SUPERVISION.INSTANCE_REQUIRED");
  }
  (void)now_microseconds;
  ApplySupervisionState(instance, AgentLifecycleState::quarantined);
  instance->restart_not_before_microseconds = 0;
  instance->cooldown_until_microseconds = 0;
  instance->last_failure_diagnostic_code = "SB_AGENT_QUARANTINE.APPLIED";
  instance->last_supervision_detail = std::move(reason);
  return {true, "SB_AGENT_QUARANTINE.APPLIED", instance->instance_uuid};
}

AgentRuntimeStatus ValidateAgentActionRetry(const AgentInstanceRecord& instance,
                                            u64 now_microseconds) {
  if (instance.state == AgentLifecycleState::quarantined || instance.quarantined) {
    return AgentError("SB_AGENT_SAFE_MODE.QUARANTINED", instance.instance_uuid);
  }
  if (instance.cooldown_until_microseconds > now_microseconds) {
    return AgentError("SB_AGENT_ACTION.RETRY_COOLDOWN_ACTIVE", instance.instance_uuid);
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateExecutionIsolation(const AgentInstanceRecord& instance,
                                              bool run_threw_exception,
                                              bool watchdog_expired) {
  if (instance.crash_loop_count >= 3) { return AgentError("SB_AGENT_ISOLATION.CRASH_LOOP_QUARANTINE", instance.instance_uuid); }
  if (run_threw_exception) { return AgentError("SB_AGENT_ISOLATION.EXCEPTION_CONTAINED", instance.instance_uuid); }
  if (watchdog_expired) { return AgentError("SB_AGENT_ISOLATION.WATCHDOG_EXPIRED", instance.instance_uuid); }
  return AgentOk();
}

AgentRuntimeStatus ValidateAgentResourceQuota(const AgentPolicy& policy,
                                              u64 runtime_microseconds,
                                              u64 history_rows,
                                              u64 evidence_fanout) {
  if (runtime_microseconds > policy.max_runtime_microseconds) { return AgentError("SB_AGENT_QUOTA.RUNTIME_EXCEEDED"); }
  if (history_rows > policy.max_history_query_rows) { return AgentError("SB_AGENT_QUOTA.HISTORY_ROWS_EXCEEDED"); }
  if (evidence_fanout > policy.max_evidence_fanout) { return AgentError("SB_AGENT_QUOTA.EVIDENCE_FANOUT_EXCEEDED"); }
  return AgentOk();
}

AgentRuntimeStatus ValidateAgentCardinality(const AgentPolicy& policy,
                                            u64 label_cardinality,
                                            u64 evidence_fanout,
                                            u64 history_query_rows) {
  if (label_cardinality > policy.max_label_cardinality) { return AgentError("SB_AGENT_CARDINALITY.LABEL_LIMIT_EXCEEDED"); }
  return ValidateAgentResourceQuota(policy, 0, history_query_rows, evidence_fanout);
}

AgentRuntimeStatus ValidateAgentDependencyGraph(const std::vector<std::string>& dependency_edges) {
  std::set<std::string> seen;
  for (const auto& edge : dependency_edges) {
    if (edge.find("->") == std::string::npos) { return AgentError("SB_AGENT_DEPENDENCY.INVALID_EDGE", edge); }
    if (!seen.insert(edge).second) { return AgentError("SB_AGENT_DEPENDENCY.DUPLICATE_EDGE", edge); }
    const auto pos = edge.find("->");
    if (edge.substr(0, pos) == edge.substr(pos + 2)) { return AgentError("SB_AGENT_DEPENDENCY.SELF_LOOP", edge); }
  }
  return AgentOk();
}

AgentRunDecision ReplayAgentDecision(const AgentRuntimeContext& context,
                                     const AgentTypeDescriptor& descriptor,
                                     const AgentPolicy& policy,
                                     const std::vector<std::string>& captured_metric_families) {
  AgentRunDecision decision;
  for (const auto& dep : descriptor.metric_dependencies) {
    if (std::find(captured_metric_families.begin(), captured_metric_families.end(), dep.metric_family) == captured_metric_families.end()) {
      decision.status = AgentError("SB_AGENT_REPLAY.METRIC_WINDOW_INCOMPLETE", dep.metric_family);
      decision.action = {AgentActionResultClass::failed_closed, decision.status.diagnostic_code, decision.status.detail, "agent-replay-failed", false};
      return decision;
    }
  }
  decision.status = ValidateAgentPolicy(policy, descriptor);
  if (!decision.status.ok) {
    decision.action = {AgentActionResultClass::failed_closed, decision.status.diagnostic_code, decision.status.detail, "agent-replay-policy", false};
    return decision;
  }
  AgentActionRequest action;
  action.action_uuid = DurableObjectUuidFromKey("agent_replay|" + descriptor.type_id);
  action.agent_type_id = descriptor.type_id;
  action.action_class = AgentActionClass::dry_run;
  action.dry_run = true;
  decision.action = BuildDryRunDecision(descriptor, action);
  decision.status = AgentOk();
  decision.explanation_lines = ExplainAgentDecision(descriptor, policy, decision.action);
  (void)context;
  return decision;
}

AgentRuntimeStatus ValidateAgentOverheadGate(const AgentPolicy& policy,
                                             u64 runtime_microseconds,
                                             u64 metric_queries,
                                             u64 evidence_writes) {
  if (runtime_microseconds > policy.max_runtime_microseconds) { return AgentError("SB_AGENT_OVERHEAD.RUNTIME_EXCEEDED"); }
  if (metric_queries > policy.max_history_query_rows) { return AgentError("SB_AGENT_OVERHEAD.METRIC_QUERY_LIMIT_EXCEEDED"); }
  if (evidence_writes > policy.max_evidence_fanout) { return AgentError("SB_AGENT_OVERHEAD.EVIDENCE_WRITE_LIMIT_EXCEEDED"); }
  return AgentOk();
}

AgentRuntimeStatus ValidateAgentStateMigration(u64 from_version, u64 to_version) {
  if (from_version == 0 || to_version == 0) { return AgentError("SB_AGENT_MIGRATION.VERSION_REQUIRED"); }
  if (to_version < from_version) { return AgentError("SB_AGENT_MIGRATION.DOWNGRADE_REQUIRES_EXPLICIT_PLAN"); }
  return AgentOk();
}

AgentActivationProfile EffectiveActivationForLifecycle(AgentActivationProfile configured,
                                                       AgentLifecycleMode mode) {
  if (mode == AgentLifecycleMode::shutdown || mode == AgentLifecycleMode::crash_recovery) { return AgentActivationProfile::disabled; }
  if (IsLifecycleReadonlyMode(mode) && configured == AgentActivationProfile::live_action) { return AgentActivationProfile::dry_run; }
  if (mode == AgentLifecycleMode::maintenance && configured == AgentActivationProfile::live_action) { return AgentActivationProfile::dry_run; }
  return configured;
}

AgentRuntimeStatus ValidateRolloutTransition(AgentActivationProfile from,
                                             AgentActivationProfile to,
                                             bool explicit_operator_approval) {
  if (IsHigherActivation(from, to) && !explicit_operator_approval) {
    return AgentError("SB_AGENT_ROLLOUT.EXPLICIT_APPROVAL_REQUIRED",
                      std::string(AgentActivationProfileName(from)) + "->" + AgentActivationProfileName(to));
  }
  return AgentOk();
}

std::vector<std::string> ExplainAgentDecision(const AgentTypeDescriptor& descriptor,
                                              const AgentPolicy& policy,
                                              const AgentActionDecision& decision) {
  auto bool_text = [](bool value) -> const char* {
    return value ? "true" : "false";
  };

  std::vector<std::string> metric_families = policy.required_metric_families;
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (!Contains(metric_families, dependency.metric_family)) {
      metric_families.push_back(dependency.metric_family);
    }
  }

  const bool failed_closed =
      decision.result_class == AgentActionResultClass::failed_closed;
  const bool refused =
      failed_closed || decision.result_class == AgentActionResultClass::refused ||
      decision.result_class == AgentActionResultClass::approval_required;
  const bool approval_required =
      policy.require_manual_approval ||
      decision.result_class == AgentActionResultClass::approval_required;

  return {
      "agent_type=" + descriptor.type_id,
      "policy=" + policy.policy_uuid,
      "activation=" + std::string(AgentActivationProfileName(policy.activation)),
      "result=" + std::string(AgentActionResultClassName(decision.result_class)),
      "diagnostic=" + decision.diagnostic_code,
      "detail=" + decision.detail,
      "remediation=review policy metrics actuator capability and evidence",
      "explain.row=metrics;agent_type=" + descriptor.type_id +
          ";required_metric_count=" + std::to_string(metric_families.size()) +
          ";families=" + Join(metric_families, "|") +
          ";metrics_are_authority=false",
      "explain.row=policy;policy_uuid=" + policy.policy_uuid +
          ";policy_generation=" + std::to_string(policy.policy_generation) +
          ";enabled=" + bool_text(policy.enabled) +
          ";activation=" + AgentActivationProfileName(policy.activation) +
          ";allow_live_action=" + bool_text(policy.allow_live_action) +
          ";explainability_required=" +
          bool_text(policy.explainability_required),
      "explain.row=dependency;policy_dependency_count=" +
          std::to_string(policy.policy_dependencies.size()) +
          ";dependencies=" + Join(policy.policy_dependencies, "|") +
          ";dependencies_are_authority=false",
      "explain.row=decision;result=" +
          std::string(AgentActionResultClassName(decision.result_class)) +
          ";diagnostic=" + decision.diagnostic_code +
          ";mutates_state=" + bool_text(decision.mutates_state),
      "explain.row=refusal;refused=" + std::string(bool_text(refused)) +
          ";failed_closed=" + bool_text(failed_closed) +
          ";diagnostic=" + decision.diagnostic_code,
      "explain.row=approval;manual_approval_required=" +
          std::string(bool_text(approval_required)) +
          ";operator_approval_required=" + bool_text(approval_required) +
          ";approval_evidence_only=true",
      "explain.row=actuator_outcome;result=" +
          std::string(AgentActionResultClassName(decision.result_class)) +
          ";evidence_uuid=" + decision.evidence_uuid +
          ";mutates_state=" + bool_text(decision.mutates_state) +
          ";provider_authority_required_for_live=true",
      "explain.row=evidence;evidence_uuid=" + decision.evidence_uuid +
          ";authority_scope=evidence_only" +
          ";parser_authority=false;donor_authority=false" +
          ";sidecar_authority=false;transaction_visibility_recovery_authority=false"};
}

std::vector<std::string> AgentDiagnosticCodes() {
  return {"SB_AGENT_OK", "SB_AGENT_REGISTRY.TYPE_ID_REQUIRED", "SB_AGENT_REGISTRY.DUPLICATE_TYPE_ID",
          "SB_AGENT_LIFECYCLE.INVALID_TRANSITION", "SB_AGENT_POLICY.LIVE_ACTION_NOT_ALLOWED",
          "SB_AGENT_POLICY.MISSING",
          "SB_AGENT_SECURITY.RIGHT_REQUIRED", "SB_AGENT_TIME.CLUSTER_MAJORITY_TIME_REQUIRED",
          "SB_AGENT_METRICS.REQUIRED_METRIC_MISSING", "SB_AGENT_ACTUATOR.UNAVAILABLE",
          "SB_AGENT_METRICS.NAMESPACE_MISMATCH", "SB_AGENT_METRICS.CLUSTER_AUTHORITY_REQUIRED",
          "SB_AGENT_METRICS.REQUIRED_METRIC_STALE", "SB_AGENT_METRICS.REQUIRED_METRIC_UNTRUSTED",
          "SB_AGENT_METRICS.NAMESPACE_SCHEMA_INCOMPATIBLE", "SB_AGENT_METRICS.SCOPE_INCOMPATIBLE",
          "SB_AGENT_METRICS.OPTIONAL_METRIC_SUPPRESSED", "SB_AGENT_POLICY_DEPENDENCY.MISSING",
          "SB_AGENT_POLICY_DEPENDENCY.INVALID", "SB_AGENT_POLICY_DEPENDENCY.SCOPE_INCOMPATIBLE",
          "SB_AGENT_ACTION.DRY_RUN_ONLY", "SB_AGENT_ACTION.ACCEPTED", "SB_AGENT_ACTION.OUTCOME_UNVERIFIED",
          "SB_AGENT_APPROVAL.REQUIRED", "SB_AGENT_SAFE_MODE.ACTION_BLOCKED", "SB_AGENT_SAFE_MODE.FAILED",
          "SB_AGENT_SAFE_MODE.QUARANTINED", "SB_AGENT_SCHEDULER.LEASE_ACQUIRED",
          "SB_AGENT_SCHEDULER.LEASE_HELD", "SB_AGENT_SCHEDULER.INSTANCE_NOT_RUNNABLE",
          "SB_AGENT_TICK_HEALTH.SELECTED_RUNNING", "SB_AGENT_TICK_HEALTH.OBSERVE_ONLY",
          "SB_AGENT_TICK_HEALTH.RECOMMEND_ONLY", "SB_AGENT_TICK_HEALTH.DRY_RUN_ONLY",
          "SB_AGENT_TICK_HEALTH.POLICY_DISABLED", "SB_AGENT_TICK_HEALTH.SUPPRESSED",
          "SB_AGENT_TICK_HEALTH.OPERATOR_ONLY",
          "SB_AGENT_RESOURCE_BUDGET.ALLOW",
          "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION",
          "SB_AGENT_RESOURCE_BUDGET.CPU_TIME_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.MEMORY_BYTES_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.IO_BYTES_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.IO_OPS_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.THREAD_SLOTS_EXHAUSTED",
          "SB_AGENT_RESOURCE_BUDGET.QUEUE_DEPTH_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.CADENCE_NOT_DUE",
          "SB_AGENT_RESOURCE_BUDGET.RETRY_BACKOFF_ACTIVE",
          "SB_AGENT_RESOURCE_BUDGET.RUNTIME_TIMEOUT_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.CANCEL_DRAIN_REQUESTED",
          "SB_AGENT_RESOURCE_BUDGET.HISTORY_ROWS_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.EVIDENCE_FANOUT_EXCEEDED",
          "SB_AGENT_RESOURCE_BUDGET.LABEL_CARDINALITY_EXCEEDED",
          "SB_AGENT_ISOLATION.CRASH_LOOP_QUARANTINE", "SB_AGENT_ISOLATION.EXCEPTION_CONTAINED",
          "SB_AGENT_ISOLATION.WATCHDOG_EXPIRED", "SB_AGENT_QUOTA.RUNTIME_EXCEEDED",
          "SB_AGENT_CARDINALITY.LABEL_LIMIT_EXCEEDED", "SB_AGENT_PRECEDENCE.HUMAN_COMMAND_WINS",
          "SB_AGENT_ARBITRATION.NO_ACTIONS", "SB_AGENT_ARBITRATION.WINNER_EXECUTES",
          "SB_AGENT_ARBITRATION.BOTH_DENIED", "SB_AGENT_ARBITRATION.OPERATOR_REVIEW_REQUIRED",
          "SB_AGENT_ARBITRATION.SUPPRESSED_BY_OVERRIDE",
          "SB_AGENT_ARBITRATION.SAFETY_PRECONDITION_FAILED",
          "SB_AGENT_OVERRIDE.RIGHT_REQUIRED", "SB_AGENT_OVERRIDE.AUTHORITY_GRANT_FORBIDDEN",
          "SB_AGENT_SUPERVISION.INSTANCE_REQUIRED", "SB_AGENT_SUPERVISION.TICK_OK", "SB_AGENT_SUPERVISION.TICK_TIMEOUT",
          "SB_AGENT_SUPERVISION.RUNTIME_TIMEOUT", "SB_AGENT_SUPERVISION.CANCELLED",
          "SB_AGENT_SUPERVISION.CANCEL_DENIED_QUARANTINED",
          "SB_AGENT_ACTION.FAILED_BACKOFF", "SB_AGENT_ACTION.RETRY_COOLDOWN_ACTIVE",
          "SB_AGENT_RESTART.BACKOFF_ACTIVE", "SB_AGENT_RESTART.LIMIT_EXHAUSTED",
          "SB_AGENT_RESTART.ACCEPTED", "SB_AGENT_RESTART.QUARANTINED",
          "SB_AGENT_RESTART.INVALID_STATE",
          "SB_AGENT_QUARANTINE.APPLIED",
          "SB_AGENT_FAULT.UNKNOWN_SCENARIO", "SB_AGENT_FAULT.FAIL_CLOSED",
          "SB_AGENT_FAULT.DISK_FULL_FAIL_CLOSED",
          "SB_AGENT_FAULT.FSYNC_FAILURE_FAIL_CLOSED",
          "SB_AGENT_FAULT.PERMISSION_DENIED_FAIL_CLOSED",
          "SB_AGENT_FAULT.CORRUPT_METRIC_FAIL_CLOSED",
          "SB_AGENT_FAULT.INVALID_POLICY_FAIL_CLOSED",
          "SB_AGENT_FAULT.QUEUE_CORRUPTION_QUARANTINE",
          "SB_AGENT_FAULT.PARTIAL_FILESPACE_GROWTH_FAIL_CLOSED",
          "SB_AGENT_FAULT.PARTIAL_PAGE_PREALLOCATION_FAIL_CLOSED"};
}

bool IsKnownAgentDiagnosticCode(const std::string& code) {
  const auto codes = AgentDiagnosticCodes();
  return std::find(codes.begin(), codes.end(), code) != codes.end();
}

namespace {

AgentFaultInjectionScenarioDescriptor FaultScenario(
    std::string scenario_key,
    AgentFaultInjectionClass fault_class,
    std::string diagnostic_code,
    std::string evidence_kind,
    AgentFaultInjectionRecoveryResponse recovery_response,
    bool uses_supervision = false,
    bool uses_arbitration = false) {
  AgentFaultInjectionScenarioDescriptor descriptor;
  descriptor.scenario_key = std::move(scenario_key);
  descriptor.fault_class = fault_class;
  descriptor.diagnostic_code = std::move(diagnostic_code);
  descriptor.evidence_kind = std::move(evidence_kind);
  descriptor.recovery_response = recovery_response;
  descriptor.uses_supervision = uses_supervision;
  descriptor.uses_arbitration = uses_arbitration;
  return descriptor;
}

AgentPolicy FaultInjectionPolicy() {
  AgentPolicy policy;
  policy.policy_uuid = DurableObjectUuidFromKey("agent_fault_injection_policy");
  policy.policy_name = "agent_fault_injection";
  policy.policy_family = "page_preallocation_policy";
  policy.scope = "database/filespace/page_family/page_type";
  policy.run_interval_microseconds = 1000;
  policy.lease_microseconds = 1000;
  policy.cooldown_microseconds = 2000;
  policy.max_runtime_microseconds = 500;
  policy.max_restart_attempts = 2;
  policy.initial_backoff_microseconds = 100;
  policy.max_backoff_microseconds = 1000;
  return policy;
}

AgentInstanceRecord FaultInjectionInstance(const std::string& scenario_key) {
  AgentInstanceRecord instance;
  instance.instance_uuid = DurableObjectUuidFromKey("agent_fault_injection_instance|" + scenario_key);
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = DurableObjectUuidFromKey("agent_fault_injection_policy");
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = AgentLifecycleState::running;
  instance.lease_until_microseconds = 2500;
  instance.last_run_start_microseconds = 500;
  return instance;
}

std::string FaultEvidenceUuid(const std::string& scenario_key) {
  return DurableObjectUuidFromKey("agent_fault_evidence|" + scenario_key);
}

std::string FaultEvidenceDetail(
    const AgentFaultInjectionScenarioDescriptor& descriptor) {
  return "scenario=" + descriptor.scenario_key +
         ";diagnostic=" + descriptor.diagnostic_code +
         ";evidence_before_success=true;success_reported=false;"
         "durable_state_changed=false;recovery=" +
         AgentFaultInjectionRecoveryResponseName(descriptor.recovery_response);
}

AgentRuntimeContext FaultArbitrationContext() {
  AgentRuntimeContext context;
  context.security_context_present = true;
  context.database_uuid = DurableDatabaseUuidFromKey("agent_fault_injection_database");
  context.wall_now_microseconds = 1000;
  context.rights.push_back("OBS_AGENT_STATE_READ");
  context.rights.push_back("OBS_AGENT_CONTROL");
  return context;
}

AgentActionRequest PartialFaultAction(
    const AgentFaultInjectionScenarioDescriptor& descriptor,
    const AgentFaultInjectionResult& result) {
  AgentActionRequest action;
  action.action_uuid = DurableObjectUuidFromKey("agent_fault_action|" + descriptor.scenario_key);
  action.instance_uuid = DurableObjectUuidFromKey("agent_fault_injection_instance|" +
                                                 descriptor.scenario_key);
  action.action_class = AgentActionClass::request_action;
  action.idempotency_key = action.action_uuid + ":idempotent";
  action.dry_run = false;
  action.inputs["arbitration_action_class"] = "protect_durability";
  action.inputs["risk"] = "critical";
  action.inputs["reversibility"] = "bounded_reversible";
  action.inputs["evidence_quality"] = "100";
  action.inputs["evidence_uuid"] = result.evidence_uuid;
  action.inputs["safety_preconditions_passed"] = "false";
  action.inputs["safety_diagnostic_code"] = descriptor.diagnostic_code;
  if (descriptor.scenario_key == "partial_filespace_growth") {
    action.agent_type_id = "filespace_capacity_manager";
    action.actuator_id = "filespace_capacity_manager";
    action.operation_id = "filespace_growth_request";
    action.inputs["scope_uuid"] = DurableDatabaseUuidFromKey("agent_fault_filespace_scope");
    action.inputs["policy_uuid"] =
        DurableObjectUuidFromKey("agent_fault_filespace_capacity_policy");
  } else {
    action.agent_type_id = "page_allocation_manager";
    action.actuator_id = "page_allocation_manager";
    action.operation_id = "page_preallocation_request";
    action.inputs["scope_uuid"] = DurableDatabaseUuidFromKey("agent_fault_page_scope");
    action.inputs["policy_uuid"] =
        DurableObjectUuidFromKey("agent_fault_page_allocation_policy");
  }
  return action;
}

AgentSupervisionDecision EvaluateSupervisionFault(
    const AgentFaultInjectionScenarioDescriptor& descriptor,
    AgentInstanceRecord* instance) {
  const auto policy = FaultInjectionPolicy();
  if (descriptor.scenario_key == "watchdog_timeout") {
    (void)ValidateExecutionIsolation(*instance, false, true);
    return RecordAgentSupervisionFailure(
        instance, policy, AgentSupervisionFailureKind::watchdog_timeout, 1000,
        "fault_injection:watchdog_timeout");
  }
  if (descriptor.scenario_key == "runtime_timeout") {
    instance->last_run_start_microseconds = 400;
    instance->lease_until_microseconds = 2500;
    return EvaluateAgentSupervisionTick(instance, policy, 1001);
  }
  if (descriptor.scenario_key == "restart_mid_action") {
    return RecordAgentSupervisionFailure(
        instance, policy, AgentSupervisionFailureKind::action_failed, 1000,
        "fault_injection:restart_mid_action");
  }
  if (descriptor.scenario_key == "actuator_failure") {
    return RecordAgentSupervisionFailure(
        instance, policy, AgentSupervisionFailureKind::action_failed, 1000,
        "fault_injection:actuator_failure");
  }
  if (descriptor.scenario_key == "lost_lease") {
    instance->last_run_start_microseconds = 500;
    instance->lease_until_microseconds = 900;
    return EvaluateAgentSupervisionTick(instance, policy, 1000);
  }
  if (descriptor.scenario_key == "action_retry_cooldown") {
    AgentSupervisionDecision decision;
    instance->state = AgentLifecycleState::failed;
    instance->cooldown_until_microseconds = 3000;
    decision.status =
        AgentError("SB_AGENT_ACTION.RETRY_COOLDOWN_ACTIVE",
                   instance->instance_uuid);
    decision.state = instance->state;
    decision.cooldown_until_microseconds = instance->cooldown_until_microseconds;
    decision.restart_allowed = false;
    return decision;
  }
  if (descriptor.scenario_key == "crash_loop") {
    AgentSupervisionDecision decision;
    instance->crash_loop_count = 3;
    const auto isolated = ValidateExecutionIsolation(*instance, false, false);
    (void)QuarantineAgentInstance(instance, 1000,
                                  "fault_injection:crash_loop");
    decision.status = isolated;
    decision.state = instance->state;
    decision.lease_cleared = instance->lease_until_microseconds == 0;
    decision.quarantined = instance->quarantined;
    return decision;
  }
  if (descriptor.scenario_key == "queue_corruption") {
    AgentSupervisionDecision decision;
    const auto quarantine = QuarantineAgentInstance(
        instance, 1000, "fault_injection:queue_corruption");
    decision.status =
        AgentError(descriptor.diagnostic_code, descriptor.scenario_key);
    decision.state = instance->state;
    decision.failure_count = instance->supervision_failure_count;
    decision.restart_attempts = instance->restart_attempts;
    decision.lease_cleared = instance->lease_until_microseconds == 0;
    decision.quarantined = instance->quarantined;
    (void)quarantine;
    return decision;
  }
  AgentSupervisionDecision decision;
  decision.status = AgentError(descriptor.diagnostic_code,
                               descriptor.scenario_key);
  decision.state = instance->state;
  return decision;
}

}  // namespace

std::vector<AgentFaultInjectionScenarioDescriptor>
AgentFaultInjectionScenarioDescriptors() {
  using Recovery = AgentFaultInjectionRecoveryResponse;
  return {
      FaultScenario("watchdog_timeout", AgentFaultInjectionClass::supervision,
                    "SB_AGENT_ISOLATION.WATCHDOG_EXPIRED",
                    "supervision_failure", Recovery::supervision_restart_backoff,
                    true),
      FaultScenario("runtime_timeout", AgentFaultInjectionClass::supervision,
                    "SB_AGENT_SUPERVISION.RUNTIME_TIMEOUT",
                    "supervision_failure", Recovery::supervision_restart_backoff,
                    true),
      FaultScenario("disk_full", AgentFaultInjectionClass::storage_io,
                    "SB_AGENT_FAULT.DISK_FULL_FAIL_CLOSED",
                    "storage_actuator_failure", Recovery::fail_closed),
      FaultScenario("fsync_failure", AgentFaultInjectionClass::storage_io,
                    "SB_AGENT_FAULT.FSYNC_FAILURE_FAIL_CLOSED",
                    "durability_fence_failure", Recovery::fail_closed),
      FaultScenario("permission_denied", AgentFaultInjectionClass::storage_io,
                    "SB_AGENT_FAULT.PERMISSION_DENIED_FAIL_CLOSED",
                    "storage_permission_failure", Recovery::fail_closed),
      FaultScenario("corrupt_metric", AgentFaultInjectionClass::metric_input,
                    "SB_AGENT_FAULT.CORRUPT_METRIC_FAIL_CLOSED",
                    "metric_sample_rejected", Recovery::reject_metric_sample),
      FaultScenario("invalid_policy", AgentFaultInjectionClass::policy_input,
                    "SB_AGENT_FAULT.INVALID_POLICY_FAIL_CLOSED",
                    "policy_rejected", Recovery::reject_policy),
      FaultScenario("queue_corruption", AgentFaultInjectionClass::queue_integrity,
                    "SB_AGENT_FAULT.QUEUE_CORRUPTION_QUARANTINE",
                    "queue_integrity_failure",
                    Recovery::supervision_quarantine, true),
      FaultScenario("partial_filespace_growth",
                    AgentFaultInjectionClass::partial_action,
                    "SB_AGENT_FAULT.PARTIAL_FILESPACE_GROWTH_FAIL_CLOSED",
                    "recovery_required", Recovery::fail_closed, false, true),
      FaultScenario("partial_page_preallocation",
                    AgentFaultInjectionClass::partial_action,
                    "SB_AGENT_FAULT.PARTIAL_PAGE_PREALLOCATION_FAIL_CLOSED",
                    "recovery_required", Recovery::fail_closed, false, true),
      FaultScenario("restart_mid_action", AgentFaultInjectionClass::supervision,
                    "SB_AGENT_ACTION.FAILED_BACKOFF",
                    "supervision_failure", Recovery::supervision_restart_backoff,
                    true),
      FaultScenario("stale_metric", AgentFaultInjectionClass::metric_input,
                    "SB_AGENT_FAULT.CORRUPT_METRIC_FAIL_CLOSED",
                    "metric_sample_rejected", Recovery::reject_metric_sample),
      FaultScenario("actuator_failure", AgentFaultInjectionClass::partial_action,
                    "SB_AGENT_ACTION.FAILED_BACKOFF",
                    "supervision_failure", Recovery::supervision_restart_backoff,
                    true),
      FaultScenario("lost_lease", AgentFaultInjectionClass::supervision,
                    "SB_AGENT_SUPERVISION.TICK_TIMEOUT",
                    "supervision_failure", Recovery::supervision_restart_backoff,
                    true),
      FaultScenario("duplicate_run", AgentFaultInjectionClass::supervision,
                    "SB_AGENT_SCHEDULER.LEASE_HELD",
                    "scheduler_refusal", Recovery::fail_closed),
      FaultScenario("corrupted_policy", AgentFaultInjectionClass::policy_input,
                    "SB_AGENT_FAULT.INVALID_POLICY_FAIL_CLOSED",
                    "policy_rejected", Recovery::reject_policy),
      FaultScenario("cluster_authority_absent",
                    AgentFaultInjectionClass::policy_input,
                    "SB_AGENT_TIME.CLUSTER_MAJORITY_TIME_REQUIRED",
                    "cluster_authority_refusal", Recovery::fail_closed),
      FaultScenario("action_retry_cooldown",
                    AgentFaultInjectionClass::supervision,
                    "SB_AGENT_ACTION.RETRY_COOLDOWN_ACTIVE",
                    "supervision_failure", Recovery::supervision_restart_backoff,
                    true),
      FaultScenario("crash_loop", AgentFaultInjectionClass::supervision,
                    "SB_AGENT_ISOLATION.CRASH_LOOP_QUARANTINE",
                    "supervision_failure", Recovery::supervision_quarantine,
                    true)};
}

std::optional<AgentFaultInjectionScenarioDescriptor>
FindAgentFaultInjectionScenarioDescriptor(const std::string& scenario) {
  for (const auto& descriptor : AgentFaultInjectionScenarioDescriptors()) {
    if (descriptor.scenario_key == scenario) { return descriptor; }
  }
  return std::nullopt;
}

std::vector<std::string> AgentFaultInjectionScenarios() {
  std::vector<std::string> scenarios;
  for (const auto& descriptor : AgentFaultInjectionScenarioDescriptors()) {
    scenarios.push_back(descriptor.scenario_key);
  }
  return scenarios;
}

AgentFaultInjectionResult EvaluateAgentFaultInjectionScenarioDetailed(
    const std::string& scenario) {
  AgentFaultInjectionResult result;
  result.scenario_key = scenario;
  const auto descriptor = FindAgentFaultInjectionScenarioDescriptor(scenario);
  if (!descriptor.has_value()) {
    result.status = AgentError("SB_AGENT_FAULT.UNKNOWN_SCENARIO", scenario);
    result.diagnostic_code = result.status.diagnostic_code;
    result.evidence_kind = "fault_injection_unknown";
    result.evidence_uuid = DurableObjectUuidFromKey("agent_fault_evidence|unknown");
    result.evidence_detail = "unknown_scenario:" + scenario;
    return result;
  }

  result.fault_class = descriptor->fault_class;
  result.status = AgentError(descriptor->diagnostic_code,
                             descriptor->scenario_key);
  result.diagnostic_code = descriptor->diagnostic_code;
  result.evidence_kind = descriptor->evidence_kind;
  result.evidence_uuid = FaultEvidenceUuid(descriptor->scenario_key);
  result.evidence_detail = FaultEvidenceDetail(*descriptor);
  result.recovery_response = descriptor->recovery_response;
  result.uses_supervision = descriptor->uses_supervision;
  result.uses_arbitration = descriptor->uses_arbitration;

  if (descriptor->uses_supervision) {
    auto instance = FaultInjectionInstance(descriptor->scenario_key);
    result.supervision = EvaluateSupervisionFault(*descriptor, &instance);
    result.state_after = result.supervision.state;
  }

  if (descriptor->uses_arbitration) {
    const auto action = PartialFaultAction(*descriptor, result);
    result.arbitration =
        ArbitrateAgentActionsDetailed(FaultArbitrationContext(), {action});
  }

  return result;
}

AgentRuntimeStatus EvaluateFaultInjectionScenario(const std::string& scenario) {
  return EvaluateAgentFaultInjectionScenarioDetailed(scenario).status;
}

bool AgentPersistenceUsesScratchBirdStorageAuthority() { return true; }

AgentRuntimeStatus RecordAgentRuntimeMetric(const AgentTypeDescriptor& descriptor,
                                            const AgentActionDecision& decision,
                                            u64 decision_latency_microseconds) {
  const auto action = scratchbird::core::metrics::RecordAgentAction(
      descriptor.type_id,
      AgentActionResultClassName(decision.result_class),
      decision.diagnostic_code);
  if (!action.ok) { return AgentError(action.diagnostic_code, action.detail); }
  const auto latency = scratchbird::core::metrics::ObserveAgentDecisionLatency(
      static_cast<double>(decision_latency_microseconds),
      descriptor.type_id,
      AgentActionResultClassName(decision.result_class));
  if (!latency.ok) { return AgentError(latency.diagnostic_code, latency.detail); }
  return AgentOk();
}

}  // namespace scratchbird::core::agents
