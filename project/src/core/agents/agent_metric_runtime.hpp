// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_STRICT_METRIC_SNAPSHOT_INTEGRATION
// SEARCH_KEY: ARHC_AGENT_RESOURCE_BUDGET_WORKER_CAPACITY
// Runtime-only support for strict agent metric snapshots and bounded agent
// work reservations. These records are admission/evidence controls only; they
// are not transaction finality, visibility, recovery, parser, donor, or client
// authority.

#include "agent_runtime.hpp"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentMetricRuntimeMode {
  production_strict,
  test_probe_relaxed_registry_only
};

struct AgentObservedMetricSnapshot {
  std::string metric_family;
  std::string namespace_path;
  std::string source_id;
  u64 generation = 0;
  u64 source_sequence = 0;
  u64 previous_source_sequence = 0;
  u64 observed_wall_microseconds = 0;
  std::string scope_uuid;
  std::string digest;
  std::string value_digest;
  std::string schema_digest;
  AgentMetricSourceQuality source_quality = AgentMetricSourceQuality::unknown;
  bool present = true;
  bool trusted = false;
  bool schema_compatible = true;
  bool attestation_verified = false;
  bool gap_detected = false;
  bool replay_detected = false;
  bool disagreement_detected = false;
  bool redacted = false;
  bool protected_material_present = false;
  bool external_provider_attested = false;
  std::string trust_provenance;
  std::string provenance_record;
  std::string attestation_key_id;
  std::string attestation_digest;
  std::string evidence_uuid;
  std::string snapshot_id;
  std::vector<std::string> authority_claims;
};

struct AgentMetricSnapshotEvaluationOptions {
  AgentMetricRuntimeMode mode = AgentMetricRuntimeMode::production_strict;
  std::string expected_scope_uuid;
  bool allow_optional_missing = true;
  u64 required_source_quorum = 2;
  bool require_schema_digest = true;
  bool require_source_attestation = true;
  bool require_redaction_and_provenance = true;
  bool fail_on_gap_replay_or_disagreement = true;
  const scratchbird::core::metrics::MetricRegistry* registry = nullptr;
};

struct AgentMetricSnapshotDiagnostic {
  std::string diagnostic_code;
  std::string metric_family;
  std::string evidence_uuid;
  std::string snapshot_id;
  std::string source_id;
  std::string detail;
  bool failed_closed = true;
};

struct AgentMetricSnapshotEvaluation {
  AgentRuntimeStatus status;
  bool accepted = false;
  bool failed_closed = true;
  bool relaxed_registry_only = false;
  bool optional_suppressed = false;
  u64 required_source_quorum = 0;
  u64 observed_source_quorum = 0;
  std::string input_digest;
  std::vector<AgentMetricSnapshotDiagnostic> diagnostics;
};

AgentMetricSnapshotEvaluation EvaluateAgentObservedMetricSnapshots(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    const std::vector<AgentObservedMetricSnapshot>& snapshots,
    const AgentMetricSnapshotEvaluationOptions& options = {});

enum class AgentResourceReservationReleaseReason {
  release,
  cancellation,
  timeout,
  shutdown
};

struct AgentResourceReservationLimits {
  u64 max_memory_bytes = 0;
  u64 max_worker_slots = 0;
  u64 foreground_reserved_worker_slots = 0;
  u64 max_overhead_microseconds = 0;
};

struct AgentResourceReservationRequest {
  std::string reservation_key;
  std::string owner_scope;
  u64 memory_bytes = 0;
  u64 worker_slots = 0;
  u64 overhead_microseconds = 0;
  bool foreground_database_work_active = false;
  bool cancellation_requested = false;
  bool protect_foreground_work = true;
};

struct AgentResourceReservationToken {
  std::string token_id;
  std::string reservation_key;
  std::string owner_scope;
  std::string agent_type_id;
  u64 memory_bytes = 0;
  u64 worker_slots = 0;
  u64 overhead_microseconds = 0;
  bool active = false;
};

struct AgentResourceReservationSnapshot {
  std::string ledger_id;
  u64 active_reservation_count = 0;
  u64 created_reservation_count = 0;
  u64 released_reservation_count = 0;
  u64 active_memory_bytes = 0;
  u64 active_worker_slots = 0;
  u64 active_overhead_microseconds = 0;
  u64 foreground_reserved_worker_slots = 0;
  u64 background_worker_slots = 0;
};

struct AgentResourceReservationResult {
  AgentRuntimeStatus status;
  AgentResourceReservationToken reservation;
  AgentResourceReservationSnapshot snapshot;
  std::string diagnostic_code;
  std::string evidence_uuid;
  bool ok = false;
  bool reservation_created = false;
  bool released = false;
  bool idempotent = false;
  bool failed_closed = true;
  std::vector<std::string> evidence;
};

class AgentResourceReservationLedger {
 public:
  AgentResourceReservationLedger(std::string ledger_id,
                                 AgentResourceReservationLimits limits);

  AgentResourceReservationResult Acquire(
      const AgentTypeDescriptor& descriptor,
      const AgentPolicy& policy,
      const AgentRuntimeContext& context,
      const AgentResourceReservationRequest& request);
  AgentResourceReservationResult Release(
      const std::string& token_id,
      AgentResourceReservationReleaseReason reason =
          AgentResourceReservationReleaseReason::release);
  AgentResourceReservationResult CancelOwnerReservations(
      const std::string& owner_scope);
  AgentResourceReservationSnapshot Snapshot() const;

 private:
  AgentResourceReservationSnapshot SnapshotLocked() const;
  AgentResourceReservationResult RefuseLocked(
      const std::string& agent_type_id,
      const AgentResourceReservationRequest& request,
      const std::string& code,
      const std::string& detail) const;
  AgentResourceReservationResult ReleaseLocked(
      const std::string& token_id,
      AgentResourceReservationReleaseReason reason);

  std::string ledger_id_;
  AgentResourceReservationLimits limits_;
  mutable std::mutex mutex_;
  std::map<std::string, AgentResourceReservationToken> active_;
  std::map<std::string, std::string> active_by_reservation_key_;
  std::set<std::string> released_tokens_;
  u64 created_count_ = 0;
  u64 released_count_ = 0;
  u64 active_memory_bytes_ = 0;
  u64 active_worker_slots_ = 0;
  u64 active_overhead_microseconds_ = 0;
};

}  // namespace scratchbird::core::agents
