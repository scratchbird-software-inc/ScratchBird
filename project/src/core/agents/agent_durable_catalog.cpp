// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_durable_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <map>
#include <openssl/sha.h>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

constexpr const char* kDurableCatalogMagic = "SB_AGENT_DURABLE_CATALOG_IMAGE";
constexpr u64 kDurableCatalogSchemaVersion = 1;

std::string BoolText(bool value) {
  return value ? "1" : "0";
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true";
}

std::string Escape(const std::string& value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    if (ch == '%' || ch == '\n' || ch == '\t' || ch == '=' ||
        ch == '|' || ch == ';') {
      out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
          << static_cast<int>(ch) << std::nouppercase << std::dec;
    } else {
      out << static_cast<char>(ch);
    }
  }
  return out.str();
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') { return ch - '0'; }
  if (ch >= 'a' && ch <= 'f') { return ch - 'a' + 10; }
  if (ch >= 'A' && ch <= 'F') { return ch - 'A' + 10; }
  return -1;
}

std::string Unescape(const std::string& value) {
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int hi = HexValue(value[i + 1]);
      const int lo = HexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

std::string HexBytes(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

std::string Checksum(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

void AddField(std::ostringstream* out, const std::string& key, const std::string& value) {
  *out << '\t' << key << '=' << Escape(value);
}

void AddField(std::ostringstream* out, const std::string& key, u64 value) {
  AddField(out, key, std::to_string(value));
}

void AddField(std::ostringstream* out, const std::string& key, bool value) {
  AddField(out, key, BoolText(value));
}

std::map<std::string, std::string> ParseFields(const std::string& line,
                                               std::string* section) {
  std::map<std::string, std::string> fields;
  std::stringstream stream(line);
  std::string part;
  if (std::getline(stream, part, '\t')) {
    *section = part;
  }
  while (std::getline(stream, part, '\t')) {
    const auto eq = part.find('=');
    if (eq == std::string::npos) { continue; }
    fields.emplace(part.substr(0, eq), Unescape(part.substr(eq + 1)));
  }
  return fields;
}

u64 U64Field(const std::map<std::string, std::string>& fields, const std::string& key) {
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) { return 0; }
  return static_cast<u64>(std::stoull(it->second));
}

std::string StringField(const std::map<std::string, std::string>& fields,
                        const std::string& key) {
  const auto it = fields.find(key);
  return it == fields.end() ? std::string() : it->second;
}

bool BoolField(const std::map<std::string, std::string>& fields, const std::string& key) {
  return ParseBool(StringField(fields, key));
}

std::string JoinVector(const std::vector<std::string>& values) {
  std::ostringstream out;
  bool first = true;
  for (const auto& value : values) {
    if (!first) { out << '|'; }
    first = false;
    out << Escape(value);
  }
  return out.str();
}

std::vector<std::string> VectorField(const std::map<std::string, std::string>& fields,
                                     const std::string& key) {
  std::vector<std::string> values;
  std::stringstream stream(StringField(fields, key));
  std::string part;
  while (std::getline(stream, part, '|')) {
    if (!part.empty()) { values.push_back(Unescape(part)); }
  }
  return values;
}

std::string JoinMap(const std::map<std::string, std::string>& values) {
  std::ostringstream out;
  bool first = true;
  for (const auto& [key, value] : values) {
    if (!first) { out << ';'; }
    first = false;
    out << Escape(key) << '=' << Escape(value);
  }
  return out.str();
}

std::map<std::string, std::string> MapField(
    const std::map<std::string, std::string>& fields,
    const std::string& key) {
  std::map<std::string, std::string> values;
  std::stringstream stream(StringField(fields, key));
  std::string part;
  while (std::getline(stream, part, ';')) {
    if (part.empty()) { continue; }
    const auto eq = part.find('=');
    if (eq == std::string::npos) { continue; }
    values.emplace(Unescape(part.substr(0, eq)), Unescape(part.substr(eq + 1)));
  }
  return values;
}

AgentLifecycleState ParseLifecycleState(const std::string& value) {
  for (AgentLifecycleState candidate :
       {AgentLifecycleState::created, AgentLifecycleState::registered,
        AgentLifecycleState::disabled, AgentLifecycleState::observe_only,
        AgentLifecycleState::recommend_only, AgentLifecycleState::dry_run,
        AgentLifecycleState::running, AgentLifecycleState::paused,
        AgentLifecycleState::safe_mode, AgentLifecycleState::quarantined,
        AgentLifecycleState::stopping, AgentLifecycleState::stopped,
        AgentLifecycleState::retired, AgentLifecycleState::failed}) {
    if (value == AgentLifecycleStateName(candidate)) { return candidate; }
  }
  return AgentLifecycleState::created;
}

AgentActivationProfile ParseActivationProfile(const std::string& value) {
  if (value == "disabled") { return AgentActivationProfile::disabled; }
  if (value == "recommend_only") { return AgentActivationProfile::recommend_only; }
  if (value == "dry_run") { return AgentActivationProfile::dry_run; }
  if (value == "live_action") { return AgentActivationProfile::live_action; }
  return AgentActivationProfile::observe_only;
}

DurableAgentLeaseState ParseLeaseState(const std::string& value) {
  if (value == "acquired") { return DurableAgentLeaseState::acquired; }
  if (value == "draining") { return DurableAgentLeaseState::draining; }
  if (value == "cancelled") { return DurableAgentLeaseState::cancelled; }
  if (value == "quarantined") { return DurableAgentLeaseState::quarantined; }
  if (value == "replay_pending") { return DurableAgentLeaseState::replay_pending; }
  if (value == "expired") { return DurableAgentLeaseState::expired; }
  return DurableAgentLeaseState::none;
}

DurableAgentActionState ParseActionState(const std::string& value) {
  if (value == "running") { return DurableAgentActionState::running; }
  if (value == "completed") { return DurableAgentActionState::completed; }
  if (value == "cancelled") { return DurableAgentActionState::cancelled; }
  if (value == "replay_pending") { return DurableAgentActionState::replay_pending; }
  if (value == "quarantined") { return DurableAgentActionState::quarantined; }
  return DurableAgentActionState::pending;
}

DurableAgentResourceReservationState ParseResourceReservationState(
    const std::string& value) {
  if (value == "released") { return DurableAgentResourceReservationState::released; }
  if (value == "cancelled") { return DurableAgentResourceReservationState::cancelled; }
  if (value == "timeout") { return DurableAgentResourceReservationState::timeout; }
  if (value == "shutdown") { return DurableAgentResourceReservationState::shutdown; }
  return DurableAgentResourceReservationState::acquired;
}

DurableAgentReplayState ParseReplayState(const std::string& value) {
  if (value == "replay_pending") { return DurableAgentReplayState::replay_pending; }
  if (value == "retry_scheduled") { return DurableAgentReplayState::retry_scheduled; }
  if (value == "compensation_required") {
    return DurableAgentReplayState::compensation_required;
  }
  if (value == "compensated") { return DurableAgentReplayState::compensated; }
  if (value == "quarantined") { return DurableAgentReplayState::quarantined; }
  if (value == "quarantine_review_pending") {
    return DurableAgentReplayState::quarantine_review_pending;
  }
  if (value == "quarantine_released") {
    return DurableAgentReplayState::quarantine_released;
  }
  if (value == "refused") { return DurableAgentReplayState::refused; }
  return DurableAgentReplayState::none;
}

bool IsLiveLease(const DurableAgentLeaseRecord& lease, u64 now_microseconds) {
  return (lease.state == DurableAgentLeaseState::acquired ||
          lease.state == DurableAgentLeaseState::draining) &&
         lease.expires_at_microseconds > now_microseconds;
}

bool IsActiveReservation(const DurableAgentResourceReservationRecord& record) {
  return record.state == DurableAgentResourceReservationState::acquired;
}

void RecordHistory(DurableAgentCatalogImage* image,
                   std::string subject_uuid,
                   std::string event_kind,
                   std::string diagnostic,
                   std::string evidence_uuid,
                   u64 now_microseconds) {
  DurableAgentHistoryRecord record;
  record.history_uuid = subject_uuid + ":" + event_kind + ":" +
                        std::to_string(image->retained_history.size() + 1);
  record.subject_uuid = std::move(subject_uuid);
  record.event_kind = std::move(event_kind);
  record.diagnostic_code = std::move(diagnostic);
  record.evidence_uuid = std::move(evidence_uuid);
  record.recorded_at_microseconds = now_microseconds;
  image->retained_history.push_back(std::move(record));
}

std::string PayloadForImage(const DurableAgentCatalogImage& image) {
  std::ostringstream out;
  out << "catalog";
  AddField(&out, "schema_version", image.schema_version);
  AddField(&out, "source", AgentCatalogStateSourceName(image.source));
  AddField(&out, "durable_catalog_authority",
           image.authority.durable_catalog_authority);
  AddField(&out, "mga_transaction_evidence",
           image.authority.mga_transaction_evidence);
  AddField(&out, "mga_transaction_uuid", image.authority.mga_transaction_uuid);
  AddField(&out, "transaction_generation", image.authority.transaction_generation);
  AddField(&out, "authority_evidence_uuid", image.authority.evidence_uuid);
  AddField(&out, "database_uuid", image.authority.database_uuid);
  AddField(&out, "catalog_storage_uuid", image.authority.catalog_storage_uuid);
  AddField(&out, "catalog_root_digest", image.authority.catalog_root_digest);
  AddField(&out, "previous_catalog_root_digest",
           image.authority.previous_catalog_root_digest);
  AddField(&out, "storage_commit_evidence_uuid",
           image.authority.storage_commit_evidence_uuid);
  AddField(&out, "catalog_generation", image.authority.catalog_generation);
  AddField(&out, "local_transaction_id", image.authority.local_transaction_id);
  AddField(&out, "storage_catalog_record_evidence",
           image.authority.storage_catalog_record_evidence);
  AddField(&out, "transaction_inventory_bound",
           image.authority.transaction_inventory_bound);
  AddField(&out, "fsync_or_checkpoint_evidence",
           image.authority.fsync_or_checkpoint_evidence);
  AddField(&out, "sidecar_storage", image.authority.sidecar_storage);
  AddField(&out, "in_memory_only", image.authority.in_memory_only);
  out << '\n';

  for (const auto& instance : image.instances) {
    out << "instance";
    AddField(&out, "instance_uuid", instance.instance_uuid);
    AddField(&out, "agent_type_id", instance.agent_type_id);
    AddField(&out, "policy_uuid", instance.policy_uuid);
    AddField(&out, "scope", instance.scope);
    AddField(&out, "state", std::string(AgentLifecycleStateName(instance.state)));
    AddField(&out, "run_generation", instance.run_generation);
    AddField(&out, "policy_generation", instance.policy_generation);
    AddField(&out, "instance_generation", instance.instance_generation);
    AddField(&out, "retired_generation", instance.retired_generation);
    AddField(&out, "lease_until_microseconds", instance.lease_until_microseconds);
    AddField(&out, "last_run_start_microseconds", instance.last_run_start_microseconds);
    AddField(&out, "last_run_end_microseconds", instance.last_run_end_microseconds);
    AddField(&out, "crash_loop_count", instance.crash_loop_count);
    AddField(&out, "supervision_failure_count", instance.supervision_failure_count);
    AddField(&out, "restart_attempts", instance.restart_attempts);
    AddField(&out, "restart_not_before_microseconds", instance.restart_not_before_microseconds);
    AddField(&out, "cooldown_until_microseconds", instance.cooldown_until_microseconds);
    AddField(&out, "disabled_by_operator", instance.disabled_by_operator);
    AddField(&out, "safe_mode", instance.safe_mode);
    AddField(&out, "quarantined", instance.quarantined);
    AddField(&out, "cancellation_requested", instance.cancellation_requested);
    AddField(&out, "retirement_evidence_uuid", instance.retirement_evidence_uuid);
    AddField(&out, "last_failure_diagnostic_code", instance.last_failure_diagnostic_code);
    AddField(&out, "last_supervision_detail", instance.last_supervision_detail);
    out << '\n';
  }
  for (const auto& policy : image.policies) {
    out << "policy";
    AddField(&out, "policy_uuid", policy.policy_uuid);
    AddField(&out, "policy_name", policy.policy_name);
    AddField(&out, "policy_family", policy.policy_family);
    AddField(&out, "scope", policy.scope);
    AddField(&out, "action_mode", policy.action_mode);
    AddField(&out, "invalid_policy_behavior", policy.invalid_policy_behavior);
    AddField(&out, "activation", std::string(AgentActivationProfileName(policy.activation)));
    AddField(&out, "policy_generation", policy.policy_generation);
    AddField(&out, "enabled", policy.enabled);
    AddField(&out, "allow_live_action", policy.allow_live_action);
    AddField(&out, "require_manual_approval", policy.require_manual_approval);
    AddField(&out, "require_dry_run_before_live", policy.require_dry_run_before_live);
    AddField(&out, "evidence_required", policy.evidence_required);
    AddField(&out, "explainability_required", policy.explainability_required);
    AddField(&out, "run_interval_microseconds", policy.run_interval_microseconds);
    AddField(&out, "jitter_microseconds", policy.jitter_microseconds);
    AddField(&out, "lease_microseconds", policy.lease_microseconds);
    AddField(&out, "cooldown_microseconds", policy.cooldown_microseconds);
    AddField(&out, "max_runtime_microseconds", policy.max_runtime_microseconds);
    AddField(&out, "max_restart_attempts", policy.max_restart_attempts);
    AddField(&out, "initial_backoff_microseconds", policy.initial_backoff_microseconds);
    AddField(&out, "max_backoff_microseconds", policy.max_backoff_microseconds);
    AddField(&out, "max_history_query_rows", policy.max_history_query_rows);
    AddField(&out, "max_evidence_fanout", policy.max_evidence_fanout);
    AddField(&out, "max_label_cardinality", policy.max_label_cardinality);
    AddField(&out, "action_budget_per_window", policy.action_budget_per_window);
    AddField(&out, "required_metric_families", JoinVector(policy.required_metric_families));
    AddField(&out, "policy_dependencies", JoinVector(policy.policy_dependencies));
    AddField(&out, "config_fields", JoinMap(policy.config_fields));
    out << '\n';
  }
  for (const auto& attachment : image.attachments) {
    out << "attachment";
    AddField(&out, "attachment_uuid", attachment.attachment_uuid);
    AddField(&out, "agent_type_id", attachment.agent_type_id);
    AddField(&out, "policy_family", attachment.policy_family);
    AddField(&out, "policy_uuid", attachment.policy_uuid);
    AddField(&out, "scope", attachment.scope);
    AddField(&out, "policy_generation", attachment.policy_generation);
    AddField(&out, "attachment_generation", attachment.attachment_generation);
    AddField(&out, "baseline", attachment.baseline);
    AddField(&out, "active", attachment.active);
    AddField(&out, "valid", attachment.valid);
    AddField(&out, "diagnostic_code", attachment.diagnostic_code);
    AddField(&out, "evidence_uuid", attachment.evidence_uuid);
    out << '\n';
  }
  for (const auto& evidence : image.evidence) {
    out << "evidence";
    AddField(&out, "evidence_uuid", evidence.evidence_uuid);
    AddField(&out, "agent_type_id", evidence.agent_type_id);
    AddField(&out, "instance_uuid", evidence.instance_uuid);
    AddField(&out, "evidence_kind", evidence.evidence_kind);
    AddField(&out, "diagnostic_code", evidence.diagnostic_code);
    AddField(&out, "detail", evidence.detail);
    AddField(&out, "input_metric_digest", evidence.input_metric_digest);
    AddField(&out, "policy_generation", evidence.policy_generation);
    AddField(&out, "principal_uuid", evidence.principal_uuid);
    AddField(&out, "rights_used", JoinVector(evidence.rights_used));
    AddField(&out, "scope_uuids", JoinVector(evidence.scope_uuids));
    AddField(&out, "decision_payload_digest", evidence.decision_payload_digest);
    AddField(&out, "result_state", evidence.result_state);
    AddField(&out, "redaction_class", evidence.redaction_class);
    AddField(&out, "retention_class", evidence.retention_class);
    AddField(&out, "outcome_verification_evidence_uuid",
             evidence.outcome_verification_evidence_uuid);
    AddField(&out, "tamper_digest_algorithm",
             evidence.tamper_digest_algorithm);
    AddField(&out, "tamper_digest", evidence.tamper_digest);
    AddField(&out, "previous_tamper_digest", evidence.previous_tamper_digest);
    AddField(&out, "tamper_chain_digest", evidence.tamper_chain_digest);
    AddField(&out, "tamper_signature_algorithm",
             evidence.tamper_signature_algorithm);
    AddField(&out, "tamper_signature", evidence.tamper_signature);
    AddField(&out, "tamper_key_id", evidence.tamper_key_id);
    AddField(&out, "tamper_key_provenance", evidence.tamper_key_provenance);
    AddField(&out, "tamper_key_generation",
             evidence.tamper_key_generation);
    AddField(&out, "evidence_key_policy_id",
             evidence.evidence_key_policy_id);
    AddField(&out, "tamper_key_rotation_epoch",
             evidence.tamper_key_rotation_epoch);
    AddField(&out, "tamper_key_not_before_microseconds",
             evidence.tamper_key_not_before_microseconds);
    AddField(&out, "tamper_key_not_after_microseconds",
             evidence.tamper_key_not_after_microseconds);
    AddField(&out, "key_residency_class", evidence.key_residency_class);
    AddField(&out, "data_residency_class", evidence.data_residency_class);
    AddField(&out, "storage_linkage_digest",
             evidence.storage_linkage_digest);
    AddField(&out, "tamper_evidence_generation",
             evidence.tamper_evidence_generation);
    AddField(&out, "created_at_microseconds", evidence.created_at_microseconds);
    AddField(&out, "expires_at_microseconds", evidence.expires_at_microseconds);
    AddField(&out, "protected_material_suppressed",
             evidence.protected_material_suppressed);
    AddField(&out, "redaction_applied_before_buffering",
             evidence.redaction_applied_before_buffering);
    AddField(&out, "legal_hold_active", evidence.legal_hold_active);
    AddField(&out, "production_key_material",
             evidence.production_key_material);
    AddField(&out, "test_key_material", evidence.test_key_material);
    AddField(&out, "key_material_exported",
             evidence.key_material_exported);
    AddField(&out, "parser_authority", evidence.parser_authority);
    AddField(&out, "client_authority", evidence.client_authority);
    AddField(&out, "reference_authority", evidence.reference_authority);
    AddField(&out, "sidecar_authority", evidence.sidecar_authority);
    AddField(&out, "transaction_authority", evidence.transaction_authority);
    AddField(&out, "finality_authority", evidence.finality_authority);
    AddField(&out, "visibility_authority", evidence.visibility_authority);
    AddField(&out, "recovery_authority", evidence.recovery_authority);
    AddField(&out, "security_authority", evidence.security_authority);
    out << '\n';
  }
  for (const auto& action : image.actions) {
    out << "action";
    AddField(&out, "action_uuid", action.action_uuid);
    AddField(&out, "instance_uuid", action.instance_uuid);
    AddField(&out, "owner_uuid", action.owner_uuid);
    AddField(&out, "operation_id", action.operation_id);
    AddField(&out, "actuator_provider_id", action.actuator_provider_id);
    AddField(&out, "state", DurableAgentActionStateName(action.state));
    AddField(&out, "idempotency_key", action.idempotency_key);
    AddField(&out, "input_evidence_digest", action.input_evidence_digest);
    AddField(&out, "evidence_uuid", action.evidence_uuid);
    AddField(&out, "verification_evidence_uuid", action.verification_evidence_uuid);
    AddField(&out, "diagnostic_code", action.diagnostic_code);
    AddField(&out, "generation", action.generation);
    AddField(&out, "retry_count", action.retry_count);
    AddField(&out, "outcome_verified", action.outcome_verified);
    AddField(&out, "compensation_required", action.compensation_required);
    AddField(&out, "compensation_attempted", action.compensation_attempted);
    AddField(&out, "retry_scheduled", action.retry_scheduled);
    AddField(&out, "retry_after_microseconds", action.retry_after_microseconds);
    AddField(&out, "retry_evidence_uuid", action.retry_evidence_uuid);
    AddField(&out, "compensation_executor_id", action.compensation_executor_id);
    AddField(&out, "compensation_evidence_uuid",
             action.compensation_evidence_uuid);
    AddField(&out, "parser_authority", action.parser_authority);
    AddField(&out, "client_authority", action.client_authority);
    AddField(&out, "reference_authority", action.reference_authority);
    AddField(&out, "sidecar_authority", action.sidecar_authority);
    out << '\n';
  }
  for (const auto& approval : image.approvals) {
    out << "approval";
    AddField(&out, "approval_uuid", approval.approval_uuid);
    AddField(&out, "action_uuid", approval.action_uuid);
    AddField(&out, "principal_uuid", approval.principal_uuid);
    AddField(&out, "approved", approval.approved);
    AddField(&out, "evidence_uuid", approval.evidence_uuid);
    AddField(&out, "approved_at_microseconds", approval.approved_at_microseconds);
    out << '\n';
  }
  for (const auto& override_record : image.overrides) {
    out << "override";
    AddField(&out, "override_uuid", override_record.override_uuid);
    AddField(&out, "agent_type_id", override_record.agent_type_id);
    AddField(&out, "scope", override_record.scope);
    AddField(&out, "principal_uuid", override_record.principal_uuid);
    AddField(&out, "expires_at_microseconds", override_record.expires_at_microseconds);
    AddField(&out, "active", override_record.active);
    AddField(&out, "evidence_uuid", override_record.evidence_uuid);
    out << '\n';
  }
  for (const auto& lease : image.leases) {
    out << "lease";
    AddField(&out, "lease_uuid", lease.lease_uuid);
    AddField(&out, "instance_uuid", lease.instance_uuid);
    AddField(&out, "owner_uuid", lease.owner_uuid);
    AddField(&out, "state", DurableAgentLeaseStateName(lease.state));
    AddField(&out, "acquired_at_microseconds", lease.acquired_at_microseconds);
    AddField(&out, "expires_at_microseconds", lease.expires_at_microseconds);
    AddField(&out, "heartbeat_generation", lease.heartbeat_generation);
    AddField(&out, "last_heartbeat_microseconds", lease.last_heartbeat_microseconds);
    AddField(&out, "replay_generation", lease.replay_generation);
    AddField(&out, "evidence_uuid", lease.evidence_uuid);
    out << '\n';
  }
  for (const auto& reservation : image.resource_reservations) {
    out << "resource_reservation";
    AddField(&out, "reservation_uuid", reservation.reservation_uuid);
    AddField(&out, "reservation_key", reservation.reservation_key);
    AddField(&out, "owner_scope", reservation.owner_scope);
    AddField(&out, "agent_type_id", reservation.agent_type_id);
    AddField(&out, "operation_id", reservation.operation_id);
    AddField(&out, "state",
             DurableAgentResourceReservationStateName(reservation.state));
    AddField(&out, "acquired_at_microseconds",
             reservation.acquired_at_microseconds);
    AddField(&out, "released_at_microseconds",
             reservation.released_at_microseconds);
    AddField(&out, "memory_bytes", reservation.memory_bytes);
    AddField(&out, "worker_slots", reservation.worker_slots);
    AddField(&out, "overhead_microseconds",
             reservation.overhead_microseconds);
    AddField(&out, "evidence_uuid", reservation.evidence_uuid);
    AddField(&out, "release_evidence_uuid",
             reservation.release_evidence_uuid);
    AddField(&out, "release_reason", reservation.release_reason);
    AddField(&out, "parser_authority", reservation.parser_authority);
    AddField(&out, "client_authority", reservation.client_authority);
    AddField(&out, "reference_authority", reservation.reference_authority);
    AddField(&out, "benchmark_authority", reservation.benchmark_authority);
    out << '\n';
  }
  for (const auto& replay : image.replay_records) {
    out << "replay";
    AddField(&out, "replay_uuid", replay.replay_uuid);
    AddField(&out, "action_uuid", replay.action_uuid);
    AddField(&out, "instance_uuid", replay.instance_uuid);
    AddField(&out, "operation_id", replay.operation_id);
    AddField(&out, "idempotency_key", replay.idempotency_key);
    AddField(&out, "state", DurableAgentReplayStateName(replay.state));
    AddField(&out, "replay_generation", replay.replay_generation);
    AddField(&out, "retry_count", replay.retry_count);
    AddField(&out, "max_retry_count", replay.max_retry_count);
    AddField(&out, "retry_after_microseconds",
             replay.retry_after_microseconds);
    AddField(&out, "recorded_at_microseconds",
             replay.recorded_at_microseconds);
    AddField(&out, "policy_digest", replay.policy_digest);
    AddField(&out, "policy_generation", replay.policy_generation);
    AddField(&out, "metric_digest", replay.metric_digest);
    AddField(&out, "catalog_root_digest", replay.catalog_root_digest);
    AddField(&out, "security_digest", replay.security_digest);
    AddField(&out, "security_epoch", replay.security_epoch);
    AddField(&out, "resource_reservation_digest",
             replay.resource_reservation_digest);
    AddField(&out, "binary_package_digest", replay.binary_package_digest);
    AddField(&out, "action_input_digest", replay.action_input_digest);
    AddField(&out, "action_evidence_digest",
             replay.action_evidence_digest);
    AddField(&out, "action_record_digest", replay.action_record_digest);
    AddField(&out, "evidence_chain_digest", replay.evidence_chain_digest);
    AddField(&out, "evidence_uuid", replay.evidence_uuid);
    AddField(&out, "review_evidence_uuid", replay.review_evidence_uuid);
    AddField(&out, "compensation_evidence_uuid",
             replay.compensation_evidence_uuid);
    AddField(&out, "diagnostic_code", replay.diagnostic_code);
    AddField(&out, "review_required", replay.review_required);
    AddField(&out, "review_approved", replay.review_approved);
    AddField(&out, "compensation_required",
             replay.compensation_required);
    AddField(&out, "compensation_attempted",
             replay.compensation_attempted);
    AddField(&out, "retry_scheduled", replay.retry_scheduled);
    AddField(&out, "cluster_route_requested",
             replay.cluster_route_requested);
    AddField(&out, "external_cluster_provider_attested",
             replay.external_cluster_provider_attested);
    AddField(&out, "parser_authority", replay.parser_authority);
    AddField(&out, "client_authority", replay.client_authority);
    AddField(&out, "reference_authority", replay.reference_authority);
    AddField(&out, "wal_authority", replay.wal_authority);
    AddField(&out, "benchmark_authority", replay.benchmark_authority);
    AddField(&out, "optimizer_plan_authority",
             replay.optimizer_plan_authority);
    AddField(&out, "index_finality_authority",
             replay.index_finality_authority);
    AddField(&out, "provider_finality_authority",
             replay.provider_finality_authority);
    AddField(&out, "memory_authority", replay.memory_authority);
    AddField(&out, "agent_action_authority",
             replay.agent_action_authority);
    out << '\n';
  }
  for (const auto& health : image.health) {
    out << "health";
    AddField(&out, "instance_uuid", health.instance_uuid);
    AddField(&out, "health_state", health.health_state);
    AddField(&out, "diagnostic_code", health.diagnostic_code);
    AddField(&out, "evidence_uuid", health.evidence_uuid);
    AddField(&out, "observed_at_microseconds", health.observed_at_microseconds);
    out << '\n';
  }
  for (const auto& history : image.retained_history) {
    out << "history";
    AddField(&out, "history_uuid", history.history_uuid);
    AddField(&out, "subject_uuid", history.subject_uuid);
    AddField(&out, "event_kind", history.event_kind);
    AddField(&out, "diagnostic_code", history.diagnostic_code);
    AddField(&out, "evidence_uuid", history.evidence_uuid);
    AddField(&out, "recorded_at_microseconds", history.recorded_at_microseconds);
    out << '\n';
  }
  for (const auto& migration : image.migrations) {
    out << "migration";
    AddField(&out, "migration_uuid", migration.migration_uuid);
    AddField(&out, "from_schema_version", migration.from_schema_version);
    AddField(&out, "to_schema_version", migration.to_schema_version);
    AddField(&out, "result", migration.result);
    AddField(&out, "evidence_uuid", migration.evidence_uuid);
    AddField(&out, "source_root_digest", migration.source_root_digest);
    AddField(&out, "target_root_digest", migration.target_root_digest);
    AddField(&out, "recorded_at_microseconds",
             migration.recorded_at_microseconds);
    out << '\n';
  }
  return out.str();
}

}  // namespace

std::string DurableAgentLeaseStateName(DurableAgentLeaseState state) {
  switch (state) {
    case DurableAgentLeaseState::none: return "none";
    case DurableAgentLeaseState::acquired: return "acquired";
    case DurableAgentLeaseState::draining: return "draining";
    case DurableAgentLeaseState::cancelled: return "cancelled";
    case DurableAgentLeaseState::quarantined: return "quarantined";
    case DurableAgentLeaseState::replay_pending: return "replay_pending";
    case DurableAgentLeaseState::expired: return "expired";
  }
  return "none";
}

std::string DurableAgentActionStateName(DurableAgentActionState state) {
  switch (state) {
    case DurableAgentActionState::pending: return "pending";
    case DurableAgentActionState::running: return "running";
    case DurableAgentActionState::completed: return "completed";
    case DurableAgentActionState::cancelled: return "cancelled";
    case DurableAgentActionState::replay_pending: return "replay_pending";
    case DurableAgentActionState::quarantined: return "quarantined";
  }
  return "pending";
}

std::string DurableAgentResourceReservationStateName(
    DurableAgentResourceReservationState state) {
  switch (state) {
    case DurableAgentResourceReservationState::acquired: return "acquired";
    case DurableAgentResourceReservationState::released: return "released";
    case DurableAgentResourceReservationState::cancelled: return "cancelled";
    case DurableAgentResourceReservationState::timeout: return "timeout";
    case DurableAgentResourceReservationState::shutdown: return "shutdown";
  }
  return "acquired";
}

std::string DurableAgentReplayStateName(DurableAgentReplayState state) {
  switch (state) {
    case DurableAgentReplayState::none: return "none";
    case DurableAgentReplayState::replay_pending: return "replay_pending";
    case DurableAgentReplayState::retry_scheduled: return "retry_scheduled";
    case DurableAgentReplayState::compensation_required:
      return "compensation_required";
    case DurableAgentReplayState::compensated: return "compensated";
    case DurableAgentReplayState::quarantined: return "quarantined";
    case DurableAgentReplayState::quarantine_review_pending:
      return "quarantine_review_pending";
    case DurableAgentReplayState::quarantine_released:
      return "quarantine_released";
    case DurableAgentReplayState::refused: return "refused";
  }
  return "none";
}

std::string AgentCatalogStateSourceName(AgentCatalogStateSource source) {
  switch (source) {
    case AgentCatalogStateSource::durable_catalog_image: return "durable_catalog_image";
    case AgentCatalogStateSource::in_memory_bootstrap: return "in_memory_bootstrap";
    case AgentCatalogStateSource::sidecar_legacy: return "sidecar_legacy";
    case AgentCatalogStateSource::pipe_delimited_legacy: return "pipe_delimited_legacy";
  }
  return "in_memory_bootstrap";
}

std::string SerializeDurableAgentCatalogImage(const DurableAgentCatalogImage& image) {
  const std::string payload = PayloadForImage(image);
  std::ostringstream out;
  out << kDurableCatalogMagic << "\tschema_version=" << kDurableCatalogSchemaVersion
      << "\tchecksum=" << Checksum(payload) << '\n'
      << payload;
  return out.str();
}

std::string DurableAgentCatalogRootDigest(DurableAgentCatalogImage image) {
  image.authority.catalog_root_digest.clear();
  for (auto& migration : image.migrations) {
    migration.target_root_digest.clear();
  }
  return Checksum(PayloadForImage(image));
}

AgentRuntimeStatus RefreshDurableAgentCatalogAuthorityDigest(
    DurableAgentCatalogImage* image,
    const std::string& evidence_uuid) {
  if (image == nullptr) {
    return AgentError("SB_AGENT_CATALOG.CATALOG_REQUIRED");
  }
  if (image->source != AgentCatalogStateSource::durable_catalog_image ||
      image->schema_version != kDurableCatalogSchemaVersion ||
      !image->authority.durable_catalog_authority ||
      !image->authority.mga_transaction_evidence ||
      image->authority.mga_transaction_uuid.empty() ||
      image->authority.database_uuid.empty() ||
      image->authority.catalog_storage_uuid.empty() ||
      image->authority.local_transaction_id == 0 ||
      !image->authority.storage_catalog_record_evidence ||
      !image->authority.transaction_inventory_bound ||
      !image->authority.fsync_or_checkpoint_evidence ||
      image->authority.sidecar_storage ||
      image->authority.in_memory_only) {
    return AgentError("SB_AGENT_CATALOG.STORAGE_MGA_AUTHORITY_REQUIRED");
  }
  image->authority.previous_catalog_root_digest =
      image->authority.catalog_root_digest;
  if (!evidence_uuid.empty()) {
    image->authority.evidence_uuid = evidence_uuid;
    image->authority.storage_commit_evidence_uuid = evidence_uuid;
  }
  if (image->authority.evidence_uuid.empty() ||
      image->authority.storage_commit_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_CATALOG.STORAGE_COMMIT_EVIDENCE_REQUIRED");
  }
  if (image->authority.transaction_generation == 0) {
    image->authority.transaction_generation = 1;
  }
  ++image->authority.catalog_generation;
  image->authority.catalog_root_digest = DurableAgentCatalogRootDigest(*image);
  return {true, "SB_AGENT_CATALOG.ROOT_DIGEST_REFRESHED",
          image->authority.catalog_root_digest};
}

DurableCatalogMigrationResult MigrateDurableAgentCatalogImageForProduction(
    DurableAgentCatalogImage image,
    const std::string& evidence_uuid,
    u64 recorded_at_microseconds) {
  DurableCatalogMigrationResult result;
  if (evidence_uuid.empty()) {
    result.status = AgentError("SB_AGENT_CATALOG.MIGRATION_EVIDENCE_REQUIRED");
    return result;
  }
  if (image.schema_version > kDurableCatalogSchemaVersion) {
    result.status = AgentError("SB_AGENT_CATALOG.SCHEMA_VERSION_UNSUPPORTED");
    return result;
  }
  if (image.source != AgentCatalogStateSource::durable_catalog_image ||
      !image.authority.durable_catalog_authority ||
      !image.authority.mga_transaction_evidence ||
      image.authority.mga_transaction_uuid.empty() ||
      image.authority.database_uuid.empty() ||
      image.authority.catalog_storage_uuid.empty() ||
      image.authority.local_transaction_id == 0 ||
      !image.authority.storage_catalog_record_evidence ||
      !image.authority.transaction_inventory_bound ||
      !image.authority.fsync_or_checkpoint_evidence ||
      image.authority.sidecar_storage ||
      image.authority.in_memory_only) {
    result.status = AgentError("SB_AGENT_CATALOG.STORAGE_MGA_AUTHORITY_REQUIRED");
    return result;
  }
  if (image.schema_version == kDurableCatalogSchemaVersion) {
    result.image = std::move(image);
    result.status = {true, "SB_AGENT_CATALOG.MIGRATION_NOT_REQUIRED",
                     evidence_uuid};
    return result;
  }

  DurableAgentStateMigrationRecord migration;
  migration.from_schema_version = image.schema_version;
  migration.to_schema_version = kDurableCatalogSchemaVersion;
  migration.result = "migrated";
  migration.evidence_uuid = evidence_uuid;
  migration.recorded_at_microseconds = recorded_at_microseconds;
  migration.source_root_digest = image.authority.catalog_root_digest.empty()
                                     ? DurableAgentCatalogRootDigest(image)
                                     : image.authority.catalog_root_digest;
  migration.migration_uuid =
      "agent_catalog_migration:" +
      std::to_string(migration.from_schema_version) + "->" +
      std::to_string(migration.to_schema_version) + ":" + evidence_uuid;

  image.schema_version = kDurableCatalogSchemaVersion;
  image.migrations.push_back(migration);
  const auto refreshed =
      RefreshDurableAgentCatalogAuthorityDigest(&image, evidence_uuid);
  if (!refreshed.ok) {
    result.status = refreshed;
    return result;
  }
  image.migrations.back().target_root_digest =
      image.authority.catalog_root_digest;
  result.migration = image.migrations.back();
  result.image = std::move(image);
  result.migrated = true;
  result.status = {true, "SB_AGENT_CATALOG.MIGRATED",
                   result.migration.migration_uuid};
  return result;
}

DurableCatalogValidationResult ValidateDurableAgentCatalogImage(
    const std::string& encoded,
    bool production_live_path) {
  DurableCatalogValidationResult result;
  if (encoded.find(kDurableCatalogMagic) != 0) {
    if (encoded.find('|') != std::string::npos) {
      result.image.source = AgentCatalogStateSource::pipe_delimited_legacy;
      result.status = AgentError("SB_AGENT_CATALOG.LEGACY_PIPE_DELIMITED_REJECTED",
                                 "pipe-delimited agent state is compatibility/test-only");
      return result;
    }
    result.status = AgentError("SB_AGENT_CATALOG.IMAGE_MAGIC_INVALID");
    return result;
  }

  const auto first_newline = encoded.find('\n');
  if (first_newline == std::string::npos) {
    result.status = AgentError("SB_AGENT_CATALOG.IMAGE_HEADER_INVALID");
    return result;
  }
  std::string section;
  const auto header = ParseFields(encoded.substr(0, first_newline), &section);
  const std::string payload = encoded.substr(first_newline + 1);
  const std::string expected_checksum = StringField(header, "checksum");
  const u64 header_schema_version = U64Field(header, "schema_version");
  result.checksum = Checksum(payload);
  if (section != kDurableCatalogMagic ||
      header_schema_version > kDurableCatalogSchemaVersion) {
    result.status = AgentError("SB_AGENT_CATALOG.SCHEMA_VERSION_UNSUPPORTED");
    return result;
  }
  if (expected_checksum.empty() || expected_checksum != result.checksum) {
    result.status = AgentError("SB_AGENT_CATALOG.CHECKSUM_MISMATCH");
    return result;
  }

  std::stringstream stream(payload);
  std::string line;
  try {
    while (std::getline(stream, line)) {
      if (line.empty()) { continue; }
      auto fields = ParseFields(line, &section);
      if (section == "catalog") {
        result.image.schema_version = U64Field(fields, "schema_version");
        const auto source = StringField(fields, "source");
        result.image.source = source == "durable_catalog_image"
                                  ? AgentCatalogStateSource::durable_catalog_image
                                  : source == "sidecar_legacy"
                                        ? AgentCatalogStateSource::sidecar_legacy
                                        : source == "pipe_delimited_legacy"
                                              ? AgentCatalogStateSource::pipe_delimited_legacy
                                              : AgentCatalogStateSource::in_memory_bootstrap;
        result.image.authority.durable_catalog_authority =
            BoolField(fields, "durable_catalog_authority");
        result.image.authority.mga_transaction_evidence =
            BoolField(fields, "mga_transaction_evidence");
        result.image.authority.mga_transaction_uuid =
            StringField(fields, "mga_transaction_uuid");
        result.image.authority.transaction_generation =
            U64Field(fields, "transaction_generation");
        result.image.authority.evidence_uuid =
            StringField(fields, "authority_evidence_uuid");
        result.image.authority.database_uuid = StringField(fields, "database_uuid");
        result.image.authority.catalog_storage_uuid =
            StringField(fields, "catalog_storage_uuid");
        result.image.authority.catalog_root_digest =
            StringField(fields, "catalog_root_digest");
        result.image.authority.previous_catalog_root_digest =
            StringField(fields, "previous_catalog_root_digest");
        result.image.authority.storage_commit_evidence_uuid =
            StringField(fields, "storage_commit_evidence_uuid");
        result.image.authority.catalog_generation =
            U64Field(fields, "catalog_generation");
        result.image.authority.local_transaction_id =
            U64Field(fields, "local_transaction_id");
        result.image.authority.storage_catalog_record_evidence =
            BoolField(fields, "storage_catalog_record_evidence");
        result.image.authority.transaction_inventory_bound =
            BoolField(fields, "transaction_inventory_bound");
        result.image.authority.fsync_or_checkpoint_evidence =
            BoolField(fields, "fsync_or_checkpoint_evidence");
        result.image.authority.sidecar_storage =
            BoolField(fields, "sidecar_storage");
        result.image.authority.in_memory_only =
            BoolField(fields, "in_memory_only");
      } else if (section == "instance") {
        AgentInstanceRecord record;
        record.instance_uuid = StringField(fields, "instance_uuid");
        record.agent_type_id = StringField(fields, "agent_type_id");
        record.policy_uuid = StringField(fields, "policy_uuid");
        record.scope = StringField(fields, "scope");
        record.state = ParseLifecycleState(StringField(fields, "state"));
        record.run_generation = U64Field(fields, "run_generation");
        record.policy_generation = U64Field(fields, "policy_generation");
        record.instance_generation = U64Field(fields, "instance_generation");
        record.retired_generation = U64Field(fields, "retired_generation");
        record.lease_until_microseconds = U64Field(fields, "lease_until_microseconds");
        record.last_run_start_microseconds =
            U64Field(fields, "last_run_start_microseconds");
        record.last_run_end_microseconds = U64Field(fields, "last_run_end_microseconds");
        record.crash_loop_count = U64Field(fields, "crash_loop_count");
        record.supervision_failure_count = U64Field(fields, "supervision_failure_count");
        record.restart_attempts = U64Field(fields, "restart_attempts");
        record.restart_not_before_microseconds =
            U64Field(fields, "restart_not_before_microseconds");
        record.cooldown_until_microseconds = U64Field(fields, "cooldown_until_microseconds");
        record.disabled_by_operator = BoolField(fields, "disabled_by_operator");
        record.safe_mode = BoolField(fields, "safe_mode");
        record.quarantined = BoolField(fields, "quarantined");
        record.cancellation_requested = BoolField(fields, "cancellation_requested");
        record.retirement_evidence_uuid = StringField(fields, "retirement_evidence_uuid");
        record.last_failure_diagnostic_code =
            StringField(fields, "last_failure_diagnostic_code");
        record.last_supervision_detail = StringField(fields, "last_supervision_detail");
        result.image.instances.push_back(std::move(record));
      } else if (section == "policy") {
        AgentPolicy policy;
        policy.policy_uuid = StringField(fields, "policy_uuid");
        policy.policy_name = StringField(fields, "policy_name");
        policy.policy_family = StringField(fields, "policy_family");
        policy.scope = StringField(fields, "scope");
        policy.action_mode = StringField(fields, "action_mode");
        policy.invalid_policy_behavior = StringField(fields, "invalid_policy_behavior");
        policy.activation = ParseActivationProfile(StringField(fields, "activation"));
        policy.policy_generation = U64Field(fields, "policy_generation");
        policy.enabled = BoolField(fields, "enabled");
        policy.allow_live_action = BoolField(fields, "allow_live_action");
        policy.require_manual_approval = BoolField(fields, "require_manual_approval");
        policy.require_dry_run_before_live =
            BoolField(fields, "require_dry_run_before_live");
        policy.evidence_required = BoolField(fields, "evidence_required");
        policy.explainability_required = BoolField(fields, "explainability_required");
        policy.run_interval_microseconds = U64Field(fields, "run_interval_microseconds");
        policy.jitter_microseconds = U64Field(fields, "jitter_microseconds");
        policy.lease_microseconds = U64Field(fields, "lease_microseconds");
        policy.cooldown_microseconds = U64Field(fields, "cooldown_microseconds");
        policy.max_runtime_microseconds = U64Field(fields, "max_runtime_microseconds");
        policy.max_restart_attempts = U64Field(fields, "max_restart_attempts");
        policy.initial_backoff_microseconds =
            U64Field(fields, "initial_backoff_microseconds");
        policy.max_backoff_microseconds = U64Field(fields, "max_backoff_microseconds");
        policy.max_history_query_rows = U64Field(fields, "max_history_query_rows");
        policy.max_evidence_fanout = U64Field(fields, "max_evidence_fanout");
        policy.max_label_cardinality = U64Field(fields, "max_label_cardinality");
        policy.action_budget_per_window = U64Field(fields, "action_budget_per_window");
        policy.required_metric_families =
            VectorField(fields, "required_metric_families");
        policy.policy_dependencies = VectorField(fields, "policy_dependencies");
        policy.config_fields = MapField(fields, "config_fields");
        result.image.policies.push_back(std::move(policy));
      } else if (section == "attachment") {
        AgentPolicyAttachmentRecord attachment;
        attachment.attachment_uuid = StringField(fields, "attachment_uuid");
        attachment.agent_type_id = StringField(fields, "agent_type_id");
        attachment.policy_family = StringField(fields, "policy_family");
        attachment.policy_uuid = StringField(fields, "policy_uuid");
        attachment.scope = StringField(fields, "scope");
        attachment.policy_generation = U64Field(fields, "policy_generation");
        attachment.attachment_generation = U64Field(fields, "attachment_generation");
        attachment.baseline = BoolField(fields, "baseline");
        attachment.active = BoolField(fields, "active");
        attachment.valid = BoolField(fields, "valid");
        attachment.diagnostic_code = StringField(fields, "diagnostic_code");
        attachment.evidence_uuid = StringField(fields, "evidence_uuid");
        result.image.attachments.push_back(std::move(attachment));
      } else if (section == "evidence") {
        AgentEvidenceRecord evidence;
        evidence.evidence_uuid = StringField(fields, "evidence_uuid");
        evidence.agent_type_id = StringField(fields, "agent_type_id");
        evidence.instance_uuid = StringField(fields, "instance_uuid");
        evidence.evidence_kind = StringField(fields, "evidence_kind");
        evidence.diagnostic_code = StringField(fields, "diagnostic_code");
        evidence.detail = StringField(fields, "detail");
        evidence.input_metric_digest = StringField(fields, "input_metric_digest");
        evidence.policy_generation = U64Field(fields, "policy_generation");
        evidence.principal_uuid = StringField(fields, "principal_uuid");
        evidence.rights_used = VectorField(fields, "rights_used");
        evidence.scope_uuids = VectorField(fields, "scope_uuids");
        evidence.decision_payload_digest =
            StringField(fields, "decision_payload_digest");
        evidence.result_state = StringField(fields, "result_state");
        evidence.redaction_class = StringField(fields, "redaction_class");
        evidence.retention_class = StringField(fields, "retention_class");
        evidence.outcome_verification_evidence_uuid =
            StringField(fields, "outcome_verification_evidence_uuid");
        evidence.tamper_digest_algorithm =
            StringField(fields, "tamper_digest_algorithm");
        evidence.tamper_digest = StringField(fields, "tamper_digest");
        evidence.previous_tamper_digest =
            StringField(fields, "previous_tamper_digest");
        evidence.tamper_chain_digest =
            StringField(fields, "tamper_chain_digest");
        evidence.tamper_signature_algorithm =
            StringField(fields, "tamper_signature_algorithm");
        evidence.tamper_signature = StringField(fields, "tamper_signature");
        evidence.tamper_key_id = StringField(fields, "tamper_key_id");
        evidence.tamper_key_provenance =
            StringField(fields, "tamper_key_provenance");
        evidence.tamper_key_generation =
            U64Field(fields, "tamper_key_generation");
        evidence.evidence_key_policy_id =
            StringField(fields, "evidence_key_policy_id");
        evidence.tamper_key_rotation_epoch =
            U64Field(fields, "tamper_key_rotation_epoch");
        evidence.tamper_key_not_before_microseconds =
            U64Field(fields, "tamper_key_not_before_microseconds");
        evidence.tamper_key_not_after_microseconds =
            U64Field(fields, "tamper_key_not_after_microseconds");
        evidence.key_residency_class =
            StringField(fields, "key_residency_class");
        evidence.data_residency_class =
            StringField(fields, "data_residency_class");
        evidence.storage_linkage_digest =
            StringField(fields, "storage_linkage_digest");
        evidence.tamper_evidence_generation =
            U64Field(fields, "tamper_evidence_generation");
        evidence.created_at_microseconds = U64Field(fields, "created_at_microseconds");
        evidence.expires_at_microseconds = U64Field(fields, "expires_at_microseconds");
        evidence.protected_material_suppressed =
            BoolField(fields, "protected_material_suppressed");
        evidence.redaction_applied_before_buffering =
            BoolField(fields, "redaction_applied_before_buffering");
        evidence.legal_hold_active = BoolField(fields, "legal_hold_active");
        evidence.production_key_material =
            BoolField(fields, "production_key_material");
        evidence.test_key_material = BoolField(fields, "test_key_material");
        evidence.key_material_exported =
            BoolField(fields, "key_material_exported");
        evidence.parser_authority = BoolField(fields, "parser_authority");
        evidence.client_authority = BoolField(fields, "client_authority");
        evidence.reference_authority = BoolField(fields, "reference_authority");
        evidence.sidecar_authority = BoolField(fields, "sidecar_authority");
        evidence.transaction_authority = BoolField(fields, "transaction_authority");
        evidence.finality_authority = BoolField(fields, "finality_authority");
        evidence.visibility_authority = BoolField(fields, "visibility_authority");
        evidence.recovery_authority = BoolField(fields, "recovery_authority");
        evidence.security_authority = BoolField(fields, "security_authority");
        result.image.evidence.push_back(std::move(evidence));
      } else if (section == "action") {
        DurableAgentActionRecord action;
        action.action_uuid = StringField(fields, "action_uuid");
        action.instance_uuid = StringField(fields, "instance_uuid");
        action.owner_uuid = StringField(fields, "owner_uuid");
        action.operation_id = StringField(fields, "operation_id");
        action.actuator_provider_id = StringField(fields, "actuator_provider_id");
        action.state = ParseActionState(StringField(fields, "state"));
        action.idempotency_key = StringField(fields, "idempotency_key");
        action.input_evidence_digest = StringField(fields, "input_evidence_digest");
        action.evidence_uuid = StringField(fields, "evidence_uuid");
        action.verification_evidence_uuid =
            StringField(fields, "verification_evidence_uuid");
        action.diagnostic_code = StringField(fields, "diagnostic_code");
        action.generation = U64Field(fields, "generation");
        action.retry_count = U64Field(fields, "retry_count");
        action.outcome_verified = BoolField(fields, "outcome_verified");
        action.compensation_required = BoolField(fields, "compensation_required");
        action.compensation_attempted = BoolField(fields, "compensation_attempted");
        action.retry_scheduled = BoolField(fields, "retry_scheduled");
        action.retry_after_microseconds =
            U64Field(fields, "retry_after_microseconds");
        action.retry_evidence_uuid = StringField(fields, "retry_evidence_uuid");
        action.compensation_executor_id =
            StringField(fields, "compensation_executor_id");
        action.compensation_evidence_uuid =
            StringField(fields, "compensation_evidence_uuid");
        action.parser_authority = BoolField(fields, "parser_authority");
        action.client_authority = BoolField(fields, "client_authority");
        action.reference_authority = BoolField(fields, "reference_authority");
        action.sidecar_authority = BoolField(fields, "sidecar_authority");
        result.image.actions.push_back(std::move(action));
      } else if (section == "approval") {
        DurableAgentApprovalRecord approval;
        approval.approval_uuid = StringField(fields, "approval_uuid");
        approval.action_uuid = StringField(fields, "action_uuid");
        approval.principal_uuid = StringField(fields, "principal_uuid");
        approval.approved = BoolField(fields, "approved");
        approval.evidence_uuid = StringField(fields, "evidence_uuid");
        approval.approved_at_microseconds = U64Field(fields, "approved_at_microseconds");
        result.image.approvals.push_back(std::move(approval));
      } else if (section == "override") {
        DurableAgentOverrideRecord override_record;
        override_record.override_uuid = StringField(fields, "override_uuid");
        override_record.agent_type_id = StringField(fields, "agent_type_id");
        override_record.scope = StringField(fields, "scope");
        override_record.principal_uuid = StringField(fields, "principal_uuid");
        override_record.expires_at_microseconds =
            U64Field(fields, "expires_at_microseconds");
        override_record.active = BoolField(fields, "active");
        override_record.evidence_uuid = StringField(fields, "evidence_uuid");
        result.image.overrides.push_back(std::move(override_record));
      } else if (section == "lease") {
        DurableAgentLeaseRecord lease;
        lease.lease_uuid = StringField(fields, "lease_uuid");
        lease.instance_uuid = StringField(fields, "instance_uuid");
        lease.owner_uuid = StringField(fields, "owner_uuid");
        lease.state = ParseLeaseState(StringField(fields, "state"));
        lease.acquired_at_microseconds = U64Field(fields, "acquired_at_microseconds");
        lease.expires_at_microseconds = U64Field(fields, "expires_at_microseconds");
        lease.heartbeat_generation = U64Field(fields, "heartbeat_generation");
        lease.last_heartbeat_microseconds = U64Field(fields, "last_heartbeat_microseconds");
        lease.replay_generation = U64Field(fields, "replay_generation");
        lease.evidence_uuid = StringField(fields, "evidence_uuid");
        result.image.leases.push_back(std::move(lease));
      } else if (section == "resource_reservation") {
        DurableAgentResourceReservationRecord reservation;
        reservation.reservation_uuid = StringField(fields, "reservation_uuid");
        reservation.reservation_key = StringField(fields, "reservation_key");
        reservation.owner_scope = StringField(fields, "owner_scope");
        reservation.agent_type_id = StringField(fields, "agent_type_id");
        reservation.operation_id = StringField(fields, "operation_id");
        reservation.state = ParseResourceReservationState(
            StringField(fields, "state"));
        reservation.acquired_at_microseconds =
            U64Field(fields, "acquired_at_microseconds");
        reservation.released_at_microseconds =
            U64Field(fields, "released_at_microseconds");
        reservation.memory_bytes = U64Field(fields, "memory_bytes");
        reservation.worker_slots = U64Field(fields, "worker_slots");
        reservation.overhead_microseconds =
            U64Field(fields, "overhead_microseconds");
        reservation.evidence_uuid = StringField(fields, "evidence_uuid");
        reservation.release_evidence_uuid =
            StringField(fields, "release_evidence_uuid");
        reservation.release_reason = StringField(fields, "release_reason");
        reservation.parser_authority = BoolField(fields, "parser_authority");
        reservation.client_authority = BoolField(fields, "client_authority");
        reservation.reference_authority = BoolField(fields, "reference_authority");
        reservation.benchmark_authority =
            BoolField(fields, "benchmark_authority");
        result.image.resource_reservations.push_back(std::move(reservation));
      } else if (section == "replay") {
        DurableAgentReplayRecord replay;
        replay.replay_uuid = StringField(fields, "replay_uuid");
        replay.action_uuid = StringField(fields, "action_uuid");
        replay.instance_uuid = StringField(fields, "instance_uuid");
        replay.operation_id = StringField(fields, "operation_id");
        replay.idempotency_key = StringField(fields, "idempotency_key");
        replay.state = ParseReplayState(StringField(fields, "state"));
        replay.replay_generation = U64Field(fields, "replay_generation");
        replay.retry_count = U64Field(fields, "retry_count");
        replay.max_retry_count = U64Field(fields, "max_retry_count");
        replay.retry_after_microseconds =
            U64Field(fields, "retry_after_microseconds");
        replay.recorded_at_microseconds =
            U64Field(fields, "recorded_at_microseconds");
        replay.policy_digest = StringField(fields, "policy_digest");
        replay.policy_generation = U64Field(fields, "policy_generation");
        replay.metric_digest = StringField(fields, "metric_digest");
        replay.catalog_root_digest = StringField(fields, "catalog_root_digest");
        replay.security_digest = StringField(fields, "security_digest");
        replay.security_epoch = U64Field(fields, "security_epoch");
        replay.resource_reservation_digest =
            StringField(fields, "resource_reservation_digest");
        replay.binary_package_digest =
            StringField(fields, "binary_package_digest");
        replay.action_input_digest =
            StringField(fields, "action_input_digest");
        replay.action_evidence_digest =
            StringField(fields, "action_evidence_digest");
        replay.action_record_digest =
            StringField(fields, "action_record_digest");
        replay.evidence_chain_digest =
            StringField(fields, "evidence_chain_digest");
        replay.evidence_uuid = StringField(fields, "evidence_uuid");
        replay.review_evidence_uuid =
            StringField(fields, "review_evidence_uuid");
        replay.compensation_evidence_uuid =
            StringField(fields, "compensation_evidence_uuid");
        replay.diagnostic_code = StringField(fields, "diagnostic_code");
        replay.review_required = BoolField(fields, "review_required");
        replay.review_approved = BoolField(fields, "review_approved");
        replay.compensation_required =
            BoolField(fields, "compensation_required");
        replay.compensation_attempted =
            BoolField(fields, "compensation_attempted");
        replay.retry_scheduled = BoolField(fields, "retry_scheduled");
        replay.cluster_route_requested =
            BoolField(fields, "cluster_route_requested");
        replay.external_cluster_provider_attested =
            BoolField(fields, "external_cluster_provider_attested");
        replay.parser_authority = BoolField(fields, "parser_authority");
        replay.client_authority = BoolField(fields, "client_authority");
        replay.reference_authority = BoolField(fields, "reference_authority");
        replay.wal_authority = BoolField(fields, "wal_authority");
        replay.benchmark_authority = BoolField(fields, "benchmark_authority");
        replay.optimizer_plan_authority =
            BoolField(fields, "optimizer_plan_authority");
        replay.index_finality_authority =
            BoolField(fields, "index_finality_authority");
        replay.provider_finality_authority =
            BoolField(fields, "provider_finality_authority");
        replay.memory_authority = BoolField(fields, "memory_authority");
        replay.agent_action_authority =
            BoolField(fields, "agent_action_authority");
        result.image.replay_records.push_back(std::move(replay));
      } else if (section == "health") {
        DurableAgentHealthRecord health;
        health.instance_uuid = StringField(fields, "instance_uuid");
        health.health_state = StringField(fields, "health_state");
        health.diagnostic_code = StringField(fields, "diagnostic_code");
        health.evidence_uuid = StringField(fields, "evidence_uuid");
        health.observed_at_microseconds = U64Field(fields, "observed_at_microseconds");
        result.image.health.push_back(std::move(health));
      } else if (section == "history") {
        DurableAgentHistoryRecord history;
        history.history_uuid = StringField(fields, "history_uuid");
        history.subject_uuid = StringField(fields, "subject_uuid");
        history.event_kind = StringField(fields, "event_kind");
        history.diagnostic_code = StringField(fields, "diagnostic_code");
        history.evidence_uuid = StringField(fields, "evidence_uuid");
        history.recorded_at_microseconds = U64Field(fields, "recorded_at_microseconds");
        result.image.retained_history.push_back(std::move(history));
      } else if (section == "migration") {
        DurableAgentStateMigrationRecord migration;
        migration.migration_uuid = StringField(fields, "migration_uuid");
        migration.from_schema_version = U64Field(fields, "from_schema_version");
        migration.to_schema_version = U64Field(fields, "to_schema_version");
        migration.result = StringField(fields, "result");
        migration.evidence_uuid = StringField(fields, "evidence_uuid");
        migration.source_root_digest = StringField(fields, "source_root_digest");
        migration.target_root_digest = StringField(fields, "target_root_digest");
        migration.recorded_at_microseconds =
            U64Field(fields, "recorded_at_microseconds");
        result.image.migrations.push_back(std::move(migration));
      }
    }
  } catch (...) {
    result.status = AgentError("SB_AGENT_CATALOG.IMAGE_FIELD_INVALID");
    return result;
  }

  if (header_schema_version < kDurableCatalogSchemaVersion ||
      result.image.schema_version < kDurableCatalogSchemaVersion) {
    result.image.schema_version = header_schema_version;
    const std::string migration_evidence =
        result.image.authority.evidence_uuid.empty()
            ? "agent_catalog_schema_migration"
            : result.image.authority.evidence_uuid;
    auto migrated = MigrateDurableAgentCatalogImageForProduction(
        std::move(result.image),
        migration_evidence,
        0);
    result.status = migrated.status;
    result.image = std::move(migrated.image);
    result.migrated = migrated.migrated;
  } else {
    result.status = production_live_path
                        ? ValidateDurableAgentCatalogForProduction(result.image)
                        : AgentOk();
  }
  if (result.status.ok) {
    result.status.diagnostic_code = "SB_AGENT_CATALOG.IMAGE_VALIDATED";
    result.status.detail = result.checksum;
  }
  return result;
}

AgentRuntimeStatus ValidateDurableAgentCatalogForProduction(
    const DurableAgentCatalogImage& image) {
  if (image.source != AgentCatalogStateSource::durable_catalog_image) {
    return AgentError("SB_AGENT_CATALOG.PRODUCTION_REQUIRES_DURABLE_CATALOG",
                      AgentCatalogStateSourceName(image.source));
  }
  if (image.schema_version != kDurableCatalogSchemaVersion) {
    return AgentError("SB_AGENT_CATALOG.SCHEMA_VERSION_UNSUPPORTED");
  }
  if (!image.authority.durable_catalog_authority ||
      !image.authority.mga_transaction_evidence ||
      image.authority.mga_transaction_uuid.empty() ||
      image.authority.evidence_uuid.empty() ||
      image.authority.transaction_generation == 0) {
    return AgentError("SB_AGENT_CATALOG.MGA_DURABLE_AUTHORITY_REQUIRED");
  }
  if (image.authority.database_uuid.empty() ||
      image.authority.catalog_storage_uuid.empty() ||
      image.authority.local_transaction_id == 0 ||
      image.authority.catalog_generation == 0 ||
      image.authority.storage_commit_evidence_uuid.empty() ||
      !image.authority.storage_catalog_record_evidence ||
      !image.authority.transaction_inventory_bound ||
      !image.authority.fsync_or_checkpoint_evidence) {
    return AgentError("SB_AGENT_CATALOG.STORAGE_MGA_AUTHORITY_REQUIRED");
  }
  if (image.authority.sidecar_storage || image.authority.in_memory_only) {
    return AgentError("SB_AGENT_CATALOG.SIDECAR_OR_MEMORY_AUTHORITY_REFUSED");
  }
  if (image.authority.catalog_root_digest.empty() ||
      image.authority.catalog_root_digest !=
          DurableAgentCatalogRootDigest(image)) {
    return AgentError("SB_AGENT_CATALOG.ROOT_DIGEST_MISMATCH");
  }
  for (const auto& replay : image.replay_records) {
    if (replay.replay_uuid.empty() ||
        replay.action_uuid.empty() ||
        replay.instance_uuid.empty() ||
        replay.operation_id.empty() ||
        replay.state == DurableAgentReplayState::none ||
        replay.replay_generation == 0 ||
        replay.recorded_at_microseconds == 0 ||
        replay.policy_digest.empty() ||
        replay.policy_generation == 0 ||
        replay.metric_digest.empty() ||
        replay.catalog_root_digest.empty() ||
        replay.security_digest.empty() ||
        replay.security_epoch == 0 ||
        replay.resource_reservation_digest.empty() ||
        replay.binary_package_digest.empty() ||
        replay.action_input_digest.empty() ||
        replay.action_evidence_digest.empty() ||
        replay.action_record_digest.empty() ||
        replay.evidence_chain_digest.empty() ||
        replay.evidence_uuid.empty() ||
        replay.diagnostic_code.empty()) {
      return AgentError("SB_AGENT_CATALOG.REPLAY_EVIDENCE_INVALID",
                        replay.replay_uuid);
    }
    if (replay.parser_authority || replay.client_authority ||
        replay.reference_authority || replay.wal_authority ||
        replay.benchmark_authority || replay.optimizer_plan_authority ||
        replay.index_finality_authority ||
        replay.provider_finality_authority || replay.memory_authority ||
        replay.agent_action_authority) {
      return AgentError("SB_AGENT_CATALOG.REPLAY_AUTHORITY_FORBIDDEN",
                        replay.replay_uuid);
    }
    if (replay.cluster_route_requested &&
        !replay.external_cluster_provider_attested) {
      return AgentError("SB_AGENT_CATALOG.REPLAY_CLUSTER_PROVIDER_REQUIRED",
                        replay.replay_uuid);
    }
    if (replay.state == DurableAgentReplayState::quarantine_released &&
        (replay.review_evidence_uuid.empty() || !replay.review_approved)) {
      return AgentError("SB_AGENT_CATALOG.QUARANTINE_REVIEW_REQUIRED",
                        replay.replay_uuid);
    }
  }
  for (const auto& migration : image.migrations) {
    if (migration.migration_uuid.empty() ||
        migration.result != "migrated" ||
        migration.evidence_uuid.empty() ||
        migration.to_schema_version != kDurableCatalogSchemaVersion ||
        migration.source_root_digest.empty() ||
        migration.target_root_digest.size() != 64 ||
        migration.target_root_digest == "bound_by_catalog_root_digest") {
      return AgentError("SB_AGENT_CATALOG.MIGRATION_EVIDENCE_INVALID");
    }
  }
  return {true, "SB_AGENT_CATALOG.PRODUCTION_AUTHORITY_ACCEPTED",
          image.authority.evidence_uuid};
}

AgentRuntimeStatus AcquireDurableAgentLease(DurableAgentCatalogImage* image,
                                            const DurableLeaseRequest& request) {
  if (image == nullptr) { return AgentError("SB_AGENT_LEASE.CATALOG_REQUIRED"); }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (request.lease_uuid.empty() || request.instance_uuid.empty() ||
      request.owner_uuid.empty() || request.lease_duration_microseconds == 0 ||
      request.evidence_uuid.empty()) {
    return AgentError("SB_AGENT_LEASE.REQUEST_EVIDENCE_REQUIRED");
  }

  for (auto& lease : image->leases) {
    if (lease.lease_uuid != request.lease_uuid) { continue; }
    if (IsLiveLease(lease, request.now_microseconds) &&
        lease.owner_uuid != request.owner_uuid) {
      return AgentError("SB_AGENT_LEASE.DUPLICATE_LIVE_OWNER_REFUSED",
                        lease.owner_uuid);
    }
    if (IsLiveLease(lease, request.now_microseconds) &&
        lease.owner_uuid == request.owner_uuid) {
      lease.expires_at_microseconds =
          request.now_microseconds + request.lease_duration_microseconds;
      lease.evidence_uuid = request.evidence_uuid;
      RecordHistory(image, lease.lease_uuid, "lease_idempotent_acquire",
                    "SB_AGENT_LEASE.IDEMPOTENT_OWNER", request.evidence_uuid,
                    request.now_microseconds);
      const auto refresh =
          RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
      if (!refresh.ok) { return refresh; }
      return {true, "SB_AGENT_LEASE.IDEMPOTENT_OWNER", lease.lease_uuid};
    }
    lease.instance_uuid = request.instance_uuid;
    lease.owner_uuid = request.owner_uuid;
    lease.state = DurableAgentLeaseState::acquired;
    lease.acquired_at_microseconds = request.now_microseconds;
    lease.expires_at_microseconds =
        request.now_microseconds + request.lease_duration_microseconds;
    lease.evidence_uuid = request.evidence_uuid;
    ++lease.replay_generation;
    RecordHistory(image, lease.lease_uuid, "lease_acquired",
                  "SB_AGENT_LEASE.ACQUIRED", request.evidence_uuid,
                  request.now_microseconds);
    const auto refresh =
        RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
    if (!refresh.ok) { return refresh; }
    return {true, "SB_AGENT_LEASE.ACQUIRED", lease.lease_uuid};
  }

  DurableAgentLeaseRecord lease;
  lease.lease_uuid = request.lease_uuid;
  lease.instance_uuid = request.instance_uuid;
  lease.owner_uuid = request.owner_uuid;
  lease.state = DurableAgentLeaseState::acquired;
  lease.acquired_at_microseconds = request.now_microseconds;
  lease.expires_at_microseconds =
      request.now_microseconds + request.lease_duration_microseconds;
  lease.evidence_uuid = request.evidence_uuid;
  image->leases.push_back(std::move(lease));
  RecordHistory(image, request.lease_uuid, "lease_acquired",
                "SB_AGENT_LEASE.ACQUIRED", request.evidence_uuid,
                request.now_microseconds);
  const auto refresh =
      RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
  if (!refresh.ok) { return refresh; }
  return {true, "SB_AGENT_LEASE.ACQUIRED", request.lease_uuid};
}

AgentRuntimeStatus HeartbeatDurableAgentLease(DurableAgentCatalogImage* image,
                                              const DurableLeaseRequest& request) {
  if (image == nullptr) { return AgentError("SB_AGENT_LEASE.CATALOG_REQUIRED"); }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  for (auto& lease : image->leases) {
    if (lease.lease_uuid != request.lease_uuid) { continue; }
    if (lease.owner_uuid != request.owner_uuid) {
      return AgentError("SB_AGENT_LEASE.OWNER_MISMATCH", lease.owner_uuid);
    }
    if (!IsLiveLease(lease, request.now_microseconds)) {
      lease.state = DurableAgentLeaseState::expired;
      const auto refresh =
          RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
      if (!refresh.ok) { return refresh; }
      return AgentError("SB_AGENT_LEASE.EXPIRED", lease.lease_uuid);
    }
    ++lease.heartbeat_generation;
    lease.last_heartbeat_microseconds = request.now_microseconds;
    if (request.lease_duration_microseconds != 0) {
      lease.expires_at_microseconds =
          request.now_microseconds + request.lease_duration_microseconds;
    }
    lease.evidence_uuid = request.evidence_uuid;
    RecordHistory(image, lease.lease_uuid, "lease_heartbeat",
                  "SB_AGENT_LEASE.HEARTBEAT_PERSISTED", request.evidence_uuid,
                  request.now_microseconds);
    const auto refresh =
        RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
    if (!refresh.ok) { return refresh; }
    return {true, "SB_AGENT_LEASE.HEARTBEAT_PERSISTED",
            std::to_string(lease.heartbeat_generation)};
  }
  return AgentError("SB_AGENT_LEASE.NOT_FOUND", request.lease_uuid);
}

AgentRuntimeStatus CancelDurableAgentLease(DurableAgentCatalogImage* image,
                                           const DurableLeaseRequest& request,
                                           DurableAgentLeaseState terminal_state) {
  if (image == nullptr) { return AgentError("SB_AGENT_LEASE.CATALOG_REQUIRED"); }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (terminal_state != DurableAgentLeaseState::cancelled &&
      terminal_state != DurableAgentLeaseState::draining &&
      terminal_state != DurableAgentLeaseState::quarantined &&
      terminal_state != DurableAgentLeaseState::replay_pending) {
    return AgentError("SB_AGENT_LEASE.INVALID_TERMINAL_STATE");
  }
  for (auto& lease : image->leases) {
    if (lease.lease_uuid != request.lease_uuid) { continue; }
    if (!request.owner_uuid.empty() && lease.owner_uuid != request.owner_uuid) {
      return AgentError("SB_AGENT_LEASE.OWNER_MISMATCH", lease.owner_uuid);
    }
    lease.state = terminal_state;
    lease.expires_at_microseconds = request.now_microseconds;
    lease.evidence_uuid = request.evidence_uuid;
    RecordHistory(image, lease.lease_uuid, DurableAgentLeaseStateName(terminal_state),
                  "SB_AGENT_LEASE.STATE_PERSISTED", request.evidence_uuid,
                  request.now_microseconds);
    const auto refresh =
        RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
    if (!refresh.ok) { return refresh; }
    return {true, "SB_AGENT_LEASE.STATE_PERSISTED",
            DurableAgentLeaseStateName(terminal_state)};
  }
  return AgentError("SB_AGENT_LEASE.NOT_FOUND", request.lease_uuid);
}

AgentRuntimeStatus AcquireDurableAgentResourceReservation(
    DurableAgentCatalogImage* image,
    const DurableAgentResourceReservationRequest& request) {
  if (image == nullptr) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.CATALOG_REQUIRED");
  }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (request.reservation_uuid.empty() || request.reservation_key.empty() ||
      request.owner_scope.empty() || request.agent_type_id.empty() ||
      request.operation_id.empty() || request.evidence_uuid.empty()) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.REQUEST_EVIDENCE_REQUIRED");
  }
  if (request.memory_bytes == 0 || request.worker_slots == 0 ||
      request.overhead_microseconds == 0) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.EMPTY_REQUEST_REFUSED");
  }
  if (request.max_active_reservations == 0 || request.max_memory_bytes == 0 ||
      request.max_worker_slots == 0 || request.max_overhead_microseconds == 0) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.UNBOUNDED_LIMIT_REFUSED");
  }

  u64 active_count = 0;
  u64 active_memory = 0;
  u64 active_workers = 0;
  u64 active_overhead = 0;
  for (const auto& reservation : image->resource_reservations) {
    if (!IsActiveReservation(reservation)) { continue; }
    ++active_count;
    active_memory += reservation.memory_bytes;
    active_workers += reservation.worker_slots;
    active_overhead += reservation.overhead_microseconds;
    if (reservation.reservation_key == request.reservation_key) {
      return AgentError("SB_AGENT_RESOURCE_RESERVATION.DUPLICATE",
                        request.reservation_key);
    }
  }
  if (active_count + 1 > request.max_active_reservations) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.ACTIVE_OVER_BUDGET",
                      std::to_string(request.max_active_reservations));
  }
  if (active_memory > request.max_memory_bytes ||
      request.memory_bytes > request.max_memory_bytes - active_memory) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.MEMORY_OVER_BUDGET",
                      std::to_string(request.memory_bytes));
  }
  if (active_workers > request.max_worker_slots ||
      request.worker_slots > request.max_worker_slots - active_workers) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.WORKER_OVER_BUDGET",
                      std::to_string(request.worker_slots));
  }
  if (active_overhead > request.max_overhead_microseconds ||
      request.overhead_microseconds >
          request.max_overhead_microseconds - active_overhead) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.OVERHEAD_OVER_BUDGET",
                      std::to_string(request.overhead_microseconds));
  }

  DurableAgentResourceReservationRecord record;
  record.reservation_uuid = request.reservation_uuid;
  record.reservation_key = request.reservation_key;
  record.owner_scope = request.owner_scope;
  record.agent_type_id = request.agent_type_id;
  record.operation_id = request.operation_id;
  record.state = DurableAgentResourceReservationState::acquired;
  record.acquired_at_microseconds = request.now_microseconds;
  record.memory_bytes = request.memory_bytes;
  record.worker_slots = request.worker_slots;
  record.overhead_microseconds = request.overhead_microseconds;
  record.evidence_uuid = request.evidence_uuid;
  image->resource_reservations.push_back(std::move(record));
  RecordHistory(image, request.reservation_uuid, "resource_reservation_acquired",
                "SB_AGENT_RESOURCE_RESERVATION.ACQUIRED",
                request.evidence_uuid, request.now_microseconds);
  const auto refresh =
      RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
  if (!refresh.ok) { return refresh; }
  return {true, "SB_AGENT_RESOURCE_RESERVATION.ACQUIRED",
          request.reservation_uuid};
}

