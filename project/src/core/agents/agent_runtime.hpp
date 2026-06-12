// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_AGENT_RUNTIME_FRAMEWORK
// Engine-owned operational agent runtime contract. Agents are not parser
// authority. Agent decisions are UUID/object/policy/metric based and must fail
// closed when authority, policy, metrics, feature gates, or actuators are not
// proven available.

#include "metric_registry.hpp"
#include "runtime_platform.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

using scratchbird::core::platform::u64;

struct AgentRuntimeStatus {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
};

AgentRuntimeStatus AgentOk();
AgentRuntimeStatus AgentError(std::string code, std::string detail = {});

// SEARCH_KEY: PFAR_016AA_AGENT_RUNTIME_UUID_AUTHORITY
// Deterministic runtime-owned UUID text helpers. These return typed durable
// engine UUID strings for UUID fields; labels remain in non-UUID metadata.
std::string DeterministicAgentRuntimeObjectUuidFromKey(const std::string& key);
std::string DeterministicAgentRuntimePrincipalUuidFromKey(const std::string& key);
std::string DeterministicAgentRuntimeDatabaseUuidFromKey(const std::string& key);

// SEARCH_KEY: SB_AGENT_RUNTIME_CANONICAL_REGISTRY
enum class AgentDeployment {
  local,
  both,
  cluster
};

enum class AgentAuthorityClass {
  observe_only,
  recommend_only,
  request_action,
  direct_bounded_action
};

enum class AgentLifecycleState {
  created,
  registered,
  disabled,
  observe_only,
  recommend_only,
  dry_run,
  running,
  paused,
  safe_mode,
  quarantined,
  stopping,
  stopped,
  retired,
  failed
};

enum class AgentActivationProfile {
  disabled,
  observe_only,
  recommend_only,
  dry_run,
  live_action
};

enum class AgentActionClass {
  none,
  recommendation,
  request_action,
  direct_bounded_action,
  dry_run,
  refusal,
  override_action,
  manual_approval_required
};

enum class AgentActionResultClass {
  accepted,
  refused,
  suppressed,
  dry_run_only,
  approval_required,
  failed_closed,
  quarantined
};

enum class AgentLifecycleMode {
  normal,
  database_create,
  database_open,
  database_close,
  backup,
  restore,
  shutdown,
  crash_recovery,
  restricted_open,
  read_only,
  maintenance,
  repair,
  archive_hold,
  pitr,
  clone,
  role_change
};

enum class AgentTimeAuthorityMode {
  single_node_os_time,
  cluster_majority_time
};

enum class AgentFeatureAvailability {
  available,
  unavailable_disabled_stub,
  unavailable_edition,
  unavailable_cluster_authority,
  unavailable_private_feature
};

enum class AgentMetricDependencyKind {
  required,
  optional,
  required_for_cluster_scope,
  required_for_shrink,
  required_for_relocation
};

enum class AgentMetricSourceQuality {
  unknown,
  trusted,
  cluster_confirmed
};

struct AgentMetricDependency {
  std::string metric_family;
  std::string namespace_prefix;
  bool required = true;
  bool cluster_only = false;
  AgentMetricDependencyKind dependency_kind = AgentMetricDependencyKind::required;
  u64 max_freshness_microseconds = 0;
  AgentMetricSourceQuality required_source_quality = AgentMetricSourceQuality::trusted;
  std::string required_quality;
  std::string aggregation;
  std::string policy_field;
  std::string decision_use;
  std::string fail_behavior;
  std::string evidence_field;
};

struct AgentMetricDependencyContractRow {
  std::string agent_type_id;
  AgentMetricDependency dependency;
};

struct AgentMetricObservation {
  std::string metric_family;
  std::string namespace_path;
  u64 age_microseconds = 0;
  bool present = true;
  bool trusted = true;
  AgentMetricSourceQuality source_quality = AgentMetricSourceQuality::trusted;
  bool schema_compatible = true;
  bool scope_compatible = true;
  std::string evidence_uuid;
  std::string snapshot_id;
};

struct AgentPolicyDependencyState {
  std::string policy_family;
  std::string policy_uuid;
  std::string scope;
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  std::string evidence_uuid;
};

struct AgentDependencyDiagnostic {
  std::string diagnostic_code;
  std::string subject_kind;
  std::string subject;
  std::string evidence_uuid;
  std::string snapshot_id;
  std::string namespace_prefix;
  std::string policy_field;
  std::string decision_use;
  std::string fail_behavior;
  std::string dependency_evidence_field;
  bool optional_suppressed = false;
  bool failed_closed = true;
};

struct AgentDependencyEvaluation {
  AgentRuntimeStatus status;
  std::vector<AgentDependencyDiagnostic> diagnostics;
  bool failed_closed = false;
  bool cluster_path_failed_closed = false;
  bool local_projection_valid = false;
  bool optional_suppressed = false;
};

struct AgentTypeDescriptor {
  std::string type_id;
  AgentDeployment deployment = AgentDeployment::local;
  std::string scope;
  AgentAuthorityClass authority = AgentAuthorityClass::observe_only;
  AgentActivationProfile default_activation = AgentActivationProfile::observe_only;
  bool cluster_only = false;
  std::vector<std::string> required_rights;
  std::vector<AgentMetricDependency> metric_dependencies;
};

struct AgentRuntimeContext {
  bool security_context_present = false;
  bool cluster_authority_available = false;
  bool cluster_time_majority_available = false;
  bool private_features_available = true;
  bool standalone_edition = true;
  bool shutdown_requested = false;
  bool read_only_mode = false;
  bool maintenance_mode = false;
  bool restricted_open_mode = false;
  bool repair_mode = false;
  bool backup_hold_mode = false;
  bool archive_hold_mode = false;
  std::string principal_uuid;
  std::string database_uuid;
  std::string cluster_uuid;
  std::vector<std::string> rights;
  std::vector<std::string> groups;
  std::vector<std::string> trace_tags;
  u64 monotonic_now_microseconds = 0;
  u64 wall_now_microseconds = 0;
  u64 cluster_now_microseconds = 0;
  u64 cluster_time_uncertainty_microseconds = 0;
};

struct AgentPolicy {
  std::string policy_uuid;
  std::string policy_name;
  std::string policy_family;
  std::string scope;
  std::string action_mode;
  std::string invalid_policy_behavior;
  AgentActivationProfile activation = AgentActivationProfile::observe_only;
  bool enabled = true;
  bool allow_live_action = false;
  bool require_manual_approval = true;
  bool require_dry_run_before_live = true;
  bool evidence_required = true;
  bool explainability_required = true;
  u64 run_interval_microseconds = 60000000;
  u64 jitter_microseconds = 5000000;
  u64 lease_microseconds = 120000000;
  u64 cooldown_microseconds = 300000000;
  u64 max_runtime_microseconds = 5000000;
  u64 max_restart_attempts = 3;
  u64 initial_backoff_microseconds = 1000000;
  u64 max_backoff_microseconds = 60000000;
  u64 max_history_query_rows = 1024;
  u64 max_evidence_fanout = 128;
  u64 max_label_cardinality = 256;
  u64 action_budget_per_window = 1;
  u64 policy_generation = 1;
  std::vector<std::string> required_metric_families;
  std::vector<std::string> policy_dependencies;
  std::map<std::string, std::string> config_fields;
};

