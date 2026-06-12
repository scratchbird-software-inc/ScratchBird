// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_metric_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string ExpectedScopeUuid(const AgentRuntimeContext& context,
                              const AgentMetricDependency& dependency,
                              const AgentMetricSnapshotEvaluationOptions& options) {
  if (!options.expected_scope_uuid.empty()) {
    return options.expected_scope_uuid;
  }
  if (dependency.cluster_only && !context.cluster_uuid.empty()) {
    return context.cluster_uuid;
  }
  return context.database_uuid;
}

bool SourceQualitySatisfies(AgentMetricSourceQuality observed,
                            AgentMetricSourceQuality required) {
  if (observed == AgentMetricSourceQuality::unknown) {
    return false;
  }
  if (required == AgentMetricSourceQuality::unknown) {
    return true;
  }
  if (required == AgentMetricSourceQuality::trusted) {
    return observed == AgentMetricSourceQuality::trusted ||
           observed == AgentMetricSourceQuality::cluster_confirmed;
  }
  return observed == AgentMetricSourceQuality::cluster_confirmed;
}

std::vector<const AgentObservedMetricSnapshot*> FindSnapshots(
    const std::vector<AgentObservedMetricSnapshot>& snapshots,
    const std::string& metric_family) {
  std::vector<const AgentObservedMetricSnapshot*> found;
  for (const auto& snapshot : snapshots) {
    if (snapshot.metric_family == metric_family) {
      found.push_back(&snapshot);
    }
  }
  return found;
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool IsForbiddenAuthorityClaim(const std::string& claim) {
  static const std::set<std::string> forbidden = {
      "transaction",
      "transaction_finality",
      "finality",
      "visibility",
      "authorization",
      "authorization_security",
      "security",
      "recovery",
      "parser",
      "reference",
      "wal",
      "benchmark",
      "optimizer_plan",
      "index_finality",
      "provider_finality",
      "cluster",
      "memory",
      "agent_action"};
  return forbidden.find(LowerAscii(claim)) != forbidden.end();
}

std::string SnapshotValueDigest(const AgentObservedMetricSnapshot& snapshot) {
  return snapshot.value_digest.empty() ? snapshot.digest : snapshot.value_digest;
}

u64 EffectiveRequiredSourceQuorum(
    const AgentMetricSnapshotEvaluationOptions& options) {
  return options.required_source_quorum == 0 ? 1 : options.required_source_quorum;
}

std::string SnapshotInputDigest(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    const std::vector<AgentObservedMetricSnapshot>& snapshots) {
  std::vector<AgentObservedMetricSnapshot> sorted = snapshots;
  std::sort(sorted.begin(), sorted.end(),
            [](const AgentObservedMetricSnapshot& left,
               const AgentObservedMetricSnapshot& right) {
              if (left.metric_family != right.metric_family) {
                return left.metric_family < right.metric_family;
              }
              if (left.source_id != right.source_id) {
                return left.source_id < right.source_id;
              }
              return left.snapshot_id < right.snapshot_id;
            });
  std::ostringstream key;
  key << "agent_metric_snapshot_input|" << descriptor.type_id << '|'
      << context.database_uuid << '|';
  for (const auto& snapshot : sorted) {
    std::vector<std::string> authority_claims = snapshot.authority_claims;
    std::sort(authority_claims.begin(), authority_claims.end());
    key << snapshot.metric_family << ',' << snapshot.namespace_path << ','
        << snapshot.source_id << ',' << snapshot.generation << ','
        << snapshot.source_sequence << ',' << snapshot.previous_source_sequence
        << ',' << snapshot.observed_wall_microseconds << ','
        << snapshot.scope_uuid << ',' << snapshot.digest << ','
        << snapshot.value_digest << ',' << snapshot.schema_digest << ','
        << static_cast<int>(snapshot.source_quality) << ','
        << (snapshot.trusted ? "trusted" : "untrusted") << ','
        << (snapshot.attestation_verified ? "attested" : "unattested") << ','
        << (snapshot.gap_detected ? "gap" : "no_gap") << ','
        << (snapshot.replay_detected ? "replay" : "no_replay") << ','
        << (snapshot.disagreement_detected ? "disagree" : "agree") << ','
        << (snapshot.redacted ? "redacted" : "unredacted") << ','
        << (snapshot.protected_material_present ? "protected_present"
                                                : "protected_absent")
        << ',' << (snapshot.external_provider_attested ? "external_provider"
                                                       : "local_provider")
        << ',' << snapshot.trust_provenance << ','
        << snapshot.provenance_record << ',' << snapshot.attestation_key_id
        << ',' << snapshot.attestation_digest << ',' << snapshot.snapshot_id;
    for (const auto& claim : authority_claims) {
      key << ",authority_claim=" << claim;
    }
    key << ';';
  }
  return DeterministicAgentRuntimeObjectUuidFromKey(key.str());
}

AgentMetricSnapshotDiagnostic MetricDiagnostic(
    const AgentMetricDependency& dependency,
    const AgentObservedMetricSnapshot* snapshot,
    std::string code,
    std::string detail,
    bool failed_closed = true) {
  AgentMetricSnapshotDiagnostic diagnostic;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.metric_family = dependency.metric_family;
  diagnostic.detail = std::move(detail);
  diagnostic.failed_closed = failed_closed;
  if (snapshot != nullptr) {
    diagnostic.evidence_uuid = snapshot->evidence_uuid;
    diagnostic.snapshot_id = snapshot->snapshot_id;
    diagnostic.source_id = snapshot->source_id;
  }
  if (diagnostic.evidence_uuid.empty()) {
    diagnostic.evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
        "agent_metric_snapshot|" + dependency.metric_family + "|" +
        diagnostic.diagnostic_code);
  }
  return diagnostic;
}