AgentRuntimeStatus ReleaseDurableAgentResourceReservation(
    DurableAgentCatalogImage* image,
    const std::string& reservation_uuid,
    const std::string& release_evidence_uuid,
    u64 now_microseconds,
    DurableAgentResourceReservationState terminal_state) {
  if (image == nullptr) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.CATALOG_REQUIRED");
  }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (reservation_uuid.empty() || release_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.RELEASE_EVIDENCE_REQUIRED");
  }
  if (terminal_state == DurableAgentResourceReservationState::acquired) {
    return AgentError("SB_AGENT_RESOURCE_RESERVATION.INVALID_TERMINAL_STATE");
  }
  for (auto& reservation : image->resource_reservations) {
    if (reservation.reservation_uuid != reservation_uuid) { continue; }
    if (!IsActiveReservation(reservation)) {
      return {true, "SB_AGENT_RESOURCE_RESERVATION.RELEASE_IDEMPOTENT",
              reservation_uuid};
    }
    reservation.state = terminal_state;
    reservation.released_at_microseconds = now_microseconds;
    reservation.release_evidence_uuid = release_evidence_uuid;
    reservation.release_reason =
        DurableAgentResourceReservationStateName(terminal_state);
    RecordHistory(image, reservation_uuid, "resource_reservation_released",
                  "SB_AGENT_RESOURCE_RESERVATION.RELEASED",
                  release_evidence_uuid, now_microseconds);
    const auto refresh =
        RefreshDurableAgentCatalogAuthorityDigest(image, release_evidence_uuid);
    if (!refresh.ok) { return refresh; }
    return {true, "SB_AGENT_RESOURCE_RESERVATION.RELEASED", reservation_uuid};
  }
  return AgentError("SB_AGENT_RESOURCE_RESERVATION.NOT_FOUND",
                    reservation_uuid);
}