struct AgentPolicyBootstrapRecord {
  std::string agent_type_id;
  std::string policy_family;
  std::string policy_uuid;
  std::string scope;
  std::string action_mode;
  std::string invalid_policy_behavior;
  bool enabled = true;
  AgentActivationProfile activation = AgentActivationProfile::observe_only;
  u64 policy_generation = 1;
  u64 run_interval_microseconds = 60000000;
  u64 cooldown_microseconds = 300000000;
  std::vector<std::string> required_fields;
  std::map<std::string, std::string> config_fields;
};

struct AgentPolicyAttachmentRecord {
  std::string attachment_uuid;
  std::string agent_type_id;
  std::string policy_family;
  std::string policy_uuid;
  std::string scope;
  u64 policy_generation = 1;
  u64 attachment_generation = 1;
  bool baseline = true;
  bool active = true;
  bool valid = true;
  std::string diagnostic_code;
  std::string evidence_uuid;
};

// SEARCH_KEY: SB_AGENT_STORAGE_SPACE_POLICY_DEFAULTS
// Default local storage cooperation policy between filespace_capacity_manager and
// page_allocation_manager. The filespace manager keeps a small physical reserve
// available, while the page allocation manager notifies it once released/free
// pages fall to half of the normal target reserve.
struct StorageSpaceAgentDefaults {
  u64 filespace_min_available_pages = 4;
  u64 filespace_target_available_pages = 8;
  u64 filespace_page_allocation_notify_pages = 4;
  u64 page_allocation_notify_released_free_pages = 4;
};

struct AgentInstanceRecord {
  std::string instance_uuid;
  std::string agent_type_id;
  std::string policy_uuid;
  std::string scope;
  AgentLifecycleState state = AgentLifecycleState::created;
  u64 run_generation = 0;
  u64 policy_generation = 0;
  u64 instance_generation = 0;
  u64 retired_generation = 0;
  u64 lease_until_microseconds = 0;
  u64 last_run_start_microseconds = 0;
  u64 last_run_end_microseconds = 0;
  u64 crash_loop_count = 0;
  u64 supervision_failure_count = 0;
  u64 restart_attempts = 0;
  u64 restart_not_before_microseconds = 0;
  u64 cooldown_until_microseconds = 0;
  bool disabled_by_operator = false;
  bool safe_mode = false;
  bool quarantined = false;
  bool cancellation_requested = false;
  std::string retirement_evidence_uuid;
  std::string last_failure_diagnostic_code;
  std::string last_supervision_detail;
};

struct AgentEvidenceRecord {
  std::string evidence_uuid;
  std::string agent_type_id;
  std::string instance_uuid;
  std::string evidence_kind;
  std::string diagnostic_code;
  std::string detail;
  std::string input_metric_digest;
  u64 policy_generation = 0;
  std::string principal_uuid;
  std::vector<std::string> rights_used;
  std::vector<std::string> scope_uuids;
  std::string decision_payload_digest;
  std::string result_state;
  std::string redaction_class = "standard";
  std::string retention_class = "operational";
  std::string outcome_verification_evidence_uuid;
  std::string tamper_digest_algorithm = "sha256-chain-v1";
  std::string tamper_digest;
  std::string previous_tamper_digest;
  std::string tamper_chain_digest;
  std::string tamper_signature_algorithm = "hmac-sha256-v1";
  std::string tamper_signature;
  std::string tamper_key_id;
  std::string tamper_key_provenance;
  u64 tamper_key_generation = 0;
  std::string evidence_key_policy_id;
  u64 tamper_key_rotation_epoch = 0;
  u64 tamper_key_not_before_microseconds = 0;
  u64 tamper_key_not_after_microseconds = 0;
  std::string key_residency_class;
  std::string data_residency_class;
  std::string storage_linkage_digest;
  u64 tamper_evidence_generation = 0;
  u64 created_at_microseconds = 0;
  u64 expires_at_microseconds = 0;
  bool protected_material_suppressed = false;
  bool redaction_applied_before_buffering = false;
  bool legal_hold_active = false;
  bool production_key_material = false;
  bool test_key_material = false;
  bool key_material_exported = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool reference_authority = false;
  bool sidecar_authority = false;
  bool transaction_authority = false;
  bool finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
  bool security_authority = false;
};

struct AgentActionRequest {
  std::string action_uuid;
  std::string agent_type_id;
  std::string instance_uuid;
  AgentActionClass action_class = AgentActionClass::none;
  std::string actuator_id;
  std::string operation_id;
  std::string idempotency_key;
  bool dry_run = true;
  bool operator_override = false;
  bool manual_approval_present = false;
  std::map<std::string, std::string> inputs;
};

struct AgentActionDecision {
  AgentActionResultClass result_class = AgentActionResultClass::failed_closed;
  std::string diagnostic_code;
  std::string detail;
  std::string evidence_uuid;
  bool mutates_state = false;
};

// SEARCH_KEY: SB_AGENT_SECURITY_GRANT_MATRIX
enum class AgentSecurityRight {
  obs_agent_state_read,
  obs_agent_evidence_read,
  obs_agent_recommendation_read,
  obs_agent_control,
  obs_agent_action_approve,
  obs_agent_action_cancel,
  obs_agent_override,
  obs_support_bundle_read,
  obs_policy_read,
  obs_policy_simulate,
  obs_policy_edit_draft,
  obs_policy_validate,
  obs_policy_approve,
  obs_policy_apply,
  obs_policy_rollback,
  obs_policy_delete,
  obs_cluster_health_inspect,
  obs_cluster_topology_inspect,
  obs_cluster_control,
  sec_auth_metrics_read,
  sec_redaction_policy_edit,
  sec_export_policy_approve,
  sec_identity_admin,
  internal_agent_trace,
  external_named_right
};

enum class AgentSecurityCommandFamily {
  state_read,
  metrics_read,
  evidence_read,
  recommendation_read,
  control,
  action_approve,
  action_cancel,
  override,
  support_bundle_read,
  policy_read,
  policy_simulate,
  policy_validate,
  policy_apply,
  policy_rollback,
  policy_delete,
  cluster_inspect,
  cluster_control,
  auth_session_metrics_read,
  redaction_policy_edit,
  export_policy_approve
};