AgentMetricSnapshotEvaluation RefuseMetric(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    const std::vector<AgentObservedMetricSnapshot>& snapshots,
    const AgentMetricDependency& dependency,
    const AgentObservedMetricSnapshot* snapshot,
    std::string code,
    std::string detail,
    u64 required_source_quorum = 0,
    u64 observed_source_quorum = 0) {
  AgentMetricSnapshotEvaluation evaluation;
  evaluation.status = AgentError(code, detail);
  evaluation.accepted = false;
  evaluation.failed_closed = true;
  evaluation.required_source_quorum = required_source_quorum;
  evaluation.observed_source_quorum = observed_source_quorum;
  evaluation.input_digest = SnapshotInputDigest(descriptor, context, snapshots);
  evaluation.diagnostics.push_back(
      MetricDiagnostic(dependency, snapshot, std::move(code), std::move(detail)));
  return evaluation;
}

bool AdditionExceeds(u64 current, u64 add, u64 limit) {
  return limit != 0 && add > limit - std::min(current, limit);
}

std::string ReservationEvidenceUuid(const std::string& ledger_id,
                                    const std::string& agent_type_id,
                                    const std::string& subject,
                                    const std::string& code) {
  return DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_resource_reservation|" + ledger_id + "|" + agent_type_id + "|" +
      subject + "|" + code);
}

std::vector<std::string> ReservationEvidence(const std::string& code) {
  return {
      "diagnostic_code=" + code,
      "agent_resource_runtime_authority=engine_runtime",
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "recovery_authority=false",
      "parser_authority=false",
      "reference_authority=false",
      "client_authority=false"};
}

}  // namespace