AgentRuntimeStatus RecordDurableAgentApproval(
    DurableAgentCatalogImage* image,
    const DurableAgentApprovalRequest& request) {
  if (image == nullptr) { return AgentError("SB_AGENT_APPROVAL.CATALOG_REQUIRED"); }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (request.approval_uuid.empty() || request.action_uuid.empty() ||
      request.principal_uuid.empty() || request.evidence_uuid.empty() ||
      request.approved_at_microseconds == 0) {
    return AgentError("SB_AGENT_APPROVAL.REQUEST_EVIDENCE_REQUIRED");
  }

  const auto action = std::find_if(
      image->actions.begin(),
      image->actions.end(),
      [&request](const DurableAgentActionRecord& record) {
        return record.action_uuid == request.action_uuid;
      });
  if (action == image->actions.end()) {
    return AgentError("SB_AGENT_APPROVAL.ACTION_NOT_FOUND",
                      request.action_uuid);
  }
  if (action->state == DurableAgentActionState::completed ||
      action->state == DurableAgentActionState::cancelled ||
      action->state == DurableAgentActionState::quarantined) {
    return AgentError("SB_AGENT_APPROVAL.TERMINAL_ACTION_REFUSED",
                      request.action_uuid);
  }

  for (const auto& approval : image->approvals) {
    if (approval.action_uuid != request.action_uuid) { continue; }
    if (approval.approved != request.approved) {
      return AgentError("SB_AGENT_APPROVAL.CONFLICTING_DECISION_REFUSED",
                        request.action_uuid);
    }
    if (approval.approval_uuid == request.approval_uuid ||
        approval.principal_uuid == request.principal_uuid) {
      return {true, "SB_AGENT_APPROVAL.IDEMPOTENT", approval.approval_uuid};
    }
  }

  DurableAgentApprovalRecord approval;
  approval.approval_uuid = request.approval_uuid;
  approval.action_uuid = request.action_uuid;
  approval.principal_uuid = request.principal_uuid;
  approval.evidence_uuid = request.evidence_uuid;
  approval.approved_at_microseconds = request.approved_at_microseconds;
  approval.approved = request.approved;
  image->approvals.push_back(std::move(approval));
  RecordHistory(image, request.action_uuid, "action_approval_recorded",
                request.approved ? "SB_AGENT_APPROVAL.APPROVED"
                                 : "SB_AGENT_APPROVAL.DENIED",
                request.evidence_uuid, request.approved_at_microseconds);
  const auto refresh =
      RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
  if (!refresh.ok) { return refresh; }
  return {true,
          request.approved ? "SB_AGENT_APPROVAL.APPROVED"
                           : "SB_AGENT_APPROVAL.DENIED",
          request.approval_uuid};
}