enum class AgentSecurityDenialKind {
  none,
  missing_security_context,
  missing_right,
  hidden_or_restricted_scope,
  redacted_payload,
  action_permission_denied,
  cluster_authority_required
};

struct AgentSecurityGrantRequirement {
  AgentSecurityRight right = AgentSecurityRight::obs_agent_state_read;
  std::string right_name;
  bool internal_trace_allowed = false;
  int alternative_group = 0;
};

struct AgentSecurityGrantDecision {
  bool allowed = false;
  AgentSecurityDenialKind denial = AgentSecurityDenialKind::missing_right;
  std::string diagnostic_code;
  std::string detail;
  std::string evidence_uuid;
  std::vector<AgentSecurityGrantRequirement> required_rights;
  std::string missing_right;
  bool hides_scope = false;
  bool hides_candidate_rows = false;
  bool payload_redacted = false;
  std::vector<std::string> redaction_evidence;
};

struct AgentEvidenceRedactionDecision {
  bool visible = false;
  bool redacted = false;
  AgentSecurityGrantDecision grant;
  AgentEvidenceRecord evidence;
};

struct AgentPolicyInspectionRecord {
  std::string policy_uuid;
  std::string policy_family;
  std::string policy_version;
  std::string attachment_uuid;
  std::string validation_state;
  std::string policy_body;
  bool restricted_scope = false;
};

struct AgentPolicyRedactionDecision {
  bool visible = false;
  bool redacted = false;
  AgentSecurityGrantDecision grant;
  AgentPolicyInspectionRecord policy;
};

struct AgentMetricInspectionRecord {
  std::string metric_family;
  std::string namespace_path;
  std::string raw_value;
  std::string redacted_value;
  bool restricted_scope = false;
  bool security_sensitive = false;
};

struct AgentMetricRedactionDecision {
  bool visible = false;
  bool redacted = false;
  AgentSecurityGrantDecision grant;
  AgentMetricInspectionRecord metric;
};

struct AgentActionInspectionRecord {
  std::string action_uuid;
  std::string action_id;
  std::string owning_agent;
  std::string actor_principal_uuid;
  std::string detail;
  bool restricted_scope = false;
  bool security_sensitive = false;
};

struct AgentActionRedactionDecision {
  bool visible = false;
  bool redacted = false;
  AgentSecurityGrantDecision grant;
  AgentActionInspectionRecord action;
};

// SEARCH_KEY: SB_AGENT_RESOURCE_BUDGET_BACKPRESSURE
enum class AgentResourceBudgetDecisionKind {
  allow,
  throttle_defer,
  shed_refuse,
  fail_closed,
  cancel_drain,
  foreground_protection
};

enum class AgentResourceBudgetDimension {
  foreground_protection,
  cpu_time,
  memory_bytes,
  io_bytes,
  io_ops,
  thread_slots,
  queue_depth,
  cadence,
  retry_backoff,
  runtime_timeout,
  cancellation_drain,
  history_rows,
  evidence_fanout,
  label_cardinality
};

struct AgentResourceBudget {
  bool protect_foreground_work = true;
  u64 max_cpu_time_microseconds = 0;
  u64 max_memory_bytes = 0;
  u64 max_io_bytes = 0;
  u64 max_io_ops = 0;
  u64 max_thread_slots = 1;
  u64 max_queue_depth = 1;
  u64 min_run_interval_microseconds = 0;
  u64 retry_backoff_microseconds = 0;
  u64 watchdog_timeout_microseconds = 0;
  u64 max_history_query_rows = 0;
  u64 max_evidence_fanout = 0;
  u64 max_label_cardinality = 0;
};

struct AgentResourceUsage {
  u64 cpu_time_microseconds = 0;
  u64 memory_bytes = 0;
  u64 io_bytes = 0;
  u64 io_ops = 0;
  u64 thread_slots = 0;
  u64 queue_depth = 0;
  u64 last_run_start_microseconds = 0;
  u64 last_run_end_microseconds = 0;
  u64 last_failure_microseconds = 0;
  u64 runtime_microseconds = 0;
  u64 history_query_rows = 0;
  u64 evidence_fanout = 0;
  u64 label_cardinality = 0;
};

struct AgentResourceBudgetEvaluationInput {
  AgentResourceBudget budget;
  AgentResourceUsage usage;
  bool foreground_database_work_active = false;
  bool cancellation_requested = false;
  bool drain_requested = false;
};

struct AgentResourceBudgetDiagnostic {
  AgentResourceBudgetDecisionKind decision =
      AgentResourceBudgetDecisionKind::allow;
  AgentResourceBudgetDimension dimension =
      AgentResourceBudgetDimension::foreground_protection;
  std::string diagnostic_code;
  std::string detail;
  std::string evidence_uuid;
  bool suppresses_mutation = false;
  bool protects_foreground = false;
};

struct AgentResourceBudgetDecision {
  AgentRuntimeStatus status;
  AgentResourceBudgetDecisionKind decision =
      AgentResourceBudgetDecisionKind::allow;
  std::vector<AgentResourceBudgetDiagnostic> diagnostics;
  std::string evidence_uuid;
  bool action_allowed = true;
  bool mutation_allowed = true;
  bool health_publish_allowed = true;
  bool failed_closed = false;
};

// SEARCH_KEY: SB_AGENT_WORKER_CAPACITY_EVIDENCE
// Deterministic database-local agent worker capacity snapshot. This is an
// auditable planning surface only: it does not own parser admission,
// transaction finality, catalog truth, storage identity, or security authority.
struct AgentWorkerCapacityConfig {
  u64 observed_cpu_count = 0;
  u64 configured_cpu_count = 0;
  u64 foreground_reserved_capacity = 1;
  u64 max_background_worker_slots = 0;
  bool foreground_database_work_active = false;
  bool standalone_edition = true;
  bool cluster_authority_available = false;
};

struct AgentWorkerCapacityCandidate {
  std::string agent_type_id;
  AgentPolicy policy;
  AgentResourceUsage usage;
  u64 requested_worker_slots = 1;
  bool dml_prework_agent = false;
  bool may_precede_foreground_demand = true;
};

struct AgentWorkerCapacityAssignment {
  std::string agent_type_id;
  bool selected = false;
  bool assigned = false;
  bool dml_prework_agent = false;
  bool can_run_before_foreground_demand = false;
  bool cluster_path_failed_closed = false;
  u64 requested_worker_slots = 0;
  u64 worker_slot_index = 0;
  AgentResourceBudgetDecisionKind resource_decision =
      AgentResourceBudgetDecisionKind::fail_closed;
  AgentResourceBudgetDimension resource_dimension =
      AgentResourceBudgetDimension::foreground_protection;
  std::string diagnostic_code;
  std::string detail;
  std::string evidence_uuid;
};

