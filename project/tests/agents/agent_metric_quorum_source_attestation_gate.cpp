// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_metric_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

agents::AgentRuntimeContext LocalContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.database_uuid = "019f0740-0000-7000-8000-000000000001";
  context.principal_uuid = "019f0740-0000-7000-8000-000000000002";
  context.rights = {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"};
  context.wall_now_microseconds = 1800000000000000ull;
  context.monotonic_now_microseconds = 7400000ull;
  return context;
}

agents::AgentRuntimeContext ClusterContext() {
  auto context = LocalContext();
  context.cluster_authority_available = true;
  context.cluster_uuid = "019f0740-0000-7000-8000-0000000000c1";
  return context;
}

agents::AgentTypeDescriptor Descriptor(const std::string& type_id) {
  const auto descriptor = agents::FindAgentType(type_id);
  Require(descriptor.has_value(), "missing descriptor " + type_id);
  return *descriptor;
}

agents::AgentMetricSourceQuality SourceQualityFor(
    const agents::AgentMetricDependency& dependency) {
  return dependency.required_source_quality ==
                 agents::AgentMetricSourceQuality::unknown
             ? agents::AgentMetricSourceQuality::trusted
             : dependency.required_source_quality;
}

agents::AgentObservedMetricSnapshot SnapshotFor(
    const agents::AgentMetricDependency& dependency,
    const agents::AgentRuntimeContext& context,
    std::string source_id,
    agents::u64 sequence,
    bool external_provider_attested = false) {
  agents::AgentObservedMetricSnapshot snapshot;
  snapshot.metric_family = dependency.metric_family;
  snapshot.namespace_path = dependency.namespace_prefix.empty()
                                ? dependency.metric_family
                                : dependency.namespace_prefix + ".observed";
  snapshot.source_id = std::move(source_id);
  snapshot.generation = sequence;
  snapshot.source_sequence = sequence;
  snapshot.previous_source_sequence = sequence - 1;
  const auto freshness = dependency.max_freshness_microseconds == 0
                             ? 1000ull
                             : dependency.max_freshness_microseconds / 2;
  snapshot.observed_wall_microseconds =
      context.wall_now_microseconds - freshness;
  snapshot.scope_uuid =
      dependency.cluster_only ? context.cluster_uuid : context.database_uuid;
  snapshot.digest = "sha256:value:" + dependency.metric_family;
  snapshot.value_digest = snapshot.digest;
  snapshot.schema_digest = "sha256:schema:" + dependency.metric_family;
  snapshot.source_quality = SourceQualityFor(dependency);
  snapshot.present = true;
  snapshot.trusted = true;
  snapshot.schema_compatible = true;
  snapshot.attestation_verified = true;
  snapshot.redacted = true;
  snapshot.protected_material_present = false;
  snapshot.external_provider_attested = external_provider_attested;
  snapshot.trust_provenance = "engine_metric_registry";
  snapshot.provenance_record = "redacted_metric_provenance:" +
                               dependency.metric_family + ":" +
                               snapshot.source_id;
  snapshot.attestation_key_id = "metric-attestation-key:" + snapshot.source_id;
  snapshot.attestation_digest = "sha256:attestation:" +
                                dependency.metric_family + ":" +
                                snapshot.source_id;
  snapshot.evidence_uuid = agents::DeterministicAgentRuntimeObjectUuidFromKey(
      "ceic074-metric-evidence|" + dependency.metric_family + "|" +
      snapshot.source_id);
  snapshot.snapshot_id = "ceic074:" + dependency.metric_family + ":" +
                         snapshot.source_id;
  snapshot.authority_claims = {"metric_evidence"};
  return snapshot;
}

std::vector<agents::AgentObservedMetricSnapshot> QuorumSnapshots(
    const agents::AgentTypeDescriptor& descriptor,
    const agents::AgentRuntimeContext& context,
    bool external_provider_attested = false) {
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  agents::u64 sequence = 1000;
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (!dependency.required) { continue; }
    if (dependency.cluster_only && context.cluster_uuid.empty()) { continue; }
    snapshots.push_back(SnapshotFor(dependency, context, "source-a",
                                    sequence++, external_provider_attested));
    snapshots.push_back(SnapshotFor(dependency, context, "source-b",
                                    sequence++, external_provider_attested));
  }
  return snapshots;
}

void RequireMetricRefusal(
    const agents::AgentMetricSnapshotEvaluation& evaluation,
    const std::string& code) {
  Require(!evaluation.accepted, "metric evaluation unexpectedly accepted");
  Require(evaluation.failed_closed, "metric evaluation did not fail closed");
  Require(evaluation.status.diagnostic_code == code,
          "status mismatch: " + evaluation.status.diagnostic_code);
  Require(!evaluation.diagnostics.empty(), "diagnostic missing");
  Require(evaluation.diagnostics.front().diagnostic_code == code,
          "diagnostic mismatch: " +
              evaluation.diagnostics.front().diagnostic_code);
  Require(!evaluation.input_digest.empty(), "input digest missing");
}