AgentRuntimeStatus CancelDurableAgentAction(
    DurableAgentCatalogImage* image,
    const DurableAgentActionCancellationRequest& request) {
  if (image == nullptr) { return AgentError("SB_AGENT_ACTION_CANCEL.CATALOG_REQUIRED"); }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (request.action_uuid.empty() || request.principal_uuid.empty() ||
      request.evidence_uuid.empty() || request.cancelled_at_microseconds == 0 ||
      !request.operator_approved) {
    return AgentError("SB_AGENT_ACTION_CANCEL.REQUEST_EVIDENCE_REQUIRED");
  }

  for (auto& action : image->actions) {
    if (action.action_uuid != request.action_uuid) { continue; }
    if (action.state == DurableAgentActionState::cancelled) {
      return {true, "SB_AGENT_ACTION_CANCEL.IDEMPOTENT", request.action_uuid};
    }
    if (action.state == DurableAgentActionState::completed) {
      return AgentError("SB_AGENT_ACTION_CANCEL.COMPLETED_ACTION_REFUSED",
                        request.action_uuid);
    }
    action.state = DurableAgentActionState::cancelled;
    action.owner_uuid = request.principal_uuid;
    action.evidence_uuid = request.evidence_uuid;
    action.diagnostic_code = "SB_AGENT_ACTION_CANCEL.CANCELLED";
    action.outcome_verified = false;
    ++action.generation;
    RecordHistory(image, request.action_uuid, "action_cancelled",
                  "SB_AGENT_ACTION_CANCEL.CANCELLED", request.evidence_uuid,
                  request.cancelled_at_microseconds);
    const auto refresh =
        RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
    if (!refresh.ok) { return refresh; }
    return {true, "SB_AGENT_ACTION_CANCEL.CANCELLED", request.action_uuid};
  }
  return AgentError("SB_AGENT_ACTION_CANCEL.ACTION_NOT_FOUND",
                    request.action_uuid);
}