struct AgentWorkerCapacitySnapshot {
  AgentRuntimeStatus status;
  u64 observed_cpu_count = 0;
  u64 configured_cpu_count = 0;
  u64 effective_cpu_count = 0;
  u64 foreground_reserved_capacity = 0;
  u64 background_worker_slots = 0;
  bool foreground_capacity_reserved = false;
  bool background_capacity_available = false;
  bool foreground_work_active = false;
  bool resource_bounds_blocked_work = false;
  bool cluster_paths_failed_closed = false;
  std::vector<AgentWorkerCapacityAssignment> assignments;
  std::vector<std::string> diagnostics;
};

struct AgentActionContractDescriptor {
  std::string action_id;
  std::string owning_agent;
  std::string actuator;
  std::string risk_class;
  std::string sync_async;
  std::string permission;
  std::string policy_gate;
  std::string metric_precondition_text;
  std::vector<std::string> metric_families;
  std::string evidence_kind;
  std::string retry_cooldown;
  std::string failure_behavior;
  AgentActionClass action_class = AgentActionClass::none;
  AgentActionResultClass default_result_class = AgentActionResultClass::failed_closed;
  bool manual_approval_required = false;
  bool operator_approval_required = false;
  bool cluster_scoped = false;
};

struct AgentActionContractEvaluationRequest {
  AgentRuntimeContext context;
  const AgentPolicy* policy = nullptr;
  bool policy_present = true;
  bool policy_gate_present = true;
  bool evidence_store_available = true;
  bool live_prerequisites_enabled = false;
  bool actuator_route_available = false;
  std::string actuator_route_id;
  bool suppressed = false;
  bool arbitration_passed = true;
  bool arbitration_evidence_present = true;
  std::optional<AgentResourceBudgetEvaluationInput> resource_budget;
  bool enforce_metric_observation_dependencies = false;
  bool enforce_policy_dependency_state = false;
  std::vector<std::string> available_metric_families;
  std::vector<AgentMetricObservation> metric_observations;
  std::vector<AgentPolicyDependencyState> policy_dependency_states;
};

enum class AgentArbitrationActionClass {
  protect_correctness,
  protect_security,
  protect_durability,
  protect_availability,
  reduce_pressure,
  optimize_performance,
  reduce_cost
};

enum class AgentArbitrationRisk {
  low,
  medium,
  high,
  critical
};

enum class AgentArbitrationReversibility {
  reversible,
  bounded_reversible,
  irreversible
};

enum class AgentArbitrationOutcome {
  winner_executes,
  both_denied,
  operator_review_required,
  suppressed_by_override
};

enum class AgentArbitrationPriorityRule {
  no_actions,
  safety_precondition_failed,
  single_action,
  override_suppression,
  override_right_required,
  override_authority_forbidden,
  action_class_priority,
  evidence_quality,
  exact_tie_operator_review
};

struct AgentArbitrationCandidate {
  std::string action_uuid;
  std::string agent_type_id;
  std::string instance_uuid;
  std::string policy_uuid;
  std::string scope_uuid;
  std::string actuator_id;
  std::string operation_id;
  AgentArbitrationActionClass action_class = AgentArbitrationActionClass::reduce_pressure;
  AgentArbitrationRisk risk = AgentArbitrationRisk::medium;
  AgentArbitrationReversibility reversibility = AgentArbitrationReversibility::reversible;
  u64 evidence_quality = 0;
  std::string evidence_uuid;
  bool dry_run = true;
  bool safety_preconditions_passed = true;
  std::string safety_diagnostic_code;
};

struct AgentArbitrationOverride {
  std::string override_uuid;
  std::string scope_uuid;
  std::string created_by;
  std::string reason_code;
  std::vector<std::string> suppressed_action_uuids;
  std::vector<std::string> allowed_action_uuids;
  u64 expires_at_microseconds = 0;
  std::string renewal_rule;
  std::string rollback_rule;
  std::string evidence_uuid;
  bool active = true;
};

struct AgentArbitrationRecord {
  AgentArbitrationOutcome outcome = AgentArbitrationOutcome::both_denied;
  AgentArbitrationPriorityRule priority_rule = AgentArbitrationPriorityRule::no_actions;
  std::vector<AgentArbitrationCandidate> normalized_actions;
  std::vector<std::string> losing_action_uuids;
  std::string winning_action_uuid;
  std::string policy_uuid;
  std::string override_uuid;
  std::string evidence_uuid;
  std::string diagnostic_code;
  std::string detail;
  bool operator_review_action_created = false;
  std::string operator_review_action_uuid;
};

struct AgentActuatorCapability {
  std::string actuator_id;
  bool registered = false;
  bool degraded = false;
  bool cluster_only = false;
  std::vector<std::string> operations;
  std::vector<std::string> required_rights;
};

// SEARCH_KEY: SB_AGENT_ACTION_ACTUATOR_AUTHORITY
enum class AgentActuatorAuthorityDomain {
  metrics,
  storage,
  filespace_lifecycle,
  page,
  memory,
  optimizer,
  admission,
  parser,
  transaction,
  cleanup,
  policy,
  support,
  job,
  backup,
  archive,
  restore,
  pitr,
  security,
  session,
  alert,
  export_subsystem,
  cluster_provider,
  operator_control,
  evidence,
  unknown
};

struct AgentActuatorAuthorityDescriptor {
  std::string action_id;
  std::string owning_agent;
  std::string actuator_id;
  AgentActuatorAuthorityDomain domain = AgentActuatorAuthorityDomain::unknown;
  bool cluster_scoped = false;
  bool route_registered = false;
  bool owns_forbidden_engine_authority = false;
};

struct AgentActuatorAuthorityDecision {
  AgentRuntimeStatus status;
  std::string expected_actuator_id;
  std::string requested_actuator_id;
  AgentActuatorAuthorityDomain domain = AgentActuatorAuthorityDomain::unknown;
  bool cluster_scoped = false;
  bool route_registered = false;
};

struct AgentTimeAuthorityDecision {
  AgentTimeAuthorityMode mode = AgentTimeAuthorityMode::single_node_os_time;
  AgentRuntimeStatus status;
  u64 monotonic_reference_microseconds = 0;
  u64 wall_reference_microseconds = 0;
  u64 cluster_reference_microseconds = 0;
  std::string clock_quality;
};

struct AgentRunDecision {
  AgentRuntimeStatus status;
  AgentActionDecision action;
  std::vector<AgentEvidenceRecord> evidence;
  std::vector<std::string> explanation_lines;
};

enum class AgentTickHealthClass {
  selected_running,
  observe_only,
  recommend_only,
  dry_run,
  policy_disabled,
  suppressed,
  manual_approval_operator_only,
  failed_closed
};