AgentMetricSnapshotEvaluation EvaluateAgentObservedMetricSnapshots(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    const std::vector<AgentObservedMetricSnapshot>& snapshots,
    const AgentMetricSnapshotEvaluationOptions& options) {
  AgentMetricSnapshotEvaluation evaluation;
  evaluation.input_digest = SnapshotInputDigest(descriptor, context, snapshots);

  if (snapshots.empty() &&
      options.mode == AgentMetricRuntimeMode::test_probe_relaxed_registry_only &&
      options.registry != nullptr) {
    const auto status =
        ResolveAgentMetricDependencies(descriptor, context, *options.registry);
    evaluation.status = status.ok
        ? AgentRuntimeStatus{true,
                             "SB_AGENT_METRIC_SNAPSHOT.RELAXED_TEST_PROBE_ONLY",
                             descriptor.type_id}
        : status;
    evaluation.accepted = status.ok;
    evaluation.failed_closed = !status.ok;
    evaluation.relaxed_registry_only = true;
    AgentMetricSnapshotDiagnostic diagnostic;
    diagnostic.diagnostic_code =
        "SB_AGENT_METRIC_SNAPSHOT.RELAXED_TEST_PROBE_ONLY";
    diagnostic.detail = "registry_only_test_probe_mode";
    diagnostic.failed_closed = false;
    diagnostic.evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
        "agent_metric_snapshot_relaxed|" + descriptor.type_id);
    evaluation.diagnostics.push_back(std::move(diagnostic));
    return evaluation;
  }

  bool local_dependency_seen = false;
  bool cluster_dependency_seen = false;
  bool cluster_path_failed_closed = false;
  const u64 required_source_quorum = EffectiveRequiredSourceQuorum(options);
  evaluation.required_source_quorum = required_source_quorum;
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (dependency.cluster_only && !context.cluster_authority_available) {
      cluster_dependency_seen = true;
      cluster_path_failed_closed = true;
      evaluation.optional_suppressed = true;
      evaluation.diagnostics.push_back(MetricDiagnostic(
          dependency, nullptr,
          "SB_AGENT_METRIC_SNAPSHOT.CLUSTER_PATH_SUPPRESSED",
          "cluster metric path unavailable in local runtime", false));
      continue;
    }
    if (dependency.cluster_only) {
      cluster_dependency_seen = true;
    } else {
      local_dependency_seen = true;
    }

    const auto matching_snapshots = FindSnapshots(snapshots, dependency.metric_family);
    if (matching_snapshots.empty()) {
      if (!dependency.required && options.allow_optional_missing) {
        evaluation.optional_suppressed = true;
        evaluation.diagnostics.push_back(MetricDiagnostic(
            dependency, nullptr,
            "SB_AGENT_METRIC_SNAPSHOT.OPTIONAL_METRIC_SUPPRESSED",
            "optional observed metric snapshot absent", false));
        continue;
      }
      return RefuseMetric(descriptor, context, snapshots, dependency, nullptr,
                          "SB_AGENT_METRIC_SNAPSHOT.MISSING",
                          dependency.metric_family, required_source_quorum, 0);
    }

    const auto expected_scope = ExpectedScopeUuid(context, dependency, options);
    std::set<std::string> source_ids;
    std::set<std::string> snapshot_ids;
    std::string schema_digest;
    std::string value_digest;

    for (const auto* snapshot : matching_snapshots) {
      if (snapshot == nullptr || !snapshot->present) {
        if (!dependency.required && options.allow_optional_missing) {
          evaluation.optional_suppressed = true;
          evaluation.diagnostics.push_back(MetricDiagnostic(
              dependency, snapshot,
              "SB_AGENT_METRIC_SNAPSHOT.OPTIONAL_METRIC_SUPPRESSED",
              "optional observed metric snapshot absent", false));
          continue;
        }
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.GAP_DETECTED",
                            "source reported absent metric snapshot",
                            required_source_quorum, source_ids.size());
      }

      if (snapshot->generation == 0 ||
          snapshot->observed_wall_microseconds == 0 ||
          snapshot->digest.empty() || snapshot->trust_provenance.empty()) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.MISSING_PROVENANCE",
            "generation timestamp digest or trust provenance missing",
            required_source_quorum, source_ids.size());
      }
      if (options.require_schema_digest && snapshot->schema_digest.empty()) {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.MISSING_SCHEMA_DIGEST",
                            "schema digest missing",
                            required_source_quorum, source_ids.size());
      }
      if (options.require_source_attestation &&
          (snapshot->source_id.empty() || snapshot->attestation_key_id.empty() ||
           snapshot->attestation_digest.empty() ||
           snapshot->source_sequence == 0 || !snapshot->attestation_verified)) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.SOURCE_ATTESTATION_MISSING",
            "source identity key digest and verified attestation are required",
            required_source_quorum, source_ids.size());
      }
      if (options.require_redaction_and_provenance &&
          (!snapshot->redacted || snapshot->protected_material_present ||
           snapshot->provenance_record.empty())) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.REDACTION_PROVENANCE_MISSING",
            "redacted provenance evidence missing or protected material present",
            required_source_quorum, source_ids.size());
      }
      if (options.fail_on_gap_replay_or_disagreement &&
          (snapshot->gap_detected || snapshot->replay_detected ||
           snapshot->disagreement_detected)) {
        const std::string code = snapshot->gap_detected
            ? "SB_AGENT_METRIC_SNAPSHOT.GAP_DETECTED"
            : (snapshot->replay_detected
                   ? "SB_AGENT_METRIC_SNAPSHOT.REPLAY_DETECTED"
                   : "SB_AGENT_METRIC_SNAPSHOT.DISAGREEMENT");
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            code, snapshot->snapshot_id,
                            required_source_quorum, source_ids.size());
      }
      if (snapshot->previous_source_sequence != 0 &&
          snapshot->source_sequence != snapshot->previous_source_sequence + 1) {
        const std::string code =
            snapshot->source_sequence <= snapshot->previous_source_sequence
                ? "SB_AGENT_METRIC_SNAPSHOT.REPLAY_DETECTED"
                : "SB_AGENT_METRIC_SNAPSHOT.GAP_DETECTED";
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            code, snapshot->source_id,
                            required_source_quorum, source_ids.size());
      }
      for (const auto& claim : snapshot->authority_claims) {
        if (IsForbiddenAuthorityClaim(claim)) {
          return RefuseMetric(
              descriptor, context, snapshots, dependency, snapshot,
              "SB_AGENT_METRIC_SNAPSHOT.FORBIDDEN_AUTHORITY_CLAIM",
              claim, required_source_quorum, source_ids.size());
        }
      }

      if (!dependency.namespace_prefix.empty() &&
          snapshot->namespace_path.rfind(dependency.namespace_prefix, 0) != 0) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.NAMESPACE_SCHEMA_INCOMPATIBLE",
            snapshot->namespace_path, required_source_quorum, source_ids.size());
      }
      if (!snapshot->schema_compatible) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.NAMESPACE_SCHEMA_INCOMPATIBLE",
            "schema incompatible", required_source_quorum, source_ids.size());
      }
      if (expected_scope.empty() || snapshot->scope_uuid != expected_scope) {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.SCOPE_MISMATCH",
                            snapshot->scope_uuid, required_source_quorum,
                            source_ids.size());
      }
      if (!snapshot->trusted ||
          snapshot->trust_provenance == "untrusted" ||
          snapshot->trust_provenance == "fixture_untrusted") {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.UNTRUSTED",
                            snapshot->trust_provenance, required_source_quorum,
                            source_ids.size());
      }
      if (!SourceQualitySatisfies(snapshot->source_quality,
                                  dependency.required_source_quality)) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.SOURCE_QUALITY_INSUFFICIENT",
            std::to_string(static_cast<int>(snapshot->source_quality)),
            required_source_quorum, source_ids.size());
      }
      if (dependency.cluster_only && !snapshot->external_provider_attested) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
            snapshot->source_id, required_source_quorum, source_ids.size());
      }
      if (context.wall_now_microseconds < snapshot->observed_wall_microseconds) {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.INVALID_TIMESTAMP",
                            "snapshot timestamp is in the future",
                            required_source_quorum, source_ids.size());
      }
      if (dependency.max_freshness_microseconds != 0 &&
          context.wall_now_microseconds - snapshot->observed_wall_microseconds >
              dependency.max_freshness_microseconds) {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.STALE",
                            snapshot->snapshot_id, required_source_quorum,
                            source_ids.size());
      }
      if (!snapshot_ids.insert(snapshot->snapshot_id).second) {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.REPLAY_DETECTED",
                            "duplicate snapshot id", required_source_quorum,
                            source_ids.size());
      }
      if (!source_ids.insert(snapshot->source_id).second) {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.REPLAY_DETECTED",
                            "duplicate source id", required_source_quorum,
                            source_ids.size());
      }
      if (schema_digest.empty()) {
        schema_digest = snapshot->schema_digest;
      } else if (schema_digest != snapshot->schema_digest) {
        return RefuseMetric(
            descriptor, context, snapshots, dependency, snapshot,
            "SB_AGENT_METRIC_SNAPSHOT.SCHEMA_DIGEST_MISMATCH",
            snapshot->schema_digest, required_source_quorum, source_ids.size());
      }
      const auto candidate_value_digest = SnapshotValueDigest(*snapshot);
      if (value_digest.empty()) {
        value_digest = candidate_value_digest;
      } else if (value_digest != candidate_value_digest) {
        return RefuseMetric(descriptor, context, snapshots, dependency, snapshot,
                            "SB_AGENT_METRIC_SNAPSHOT.DISAGREEMENT",
                            snapshot->snapshot_id, required_source_quorum,
                            source_ids.size());
      }
    }

    if (!dependency.required && options.allow_optional_missing &&
        source_ids.empty()) {
      continue;
    }

    if (source_ids.size() < required_source_quorum) {
      return RefuseMetric(
          descriptor, context, snapshots, dependency,
          matching_snapshots.empty() ? nullptr : matching_snapshots.front(),
          "SB_AGENT_METRIC_SNAPSHOT.QUORUM_NOT_MET",
          std::to_string(source_ids.size()), required_source_quorum,
          source_ids.size());
    }
    evaluation.observed_source_quorum =
        std::max<u64>(evaluation.observed_source_quorum, source_ids.size());
  }

  if (!local_dependency_seen && cluster_dependency_seen &&
      cluster_path_failed_closed) {
    return RefuseMetric(
        descriptor, context, snapshots, descriptor.metric_dependencies.front(),
        nullptr, "SB_AGENT_METRIC_SNAPSHOT.CLUSTER_AUTHORITY_REQUIRED",
        descriptor.type_id);
  }

  evaluation.status =
      AgentRuntimeStatus{true, "SB_AGENT_METRIC_SNAPSHOT.ACCEPTED",
                         descriptor.type_id};
  evaluation.accepted = true;
  evaluation.failed_closed = false;
  AgentMetricSnapshotDiagnostic diagnostic;
  diagnostic.diagnostic_code = "SB_AGENT_METRIC_SNAPSHOT.ACCEPTED";
  diagnostic.detail = descriptor.type_id;
  diagnostic.failed_closed = false;
  diagnostic.evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_metric_snapshot_accepted|" + descriptor.type_id + "|" +
      evaluation.input_digest);
  evaluation.diagnostics.push_back(std::move(diagnostic));
  return evaluation;
}