AgentRuntimeStatus ApplyDurableAgentPolicyUpdate(
    DurableAgentCatalogImage* image,
    const DurableAgentPolicyUpdateRequest& request) {
  if (image == nullptr) { return AgentError("SB_AGENT_POLICY_UPDATE.CATALOG_REQUIRED"); }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (request.agent_type_id.empty() || request.principal_uuid.empty() ||
      request.evidence_uuid.empty() || request.updated_at_microseconds == 0 ||
      !request.operator_approved) {
    return AgentError("SB_AGENT_POLICY_UPDATE.REQUEST_EVIDENCE_REQUIRED");
  }
  const auto descriptor = FindAgentType(request.agent_type_id);
  if (!descriptor.has_value()) {
    return AgentError("SB_AGENT_POLICY_UPDATE.AGENT_DESCRIPTOR_REQUIRED",
                      request.agent_type_id);
  }
  const auto policy_status = ValidateAgentPolicy(request.policy, *descriptor);
  if (!policy_status.ok) { return policy_status; }

  auto policy = std::find_if(
      image->policies.begin(),
      image->policies.end(),
      [&request](const AgentPolicy& candidate) {
        return candidate.policy_uuid == request.policy.policy_uuid;
      });
  const u64 previous_generation =
      policy == image->policies.end() ? 0 : policy->policy_generation;
  if (request.expected_previous_generation != previous_generation) {
    return AgentError("SB_AGENT_POLICY_UPDATE.GENERATION_CONFLICT",
                      std::to_string(previous_generation));
  }
  if (request.policy.policy_generation <= previous_generation) {
    return AgentError("SB_AGENT_POLICY_UPDATE.NON_MONOTONIC_GENERATION",
                      std::to_string(request.policy.policy_generation));
  }

  if (policy == image->policies.end()) {
    image->policies.push_back(request.policy);
  } else {
    *policy = request.policy;
  }
  for (auto& attachment : image->attachments) {
    if (attachment.policy_uuid != request.policy.policy_uuid) { continue; }
    attachment.policy_generation = request.policy.policy_generation;
    attachment.attachment_generation =
        std::max(attachment.attachment_generation,
                 request.policy.policy_generation);
    attachment.evidence_uuid = request.evidence_uuid;
  }
  for (auto& instance : image->instances) {
    if (instance.agent_type_id != request.agent_type_id ||
        instance.policy_uuid != request.policy.policy_uuid) {
      continue;
    }
    instance.policy_generation = request.policy.policy_generation;
  }
  RecordHistory(image, request.policy.policy_uuid, "policy_generation_updated",
                "SB_AGENT_POLICY_UPDATE.APPLIED", request.evidence_uuid,
                request.updated_at_microseconds);
  const auto refresh =
      RefreshDurableAgentCatalogAuthorityDigest(image, request.evidence_uuid);
  if (!refresh.ok) { return refresh; }
  return {true, "SB_AGENT_POLICY_UPDATE.APPLIED",
          std::to_string(request.policy.policy_generation)};
}