struct AgentTickHealthRequest {
  AgentRuntimeContext context;
  u64 policy_generation = 1;
  bool use_explicit_policy_state = false;
  bool enforce_metric_observation_dependencies = false;
  bool enforce_policy_dependency_state = false;
  std::optional<AgentResourceBudgetEvaluationInput> resource_budget;
  std::vector<AgentPolicy> policies;
  std::vector<AgentMetricObservation> metric_observations;
  std::vector<AgentPolicyDependencyState> policy_dependency_states;
  std::vector<std::string> suppressed_agent_type_ids;
  std::vector<std::string> missing_metric_families;
};

struct AgentTickHealthRecord {
  std::string agent_type_id;
  AgentDeployment deployment = AgentDeployment::local;
  std::string policy_uuid;
  AgentTickHealthClass tick_class = AgentTickHealthClass::failed_closed;
  AgentLifecycleState lifecycle_state = AgentLifecycleState::failed;
  AgentActionClass action_class = AgentActionClass::none;
  AgentActionResultClass action_result_class = AgentActionResultClass::failed_closed;
  bool selected = false;
  bool runnable = false;
  bool tick_produced = false;
  bool health_published = false;
  bool action_evidence_published = false;
  bool policy_disabled = false;
  bool suppressed = false;
  bool manual_approval_required = false;
  bool failed_closed = true;
  bool cluster_path_failed_closed = false;
  bool resource_budget_limited = false;
  std::string diagnostic_code;
  std::string detail;
  std::string health_evidence_uuid;
  std::string action_evidence_uuid;
  std::vector<AgentDependencyDiagnostic> dependency_diagnostics;
  std::vector<AgentResourceBudgetDiagnostic> resource_budget_diagnostics;
};

struct AgentTickHealthResult {
  AgentRuntimeStatus status;
  std::vector<AgentTickHealthRecord> records;
};

enum class AgentSupervisionFailureKind {
  tick_timeout,
  watchdog_timeout,
  exception,
  runtime_timeout,
  action_failed
};

struct AgentSupervisionDecision {
  AgentRuntimeStatus status;
  AgentLifecycleState state = AgentLifecycleState::failed;
  u64 failure_count = 0;
  u64 restart_attempts = 0;
  u64 backoff_microseconds = 0;
  u64 restart_not_before_microseconds = 0;
  u64 cooldown_until_microseconds = 0;
  bool lease_cleared = false;
  bool quarantined = false;
  bool restart_allowed = false;
};

enum class AgentFaultInjectionClass {
  supervision,
  storage_io,
  metric_input,
  policy_input,
  queue_integrity,
  partial_action
};

enum class AgentFaultInjectionRecoveryResponse {
  fail_closed,
  reject_metric_sample,
  reject_policy,
  supervision_restart_backoff,
  supervision_quarantine
};

struct AgentFaultInjectionScenarioDescriptor {
  std::string scenario_key;
  AgentFaultInjectionClass fault_class = AgentFaultInjectionClass::supervision;
  std::string diagnostic_code;
  std::string evidence_kind;
  AgentFaultInjectionRecoveryResponse recovery_response =
      AgentFaultInjectionRecoveryResponse::fail_closed;
  bool uses_supervision = false;
  bool uses_arbitration = false;
};

struct AgentFaultInjectionResult {
  std::string scenario_key;
  AgentRuntimeStatus status;
  AgentFaultInjectionClass fault_class = AgentFaultInjectionClass::supervision;
  AgentActionResultClass result_class = AgentActionResultClass::failed_closed;
  AgentLifecycleState state_after = AgentLifecycleState::failed;
  std::string diagnostic_code;
  std::string evidence_kind;
  std::string evidence_uuid;
  std::string evidence_detail;
  AgentFaultInjectionRecoveryResponse recovery_response =
      AgentFaultInjectionRecoveryResponse::fail_closed;
  AgentSupervisionDecision supervision;
  AgentArbitrationRecord arbitration;
  bool uses_supervision = false;
  bool uses_arbitration = false;
  bool failed_closed = true;
  bool durable_state_changed = false;
  bool success_reported = false;
  bool evidence_recorded_before_success = true;
  bool unsafe_state_mutation = false;
};

const char* AgentDeploymentName(AgentDeployment value);
const char* AgentAuthorityClassName(AgentAuthorityClass value);
const char* AgentLifecycleStateName(AgentLifecycleState value);
const char* AgentActivationProfileName(AgentActivationProfile value);
const char* AgentActionClassName(AgentActionClass value);
const char* AgentActionResultClassName(AgentActionResultClass value);
const char* AgentLifecycleModeName(AgentLifecycleMode value);
const char* AgentFeatureAvailabilityName(AgentFeatureAvailability value);
const char* AgentTickHealthClassName(AgentTickHealthClass value);
const char* AgentSupervisionFailureKindName(AgentSupervisionFailureKind value);
const char* AgentArbitrationActionClassName(AgentArbitrationActionClass value);
const char* AgentArbitrationRiskName(AgentArbitrationRisk value);
const char* AgentArbitrationReversibilityName(AgentArbitrationReversibility value);
const char* AgentArbitrationOutcomeName(AgentArbitrationOutcome value);
const char* AgentArbitrationPriorityRuleName(AgentArbitrationPriorityRule value);
const char* AgentFaultInjectionClassName(AgentFaultInjectionClass value);
const char* AgentFaultInjectionRecoveryResponseName(
    AgentFaultInjectionRecoveryResponse value);

std::vector<AgentTypeDescriptor> CanonicalAgentRegistry();
std::optional<AgentTypeDescriptor> FindAgentType(const std::string& type_id);
AgentRuntimeStatus ValidateCanonicalAgentRegistry();
AgentRuntimeStatus ValidateAgentType(const AgentTypeDescriptor& descriptor);
std::vector<AgentMetricDependencyContractRow> AgentMetricDependencyContractRegistry();
std::vector<AgentMetricDependency> MetricDependenciesForAgent(const std::string& agent_type_id);
std::optional<AgentMetricDependency> FindAgentMetricDependencyContract(
    const std::string& agent_type_id,
    const std::string& metric_family);

// SEARCH_KEY: SB_AGENT_STORAGE_SPACE_POLICY_DEFAULTS
StorageSpaceAgentDefaults DefaultStorageSpaceAgentDefaults();
AgentRuntimeStatus ValidateStorageSpaceAgentDefaults(const StorageSpaceAgentDefaults& defaults);

// SEARCH_KEY: SB_AGENT_RUNTIME_FEATURE_GATES
AgentFeatureAvailability EvaluateAgentFeatureAvailability(const AgentTypeDescriptor& descriptor,
                                                          const AgentRuntimeContext& context);

// SEARCH_KEY: SB_AGENT_RUNTIME_INSTANCE_STORE
std::string DeterministicAgentInstanceUuid(const std::string& database_uuid,
                                           const std::string& agent_type_id,
                                           const std::string& scope,
                                           u64 policy_generation);