AgentResourceReservationLedger::AgentResourceReservationLedger(
    std::string ledger_id,
    AgentResourceReservationLimits limits)
    : ledger_id_(std::move(ledger_id)), limits_(limits) {}

AgentResourceReservationSnapshot
AgentResourceReservationLedger::SnapshotLocked() const {
  AgentResourceReservationSnapshot snapshot;
  snapshot.ledger_id = ledger_id_;
  snapshot.active_reservation_count = active_.size();
  snapshot.created_reservation_count = created_count_;
  snapshot.released_reservation_count = released_count_;
  snapshot.active_memory_bytes = active_memory_bytes_;
  snapshot.active_worker_slots = active_worker_slots_;
  snapshot.active_overhead_microseconds = active_overhead_microseconds_;
  snapshot.foreground_reserved_worker_slots =
      std::min(limits_.foreground_reserved_worker_slots, limits_.max_worker_slots);
  snapshot.background_worker_slots =
      limits_.max_worker_slots > snapshot.foreground_reserved_worker_slots
          ? limits_.max_worker_slots - snapshot.foreground_reserved_worker_slots
          : 0;
  return snapshot;
}

AgentResourceReservationSnapshot AgentResourceReservationLedger::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

AgentResourceReservationResult AgentResourceReservationLedger::RefuseLocked(
    const std::string& agent_type_id,
    const AgentResourceReservationRequest& request,
    const std::string& code,
    const std::string& detail) const {
  AgentResourceReservationResult result;
  result.status = AgentError(code, detail);
  result.diagnostic_code = code;
  result.evidence_uuid = ReservationEvidenceUuid(
      ledger_id_, agent_type_id, request.reservation_key, code);
  result.snapshot = SnapshotLocked();
  result.ok = false;
  result.failed_closed = true;
  result.evidence = ReservationEvidence(code);
  return result;
}