AgentRuntimeStatus RecoverDurableAgentCatalogAfterCrash(DurableAgentCatalogImage* image,
                                                        u64 now_microseconds,
                                                        const std::string& evidence_uuid) {
  if (image == nullptr) { return AgentError("SB_AGENT_RECOVERY.CATALOG_REQUIRED"); }
  const auto authority = ValidateDurableAgentCatalogForProduction(*image);
  if (!authority.ok) { return authority; }
  if (evidence_uuid.empty()) {
    return AgentError("SB_AGENT_RECOVERY.EVIDENCE_REQUIRED");
  }

  u64 replayed = 0;
  for (auto& lease : image->leases) {
    if (lease.state == DurableAgentLeaseState::acquired ||
        lease.state == DurableAgentLeaseState::draining) {
      lease.state = DurableAgentLeaseState::replay_pending;
      lease.expires_at_microseconds = now_microseconds;
      lease.evidence_uuid = evidence_uuid;
      ++lease.replay_generation;
      ++replayed;
      RecordHistory(image, lease.lease_uuid, "lease_replay_pending",
                    "SB_AGENT_RECOVERY.LEASE_REPLAY_PENDING", evidence_uuid,
                    now_microseconds);
    }
  }
  for (auto& action : image->actions) {
    if (action.state == DurableAgentActionState::pending ||
        action.state == DurableAgentActionState::running) {
      action.state = DurableAgentActionState::replay_pending;
      action.evidence_uuid = evidence_uuid;
      ++action.generation;
      ++replayed;
      RecordHistory(image, action.action_uuid, "action_replay_pending",
                    "SB_AGENT_RECOVERY.ACTION_REPLAY_PENDING", evidence_uuid,
                    now_microseconds);
    }
  }
  for (auto& reservation : image->resource_reservations) {
    if (reservation.state == DurableAgentResourceReservationState::acquired) {
      reservation.state = DurableAgentResourceReservationState::cancelled;
      reservation.released_at_microseconds = now_microseconds;
      reservation.release_evidence_uuid = evidence_uuid;
      reservation.release_reason = "crash_recovery";
      ++replayed;
      RecordHistory(image, reservation.reservation_uuid,
                    "resource_reservation_crash_released",
                    "SB_AGENT_RECOVERY.RESOURCE_RESERVATION_RELEASED",
                    evidence_uuid, now_microseconds);
    }
  }
  const auto refresh =
      RefreshDurableAgentCatalogAuthorityDigest(image, evidence_uuid);
  if (!refresh.ok) { return refresh; }
  return {true, "SB_AGENT_RECOVERY.REPLAY_DETERMINISTIC", std::to_string(replayed)};
}

}  // namespace scratchbird::core::agents