std::string SerializeAgentInstanceRecord(const AgentInstanceRecord& instance);
AgentRuntimeStatus RestoreAgentInstanceRecord(const std::string& encoded,
                                              AgentInstanceRecord* instance);

// SEARCH_KEY: SB_AGENT_CATALOG_SCHEMA_LAYOUT
std::vector<std::string> AgentCatalogRecordLayouts();

// SEARCH_KEY: SB_AGENT_RUNTIME_LIFECYCLE_STATE_MACHINE
bool AgentLifecycleTransitionAllowed(AgentLifecycleState from, AgentLifecycleState to);
AgentRuntimeStatus ValidateAgentLifecycleTransition(AgentLifecycleState from, AgentLifecycleState to);

// SEARCH_KEY: SB_AGENT_POLICY_BINDING
std::vector<std::string> RequiredPolicyFamiliesForAgent(const AgentTypeDescriptor& descriptor);
std::vector<std::string> RequiredPolicyConfigFieldsForFamily(const std::string& policy_family);
AgentPolicy BaselinePolicyForAgent(const AgentTypeDescriptor& descriptor);
AgentPolicy BaselinePolicyForAgentFamily(const AgentTypeDescriptor& descriptor,
                                         const std::string& policy_family,
                                         u64 policy_generation = 1);
std::vector<AgentPolicyBootstrapRecord> BaselinePolicyBootstrapRecordsForAgent(
    const AgentTypeDescriptor& descriptor,
    u64 policy_generation = 1);
std::vector<AgentPolicyBootstrapRecord> DatabaseApplicableBaselinePolicyBootstrapRecords(
    u64 policy_generation = 1);
AgentRuntimeStatus ValidateAgentPolicy(const AgentPolicy& policy,
                                       const AgentTypeDescriptor& descriptor);
AgentRuntimeStatus ValidateAgentPolicyAttachment(const AgentPolicy& policy,
                                                 const AgentPolicyAttachmentRecord& attachment,
                                                 const AgentTypeDescriptor& descriptor,
                                                 u64 expected_policy_generation);
AgentRuntimeStatus ValidateAgentPolicyStateForMutation(const AgentPolicy* policy,
                                                       const AgentPolicyAttachmentRecord* attachment,
                                                       const AgentTypeDescriptor& descriptor,
                                                       u64 expected_policy_generation);

// SEARCH_KEY: SB_AGENT_RUNTIME_IN_MEMORY_CATALOG
class InMemoryAgentRuntimeCatalog {
 public:
  void BootstrapDatabasePolicies(const std::string& database_uuid, u64 policy_generation);
  AgentRuntimeStatus SaveInstances(const std::vector<AgentInstanceRecord>& instances);
  AgentRuntimeStatus RetireInstance(const std::string& instance_uuid,
                                    std::string evidence_uuid,
                                    u64 retired_generation);

  const std::vector<AgentPolicy>& policies() const { return policies_; }
  const std::vector<AgentPolicyAttachmentRecord>& attachments() const { return attachments_; }
  const std::vector<AgentInstanceRecord>& instances() const { return instances_; }
  const std::vector<AgentEvidenceRecord>& evidence() const { return evidence_; }

 private:
  std::vector<AgentPolicy> policies_;
  std::vector<AgentPolicyAttachmentRecord> attachments_;
  std::vector<AgentInstanceRecord> instances_;
  std::vector<AgentEvidenceRecord> evidence_;
};

// SEARCH_KEY: SB_AGENT_POLICY_LINT_CONFORMANCE
std::vector<AgentRuntimeStatus> LintAgentPolicy(const AgentPolicy& policy,
                                                const AgentTypeDescriptor& descriptor,
                                                const scratchbird::core::metrics::MetricRegistry& registry = scratchbird::core::metrics::DefaultMetricRegistry());

// SEARCH_KEY: SB_AGENT_RUNTIME_SECURITY_RIGHTS
const char* AgentSecurityRightName(AgentSecurityRight value);
std::optional<AgentSecurityRight> ParseAgentSecurityRight(const std::string& right);
const char* AgentSecurityCommandFamilyName(AgentSecurityCommandFamily value);
const char* AgentSecurityDenialKindName(AgentSecurityDenialKind value);
std::vector<AgentSecurityGrantRequirement> RequiredAgentSecurityRightsForCommand(
    AgentSecurityCommandFamily command_family);
std::vector<AgentSecurityGrantRequirement> RequiredAgentSecurityRightsForActionContract(
    const AgentActionContractDescriptor& contract);
AgentSecurityGrantDecision EvaluateAgentSecurityGrant(
    const AgentRuntimeContext& context,
    const std::vector<AgentSecurityGrantRequirement>& requirements,
    std::string detail = {},
    bool action_permission = false,
    bool restricted_scope = false,
    bool cluster_scope = false);
AgentSecurityGrantDecision EvaluateAgentCommandGrant(
    const AgentRuntimeContext& context,
    AgentSecurityCommandFamily command_family,
    std::string detail = {},
    bool restricted_scope = false,
    bool cluster_scope = false);
AgentSecurityGrantDecision EvaluateAgentActionContractGrant(
    const AgentRuntimeContext& context,
    const AgentActionContractDescriptor& contract,
    bool restricted_scope = false);
bool AgentContextHasRight(const AgentRuntimeContext& context, const std::string& right);
AgentRuntimeStatus ValidateAgentSecurity(const AgentRuntimeContext& context,
                                         const AgentTypeDescriptor& descriptor,
                                         const std::string& operation_class);

// SEARCH_KEY: SB_AGENT_RUNTIME_TIME_AUTHORITY
AgentTimeAuthorityDecision ResolveAgentTimeAuthority(const AgentRuntimeContext& context,
                                                     bool cluster_scoped_action);

// SEARCH_KEY: SB_AGENT_RUNTIME_METRIC_DEPENDENCY_RESOLVER
AgentDependencyEvaluation EvaluateAgentMetricDependencies(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    const std::vector<AgentMetricObservation>& observations,
    const std::vector<AgentPolicyDependencyState>& policy_state = {},
    bool enforce_policy_state = false,
    const scratchbird::core::metrics::MetricRegistry& registry = scratchbird::core::metrics::DefaultMetricRegistry());
AgentRuntimeStatus ResolveAgentMetricDependencies(const AgentTypeDescriptor& descriptor,
                                                  const AgentRuntimeContext& context,
                                                  const scratchbird::core::metrics::MetricRegistry& registry = scratchbird::core::metrics::DefaultMetricRegistry());

// SEARCH_KEY: SB_AGENT_NONCLUSTER_TICK_HEALTH_COVERAGE
AgentTickHealthResult BuildNonClusterAgentTickHealthSnapshot(
    const AgentTickHealthRequest& request,
    const scratchbird::core::metrics::MetricRegistry& registry = scratchbird::core::metrics::DefaultMetricRegistry());