AgentResourceReservationResult AgentResourceReservationLedger::Acquire(
    const AgentTypeDescriptor& descriptor,
    const AgentPolicy& policy,
    const AgentRuntimeContext& context,
    const AgentResourceReservationRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto policy_status = ValidateAgentPolicy(policy, descriptor);
  if (!policy_status.ok) {
    return RefuseLocked(descriptor.type_id, request,
                        policy_status.diagnostic_code, policy_status.detail);
  }
  if (request.reservation_key.empty() || request.owner_scope.empty()) {
    return RefuseLocked(descriptor.type_id, request,
                        "SB_AGENT_RESOURCE_RESERVATION.INVALID_REQUEST",
                        "reservation key and owner scope are required");
  }
  if (limits_.max_memory_bytes == 0 || limits_.max_worker_slots == 0 ||
      limits_.max_overhead_microseconds == 0) {
    return RefuseLocked(
        descriptor.type_id, request,
        "SB_AGENT_RESOURCE_RESERVATION.UNBOUNDED_LIMIT_REFUSED",
        "memory worker and overhead limits must be explicitly bounded");
  }
  if (request.memory_bytes == 0 || request.worker_slots == 0 ||
      request.overhead_microseconds == 0) {
    return RefuseLocked(
        descriptor.type_id, request,
        "SB_AGENT_RESOURCE_RESERVATION.EMPTY_REQUEST_REFUSED",
        "memory worker and overhead reservation requests must be non-zero");
  }
  if (request.cancellation_requested || context.shutdown_requested) {
    return RefuseLocked(descriptor.type_id, request,
                        "SB_AGENT_RESOURCE_RESERVATION.CANCELLED",
                        "cancellation or shutdown requested");
  }
  if (active_by_reservation_key_.find(request.reservation_key) !=
      active_by_reservation_key_.end()) {
    return RefuseLocked(descriptor.type_id, request,
                        "SB_AGENT_RESOURCE_RESERVATION.DUPLICATE",
                        request.reservation_key);
  }
  if (request.protect_foreground_work &&
      request.foreground_database_work_active &&
      descriptor.authority != AgentAuthorityClass::observe_only) {
    return RefuseLocked(
        descriptor.type_id, request,
        "SB_AGENT_RESOURCE_RESERVATION.FOREGROUND_PROTECTION",
        "foreground database work active");
  }
  if (AdditionExceeds(active_memory_bytes_, request.memory_bytes,
                      limits_.max_memory_bytes)) {
    return RefuseLocked(descriptor.type_id, request,
                        "SB_AGENT_RESOURCE_RESERVATION.MEMORY_OVER_BUDGET",
                        std::to_string(request.memory_bytes));
  }
  if (AdditionExceeds(active_overhead_microseconds_,
                      request.overhead_microseconds,
                      limits_.max_overhead_microseconds)) {
    return RefuseLocked(descriptor.type_id, request,
                        "SB_AGENT_RESOURCE_RESERVATION.OVERHEAD_OVER_BUDGET",
                        std::to_string(request.overhead_microseconds));
  }

  const u64 foreground_reserved =
      std::min(limits_.foreground_reserved_worker_slots, limits_.max_worker_slots);
  const u64 background_worker_slots =
      limits_.max_worker_slots > foreground_reserved
          ? limits_.max_worker_slots - foreground_reserved
          : 0;
  if (request.worker_slots == 0 || background_worker_slots == 0 ||
      AdditionExceeds(active_worker_slots_, request.worker_slots,
                      background_worker_slots)) {
    return RefuseLocked(
        descriptor.type_id, request,
        "SB_AGENT_RESOURCE_RESERVATION.FOREGROUND_WORKER_PROTECTION",
        std::to_string(request.worker_slots));
  }

  AgentResourceReservationToken token;
  token.token_id = ReservationEvidenceUuid(
      ledger_id_, descriptor.type_id, request.reservation_key, "token");
  token.reservation_key = request.reservation_key;
  token.owner_scope = request.owner_scope;
  token.agent_type_id = descriptor.type_id;
  token.memory_bytes = request.memory_bytes;
  token.worker_slots = request.worker_slots;
  token.overhead_microseconds = request.overhead_microseconds;
  token.active = true;

  active_memory_bytes_ += token.memory_bytes;
  active_worker_slots_ += token.worker_slots;
  active_overhead_microseconds_ += token.overhead_microseconds;
  active_by_reservation_key_[token.reservation_key] = token.token_id;
  active_[token.token_id] = token;
  ++created_count_;

  AgentResourceReservationResult result;
  result.status =
      AgentRuntimeStatus{true, "SB_AGENT_RESOURCE_RESERVATION.ACQUIRED",
                         token.token_id};
  result.reservation = token;
  result.snapshot = SnapshotLocked();
  result.diagnostic_code = result.status.diagnostic_code;
  result.evidence_uuid = ReservationEvidenceUuid(
      ledger_id_, descriptor.type_id, request.reservation_key,
      result.diagnostic_code);
  result.ok = true;
  result.reservation_created = true;
  result.failed_closed = false;
  result.evidence = ReservationEvidence(result.diagnostic_code);
  return result;
}