void TestAcceptedStrictQuorum() {
  const auto context = LocalContext();
  const auto descriptor = Descriptor("page_allocation_manager");
  const auto snapshots = QuorumSnapshots(descriptor, context);
  const auto evaluation = agents::EvaluateAgentObservedMetricSnapshots(
      descriptor, context, snapshots);

  Require(evaluation.accepted, "strict attested quorum was refused: " +
                                   evaluation.status.diagnostic_code);
  Require(!evaluation.failed_closed, "accepted quorum marked failed closed");
  Require(evaluation.required_source_quorum == 2,
          "default production quorum was not two sources");
  Require(evaluation.observed_source_quorum >= 2,
          "observed quorum count not recorded");
  Require(evaluation.status.diagnostic_code ==
              "SB_AGENT_METRIC_SNAPSHOT.ACCEPTED",
          "accepted diagnostic mismatch: " + evaluation.status.diagnostic_code);
}

void TestQuorumFreshnessQualityAndAttestationRefusals() {
  const auto context = LocalContext();
  const auto descriptor = Descriptor("page_allocation_manager");
  auto snapshots = QuorumSnapshots(descriptor, context);

  auto single_source = snapshots;
  for (auto it = single_source.begin(); it != single_source.end();) {
    if (it->source_id == "source-b") {
      it = single_source.erase(it);
    } else {
      ++it;
    }
  }
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   single_source),
      "SB_AGENT_METRIC_SNAPSHOT.QUORUM_NOT_MET");

  auto missing_attestation = snapshots;
  missing_attestation.front().attestation_verified = false;
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   missing_attestation),
      "SB_AGENT_METRIC_SNAPSHOT.SOURCE_ATTESTATION_MISSING");

  auto missing_schema = snapshots;
  missing_schema.front().schema_digest.clear();
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   missing_schema),
      "SB_AGENT_METRIC_SNAPSHOT.MISSING_SCHEMA_DIGEST");

  auto untrusted_quality = snapshots;
  untrusted_quality.front().source_quality =
      agents::AgentMetricSourceQuality::unknown;
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   untrusted_quality),
      "SB_AGENT_METRIC_SNAPSHOT.SOURCE_QUALITY_INSUFFICIENT");

  auto stale = snapshots;
  const auto& dependency = descriptor.metric_dependencies.front();
  stale.front().observed_wall_microseconds =
      context.wall_now_microseconds - dependency.max_freshness_microseconds - 1;
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context, stale),
      "SB_AGENT_METRIC_SNAPSHOT.STALE");
}

void TestGapReplayDisagreementRedactionAndAuthorityRefusals() {
  const auto context = LocalContext();
  const auto descriptor = Descriptor("page_allocation_manager");
  auto snapshots = QuorumSnapshots(descriptor, context);

  auto gap = snapshots;
  gap.front().source_sequence = gap.front().previous_source_sequence + 2;
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context, gap),
      "SB_AGENT_METRIC_SNAPSHOT.GAP_DETECTED");

  auto duplicate_source = snapshots;
  duplicate_source[1].source_id = duplicate_source[0].source_id;
  duplicate_source[1].snapshot_id += ":duplicate-source";
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   duplicate_source),
      "SB_AGENT_METRIC_SNAPSHOT.REPLAY_DETECTED");

  auto disagreement = snapshots;
  disagreement[1].value_digest += ":different";
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   disagreement),
      "SB_AGENT_METRIC_SNAPSHOT.DISAGREEMENT");

  auto unredacted = snapshots;
  unredacted.front().redacted = false;
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   unredacted),
      "SB_AGENT_METRIC_SNAPSHOT.REDACTION_PROVENANCE_MISSING");

  auto authority_drift = snapshots;
  authority_drift.front().authority_claims.push_back("transaction_finality");
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, context,
                                                   authority_drift),
      "SB_AGENT_METRIC_SNAPSHOT.FORBIDDEN_AUTHORITY_CLAIM");
}

void TestClusterExternalProviderFailClosed() {
  const auto descriptor = Descriptor("cluster_autoscale_manager");
  const auto local_context = LocalContext();
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, local_context,
                                                   {}),
      "SB_AGENT_METRIC_SNAPSHOT.CLUSTER_AUTHORITY_REQUIRED");

  const auto cluster_context = ClusterContext();
  auto snapshots = QuorumSnapshots(descriptor, cluster_context, false);
  RequireMetricRefusal(
      agents::EvaluateAgentObservedMetricSnapshots(descriptor, cluster_context,
                                                   snapshots),
      "SB_AGENT_METRIC_SNAPSHOT.CLUSTER_EXTERNAL_PROVIDER_REQUIRED");

  for (auto& snapshot : snapshots) {
    snapshot.external_provider_attested = true;
  }
  const auto accepted = agents::EvaluateAgentObservedMetricSnapshots(
      descriptor, cluster_context, snapshots);
  Require(accepted.accepted,
          "external-provider cluster quorum refused: " +
              accepted.status.diagnostic_code);
}

}  // namespace

int main() {
  TestAcceptedStrictQuorum();
  TestQuorumFreshnessQualityAndAttestationRefusals();
  TestGapReplayDisagreementRedactionAndAuthorityRefusals();
  TestClusterExternalProviderFailClosed();
  return EXIT_SUCCESS;
}