// SEARCH_KEY: SB_AGENT_RUNTIME_EVIDENCE_STORE
AgentEvidenceRecord MakeAgentEvidence(const AgentRuntimeContext& context,
                                      const AgentTypeDescriptor& descriptor,
                                      const AgentInstanceRecord& instance,
                                      std::string evidence_kind,
                                      std::string diagnostic_code,
                                      std::string detail);
AgentRuntimeStatus ValidateEvidenceVisibility(const AgentRuntimeContext& context,
                                              const AgentEvidenceRecord& evidence);

// SEARCH_KEY: SB_AGENT_RUNTIME_EVIDENCE_PRIVACY
AgentEvidenceRecord RedactAgentEvidence(const AgentEvidenceRecord& evidence,
                                        const AgentRuntimeContext& context);
AgentEvidenceRedactionDecision RedactAgentEvidenceForSecurity(
    const AgentEvidenceRecord& evidence,
    const AgentRuntimeContext& context,
    bool support_bundle_view = false,
    bool restricted_scope = false);
AgentPolicyRedactionDecision RedactAgentPolicyForSecurity(
    const AgentPolicyInspectionRecord& policy,
    const AgentRuntimeContext& context,
    bool restricted_scope = false);
AgentMetricRedactionDecision RedactAgentMetricForSecurity(
    const AgentMetricInspectionRecord& metric,
    const AgentRuntimeContext& context);
AgentActionRedactionDecision RedactAgentActionForSecurity(
    const AgentActionInspectionRecord& action,
    const AgentRuntimeContext& context);

// SEARCH_KEY: SB_AGENT_RUNTIME_ACTUATOR_CAPABILITY_REGISTRY
std::vector<AgentActuatorCapability> DefaultActuatorCapabilities();
std::optional<AgentActuatorCapability> FindActuatorCapability(const std::string& actuator_id);
AgentRuntimeStatus ValidateActuatorCapability(const AgentRuntimeContext& context,
                                              const AgentActionRequest& action);
// SEARCH_KEY: SB_AGENT_ACTION_ACTUATOR_AUTHORITY
const char* AgentActuatorAuthorityDomainName(AgentActuatorAuthorityDomain value);
AgentActuatorAuthorityDomain ActuatorAuthorityDomainForId(const std::string& actuator_id);
std::vector<AgentActuatorAuthorityDescriptor> AgentActuatorAuthorityRegistry();
std::optional<AgentActuatorAuthorityDescriptor> FindAgentActuatorAuthority(
    const std::string& owning_agent,
    const std::string& action_id);
AgentRuntimeStatus ValidateAgentActuatorAuthorityRegistry();
AgentActuatorAuthorityDecision EvaluateAgentActionActuatorAuthority(
    const AgentActionContractDescriptor& contract,
    const AgentActionContractEvaluationRequest& request);

// SEARCH_KEY: SB_AGENT_RUNTIME_ACTION_ENVELOPE
AgentActionDecision EvaluateAgentAction(const AgentRuntimeContext& context,
                                        const AgentTypeDescriptor& descriptor,
                                        const AgentPolicy& policy,
                                        const AgentActionRequest& action);

// SEARCH_KEY: SB_AGENT_ACTION_CONTRACT_MATRIX_RUNTIME
std::vector<AgentActionContractDescriptor> AgentActionContractRegistry();
std::optional<AgentActionContractDescriptor> FindAgentActionContract(
    const std::string& owning_agent,
    const std::string& action_id);
std::vector<std::string> CanonicalAgentAllowedActionIds(const std::string& agent_type_id);
AgentActionDecision EvaluateAgentActionContract(
    const AgentActionContractDescriptor& contract,
    const AgentActionContractEvaluationRequest& request);
AgentActionDecision EvaluateAgentActionContract(
    const std::string& owning_agent,
    const std::string& action_id,
    const AgentActionContractEvaluationRequest& request);

// SEARCH_KEY: SB_AGENT_RESOURCE_BUDGET_BACKPRESSURE
const char* AgentResourceBudgetDecisionKindName(AgentResourceBudgetDecisionKind value);
const char* AgentResourceBudgetDimensionName(AgentResourceBudgetDimension value);
AgentResourceBudget DefaultAgentResourceBudgetForPolicy(const AgentPolicy& policy);
AgentResourceBudgetDecision EvaluateAgentResourceBudget(
    const AgentTypeDescriptor& descriptor,
    const AgentPolicy& policy,
    const AgentRuntimeContext& context,
    const AgentResourceBudgetEvaluationInput& input);
std::vector<AgentWorkerCapacityCandidate> DefaultDmlPreworkAgentWorkerCandidates(
    u64 policy_generation = 1);
AgentWorkerCapacitySnapshot PlanAgentWorkerCapacity(
    const AgentWorkerCapacityConfig& config,
    const AgentRuntimeContext& context,
    const std::vector<AgentWorkerCapacityCandidate>& candidates);

// SEARCH_KEY: SB_AGENT_RUNTIME_ACTION_SAFETY_BUDGETS
AgentRuntimeStatus ValidateActionSafetyBudget(const AgentPolicy& policy,
                                              const AgentActionRequest& action,
                                              u64 actions_used_in_window);

// SEARCH_KEY: SB_AGENT_RUNTIME_ACTION_OUTCOME_VERIFICATION
AgentActionDecision VerifyActionOutcome(const AgentActionRequest& action,
                                        bool subsystem_reported_success,
                                        bool intended_state_observed);

// SEARCH_KEY: SB_AGENT_RUNTIME_ACTION_ARBITRATION
int AgentArbitrationActionClassPriority(AgentArbitrationActionClass action_class);
AgentArbitrationCandidate NormalizeAgentActionForArbitration(const AgentRuntimeContext& context,
                                                             const AgentActionRequest& action);
AgentArbitrationRecord ArbitrateAgentActionCandidates(
    const AgentRuntimeContext& context,
    const std::vector<AgentArbitrationCandidate>& candidates,
    const std::vector<AgentArbitrationOverride>& overrides = {});
AgentArbitrationRecord ArbitrateAgentActionsDetailed(
    const AgentRuntimeContext& context,
    const std::vector<AgentActionRequest>& actions,
    const std::vector<AgentArbitrationOverride>& overrides = {});
AgentActionDecision ArbitrateAgentActions(const AgentRuntimeContext& context,
                                          const std::vector<AgentActionRequest>& actions);

// SEARCH_KEY: SB_AGENT_RUNTIME_HUMAN_COMMAND_PRECEDENCE
AgentRuntimeStatus ValidateHumanCommandPrecedence(const std::string& human_command_class,
                                                  const AgentActionRequest& agent_action);