AgentResourceReservationResult AgentResourceReservationLedger::ReleaseLocked(
    const std::string& token_id,
    AgentResourceReservationReleaseReason reason) {
  const auto found = active_.find(token_id);
  if (found == active_.end()) {
    AgentResourceReservationResult result;
    const bool already_released = released_tokens_.find(token_id) != released_tokens_.end();
    const std::string code = already_released
        ? "SB_AGENT_RESOURCE_RESERVATION.RELEASE_IDEMPOTENT"
        : "SB_AGENT_RESOURCE_RESERVATION.NOT_FOUND";
    result.status = already_released
        ? AgentRuntimeStatus{true, code, token_id}
        : AgentError(code, token_id);
    result.snapshot = SnapshotLocked();
    result.diagnostic_code = code;
    result.evidence_uuid =
        ReservationEvidenceUuid(ledger_id_, "unknown", token_id, code);
    result.ok = already_released;
    result.idempotent = already_released;
    result.failed_closed = !already_released;
    result.evidence = ReservationEvidence(code);
    (void)reason;
    return result;
  }

  AgentResourceReservationToken token = found->second;
  active_memory_bytes_ -= std::min(active_memory_bytes_, token.memory_bytes);
  active_worker_slots_ -= std::min(active_worker_slots_, token.worker_slots);
  active_overhead_microseconds_ -=
      std::min(active_overhead_microseconds_, token.overhead_microseconds);
  active_by_reservation_key_.erase(token.reservation_key);
  active_.erase(found);
  released_tokens_.insert(token_id);
  ++released_count_;
  token.active = false;

  const std::string code =
      reason == AgentResourceReservationReleaseReason::cancellation
          ? "SB_AGENT_RESOURCE_RESERVATION.CANCEL_RELEASED"
          : "SB_AGENT_RESOURCE_RESERVATION.RELEASED";
  AgentResourceReservationResult result;
  result.status = AgentRuntimeStatus{true, code, token_id};
  result.reservation = token;
  result.snapshot = SnapshotLocked();
  result.diagnostic_code = code;
  result.evidence_uuid =
      ReservationEvidenceUuid(ledger_id_, token.agent_type_id, token_id, code);
  result.ok = true;
  result.released = true;
  result.failed_closed = false;
  result.evidence = ReservationEvidence(code);
  return result;
}

AgentResourceReservationResult AgentResourceReservationLedger::Release(
    const std::string& token_id,
    AgentResourceReservationReleaseReason reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReleaseLocked(token_id, reason);
}

AgentResourceReservationResult
AgentResourceReservationLedger::CancelOwnerReservations(
    const std::string& owner_scope) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> tokens;
  for (const auto& entry : active_) {
    if (entry.second.owner_scope == owner_scope) {
      tokens.push_back(entry.first);
    }
  }
  AgentResourceReservationResult last;
  last.status = AgentRuntimeStatus{
      true, "SB_AGENT_RESOURCE_RESERVATION.CANCEL_OWNER_EMPTY", owner_scope};
  last.snapshot = SnapshotLocked();
  last.diagnostic_code = last.status.diagnostic_code;
  last.evidence_uuid =
      ReservationEvidenceUuid(ledger_id_, "owner", owner_scope,
                              last.diagnostic_code);
  last.ok = true;
  last.failed_closed = false;
  last.evidence = ReservationEvidence(last.diagnostic_code);
  for (const auto& token : tokens) {
    last = ReleaseLocked(token, AgentResourceReservationReleaseReason::cancellation);
  }
  return last;
}

}  // namespace scratchbird::core::agents