// SEARCH_KEY: SB_AGENT_RUNTIME_MANUAL_APPROVAL_QUEUE
AgentRuntimeStatus ValidateManualApproval(const AgentPolicy& policy,
                                          const AgentActionRequest& action);

// SEARCH_KEY: SB_AGENT_RUNTIME_DRY_RUN
AgentActionDecision BuildDryRunDecision(const AgentTypeDescriptor& descriptor,
                                        const AgentActionRequest& action);

// SEARCH_KEY: SB_AGENT_RUNTIME_OPERATOR_OVERRIDE
AgentRuntimeStatus ValidateOperatorOverride(const AgentRuntimeContext& context,
                                            const AgentActionRequest& action);

// SEARCH_KEY: SB_AGENT_RUNTIME_SAFE_MODE
AgentRuntimeStatus ValidateAgentSafeMode(const AgentInstanceRecord& instance,
                                         const AgentActionRequest& action);

// SEARCH_KEY: SB_AGENT_RUNTIME_SCHEDULER_OWNERSHIP
AgentRuntimeStatus AcquireAgentRunLease(AgentInstanceRecord* instance,
                                        const AgentPolicy& policy,
                                        u64 now_microseconds);

// SEARCH_KEY: SB_AGENT_RUNTIME_SUPERVISION_FAILURE_QUARANTINE
u64 AgentRestartBackoffMicroseconds(const AgentPolicy& policy, u64 restart_attempt);
bool AgentInstanceAllowsRunLease(const AgentInstanceRecord& instance);
AgentRuntimeStatus EnforceAgentSupervisionSafety(AgentInstanceRecord* instance);
AgentSupervisionDecision RecordAgentSupervisionFailure(AgentInstanceRecord* instance,
                                                       const AgentPolicy& policy,
                                                       AgentSupervisionFailureKind failure_kind,
                                                       u64 now_microseconds,
                                                       std::string detail = {});
AgentSupervisionDecision EvaluateAgentSupervisionTick(AgentInstanceRecord* instance,
                                                      const AgentPolicy& policy,
                                                      u64 now_microseconds);
AgentRuntimeStatus RequestAgentSupervisionRestart(AgentInstanceRecord* instance,
                                                  const AgentPolicy& policy,
                                                  u64 now_microseconds);
AgentRuntimeStatus CancelAgentRun(AgentInstanceRecord* instance,
                                  u64 now_microseconds,
                                  std::string reason);
AgentRuntimeStatus QuarantineAgentInstance(AgentInstanceRecord* instance,
                                           u64 now_microseconds,
                                           std::string reason);
AgentRuntimeStatus ValidateAgentActionRetry(const AgentInstanceRecord& instance,
                                            u64 now_microseconds);

// SEARCH_KEY: SB_AGENT_RUNTIME_EXECUTION_ISOLATION
AgentRuntimeStatus ValidateExecutionIsolation(const AgentInstanceRecord& instance,
                                              bool run_threw_exception,
                                              bool watchdog_expired);

// SEARCH_KEY: SB_AGENT_RUNTIME_RESOURCE_QUOTAS
AgentRuntimeStatus ValidateAgentResourceQuota(const AgentPolicy& policy,
                                              u64 runtime_microseconds,
                                              u64 history_rows,
                                              u64 evidence_fanout);

// SEARCH_KEY: SB_AGENT_RUNTIME_CARDINALITY_SCOPE_CONTROLS
AgentRuntimeStatus ValidateAgentCardinality(const AgentPolicy& policy,
                                            u64 label_cardinality,
                                            u64 evidence_fanout,
                                            u64 history_query_rows);

// SEARCH_KEY: SB_AGENT_RUNTIME_DEPENDENCY_GRAPH
AgentRuntimeStatus ValidateAgentDependencyGraph(const std::vector<std::string>& dependency_edges);

// SEARCH_KEY: SB_AGENT_RUNTIME_SIMULATION_REPLAY
AgentRunDecision ReplayAgentDecision(const AgentRuntimeContext& context,
                                     const AgentTypeDescriptor& descriptor,
                                     const AgentPolicy& policy,
                                     const std::vector<std::string>& captured_metric_families);

// SEARCH_KEY: SB_AGENT_RUNTIME_OVERHEAD_GATES
AgentRuntimeStatus ValidateAgentOverheadGate(const AgentPolicy& policy,
                                             u64 runtime_microseconds,
                                             u64 metric_queries,
                                             u64 evidence_writes);

// SEARCH_KEY: SB_AGENT_RUNTIME_STATE_MIGRATION
AgentRuntimeStatus ValidateAgentStateMigration(u64 from_version, u64 to_version);

// SEARCH_KEY: SB_AGENT_RUNTIME_ENGINE_LIFECYCLE_INTEGRATION
AgentActivationProfile EffectiveActivationForLifecycle(AgentActivationProfile configured,
                                                       AgentLifecycleMode mode);

// SEARCH_KEY: SB_AGENT_RUNTIME_ROLLOUT_PROFILES
AgentRuntimeStatus ValidateRolloutTransition(AgentActivationProfile from,
                                             AgentActivationProfile to,
                                             bool explicit_operator_approval);

// SEARCH_KEY: SB_AGENT_RUNTIME_EXPLAINABILITY_OUTPUT
std::vector<std::string> ExplainAgentDecision(const AgentTypeDescriptor& descriptor,
                                              const AgentPolicy& policy,
                                              const AgentActionDecision& decision);

// SEARCH_KEY: SB_AGENT_RUNTIME_DIAGNOSTIC_TAXONOMY
std::vector<std::string> AgentDiagnosticCodes();
bool IsKnownAgentDiagnosticCode(const std::string& code);

// SEARCH_KEY: SB_AGENT_RUNTIME_FAULT_INJECTION_SCENARIOS
std::vector<AgentFaultInjectionScenarioDescriptor> AgentFaultInjectionScenarioDescriptors();
std::optional<AgentFaultInjectionScenarioDescriptor> FindAgentFaultInjectionScenarioDescriptor(
    const std::string& scenario);
std::vector<std::string> AgentFaultInjectionScenarios();
AgentFaultInjectionResult EvaluateAgentFaultInjectionScenarioDetailed(
    const std::string& scenario);
AgentRuntimeStatus EvaluateFaultInjectionScenario(const std::string& scenario);

// SEARCH_KEY: SB_AGENT_NO_WAL_AUTHORITY
bool AgentPersistenceUsesScratchBirdStorageAuthority();

// SEARCH_KEY: SB_METRICS_AGENTS_RUNTIME_FAMILIES
AgentRuntimeStatus RecordAgentRuntimeMetric(const AgentTypeDescriptor& descriptor,
                                            const AgentActionDecision& decision,
                                            u64 decision_latency_microseconds);

}  // namespace scratchbird::core::agents
