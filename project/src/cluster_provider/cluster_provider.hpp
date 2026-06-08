// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::cluster_provider {

// CLUSTER_PROVIDER_BOUNDARY: production cluster behavior is supplied only by an
// external provider target; in-tree providers are non-mutating fail-closed modes.
// CLUSTER_PROVIDER_ABI_HANDSHAKE: cluster route admission is allowed only after
// the external provider reports the expected ABI, catalog manifest, operation
// set, feature flags, authority domains, and catalog digest.
inline constexpr std::uint32_t kClusterProviderAbiVersionCurrent = 1;
inline constexpr std::uint32_t kClusterProviderCatalogManifestVersionCurrent = 1;
inline constexpr std::uint32_t kClusterProviderCatalogRecordCodecVersionCurrent = 1;
inline constexpr std::string_view kClusterProviderContractId =
    "scratchbird.cluster_provider.abi.v1";
inline constexpr std::string_view kClusterProviderCatalogManifestId =
    "sb.cluster_catalog.public_source.v1";
inline constexpr std::string_view kClusterProviderCatalogCompatibilityDigest =
    "sha256:cd1bce3b9693404108dbb321725402eefdc7b6d98a424b8db5b0c05512c8ab29";
inline constexpr std::string_view kClusterSupportNotEnabledCode =
    "SBLR.CLUSTER.SUPPORT_NOT_ENABLED";
inline constexpr std::string_view kClusterHandshakeAcceptedCode =
    "SBLR.CLUSTER.HANDSHAKE.ACCEPTED";
inline constexpr std::string_view kClusterHandshakeExternalProviderRequiredCode =
    "SBLR.CLUSTER.HANDSHAKE.EXTERNAL_PROVIDER_REQUIRED";
inline constexpr std::string_view kClusterHandshakeStubCompileLinkOnlyCode =
    "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY";
inline constexpr std::string_view kClusterHandshakeAbiMismatchCode =
    "SBLR.CLUSTER.HANDSHAKE.ABI_VERSION_MISMATCH";
inline constexpr std::string_view kClusterHandshakeCatalogMismatchCode =
    "SBLR.CLUSTER.HANDSHAKE.CATALOG_MANIFEST_MISMATCH";
inline constexpr std::string_view kClusterHandshakeOperationSetIncompleteCode =
    "SBLR.CLUSTER.HANDSHAKE.OPERATION_SET_INCOMPLETE";
inline constexpr std::string_view kClusterHandshakeFeatureFlagsIncompleteCode =
    "SBLR.CLUSTER.HANDSHAKE.FEATURE_FLAGS_INCOMPLETE";
inline constexpr std::string_view kClusterHandshakeAuthorityDomainsIncompleteCode =
    "SBLR.CLUSTER.HANDSHAKE.AUTHORITY_DOMAINS_INCOMPLETE";
inline constexpr std::string_view kClusterHandshakeDigestMismatchCode =
    "SBLR.CLUSTER.HANDSHAKE.DIGEST_MISMATCH";
inline constexpr std::string_view kClusterHandshakeLocalRuntimeRefusedCode =
    "SBLR.CLUSTER.HANDSHAKE.LOCAL_RUNTIME_REFUSED";
inline constexpr std::string_view kClusterHandshakeLocalMutationRefusedCode =
    "SBLR.CLUSTER.HANDSHAKE.LOCAL_MUTATION_REFUSED";
inline constexpr std::string_view kClusterRouteAdmissionUnsupportedOperationCode =
    "SBLR.CLUSTER.ROUTE_ADMISSION.UNSUPPORTED_OPERATION";
inline constexpr std::string_view kClusterProviderInfoOperationId =
    "cluster.inspect_provider";
inline constexpr std::string_view kClusterProviderInfoOpcode =
    "SBLR_CLUSTER_INSPECT_PROVIDER";
inline constexpr std::string_view kClusterProviderInfoResultKind =
    "cluster.provider.info.v1";

struct ClusterProviderInfo {
  std::string_view provider_name;
  std::string_view provider_type;
  std::string_view provider_version;
  std::string_view support_status;
  std::uint32_t provider_abi_version = 0;
  std::string_view provider_contract_id;
  std::string_view catalog_manifest_id;
  std::uint32_t catalog_manifest_version = 0;
  std::uint32_t catalog_record_codec_version = 0;
  std::string_view catalog_compatibility_digest;
  std::vector<std::string_view> operation_ids;
  std::vector<std::string_view> feature_flags;
  std::vector<std::string_view> authority_domains;
  bool external_provider = false;
  bool compile_link_only = false;
  bool supports_execution = false;
  bool supports_route_admission = false;
  bool local_runtime_execution_enabled = false;
  bool mutable_by_local_core = false;
};

struct ClusterProviderRequest {
  internal_api::EngineRequestContext context;
  sblr::SblrOperationEnvelope envelope;
  internal_api::EngineApiRequest api_request;
};

struct ClusterProviderHandshakeIssue {
  std::string diagnostic_code;
  std::string detail;
};

struct ClusterProviderHandshakeResult {
  bool ok = false;
  bool route_admission_allowed = false;
  bool external_provider_required = true;
  bool failed_closed = true;
  std::string diagnostic_code;
  std::vector<ClusterProviderHandshakeIssue> issues;
};

struct ClusterProviderRouteAdmissionResult {
  bool ok = false;
  bool route_admitted = false;
  bool handshake_ok = false;
  bool failed_closed = true;
  std::string diagnostic_code;
  std::string detail;
};

struct ClusterProviderCommandBoundary {
  std::string_view normalized_command;
  std::string_view provider_operation_id;
  bool provider_routed = false;
};

ClusterProviderInfo DescribeClusterProvider();
std::string_view ClusterProviderMode();
bool ClusterProviderSupportsExecution();
internal_api::EngineApiResult InspectClusterProvider(const ClusterProviderRequest& request);
internal_api::EngineApiResult ExecuteClusterOperation(const ClusterProviderRequest& request);

inline const std::vector<ClusterProviderCommandBoundary>&
RequiredClusterProviderCommandBoundarySet() {
  static const std::vector<ClusterProviderCommandBoundary> commands = {
      {"cluster.topology.inspect", "cluster.inspect_state", true},
      {"cluster.topology.define_region", "cluster.control_topology", true},
      {"cluster.topology.define_shard_profile", "cluster.control_topology", true},
      {"cluster.topology.publish_manifest", "cluster.publish_topology_manifest", true},
      {"cluster.topology.validate_schema_version", "cluster.validate_topology_schema", true},
      {"cluster.topology.inspect_filespace_shards", "cluster.inspect_state", true},
      {"cluster.node.admit_member", "cluster.admit_member", true},
      {"cluster.node.remove_member", "cluster.remove_member", true},
      {"cluster.node.drain_member", "cluster.drain_member", true},
      {"cluster.node.set_role", "cluster.set_node_role", true},
      {"cluster.node.inspect_health", "cluster.inspect_state", true},
      {"cluster.node.validate_role_suitability", "cluster.validate_node_role", true},
      {"cluster.route.publish_owner", "cluster.publish_route", true},
      {"cluster.route.reject_stale_owner", "cluster.reject_stale_owner", true},
      {"cluster.route.inspect_plan", "cluster.inspect_routing_plan", true},
      {"cluster.placement.place_object", "cluster.place_object", true},
      {"cluster.placement.rebalance_shards", "cluster.rebalance_shards", true},
      {"cluster.placement.validate_partition_distribution",
       "cluster.validate_partition_distribution",
       true},
      {"cluster.placement.assign_tablet_range", "cluster.assign_tablet_range", true},
      {"cluster.tx.begin_distributed", "cluster.begin_distributed_transaction", true},
      {"cluster.tx.prepare_remote_participant",
       "cluster.prepare_remote_participant_insert",
       true},
      {"cluster.tx.publish_commit_barrier", "cluster.publish_commit_barrier", true},
      {"cluster.tx.publish_rollback_barrier", "cluster.publish_rollback_barrier", true},
      {"cluster.tx.recover_limbo_participant", "cluster.recover_limbo_participant", true},
      {"cluster.tx.advance_cleanup_low_water", "cluster.advance_cleanup_low_water", true},
      {"cluster.tx.validate_finality_proof", "cluster.validate_finality_proof", true},
      {"cluster.replication.consume_cluster_event", "cluster.inspect_replication", true},
      {"cluster.reconcile.branch_ledger", "cluster.reconcile_branch_ledger", true},
      {"cluster.reconcile.apply_merge_policy", "cluster.apply_merge_policy", true},
      {"cluster.reconcile.report_conflict",
       "cluster.report_reconciliation_conflict",
       true},
      {"cluster.reconcile.classify_non_mergeable",
       "cluster.classify_non_mergeable_data",
       true},
      {"cluster.reconcile.publish_client_finality",
       "cluster.publish_reconciled_finality",
       true},
      {"cluster.security.validate_epoch", "cluster.validate_epoch", true},
      {"cluster.security.issue_fence_token", "cluster.issue_fence_token", true},
      {"cluster.security.revoke_fence_token", "cluster.revoke_fence_token", true},
      {"cluster.security.validate_policy_version",
       "cluster.validate_policy_version",
       true},
      {"cluster.security.admit_provider_handshake",
       "cluster.validate_provider_handshake",
       true},
      {"cluster.security.refuse_local_cluster_mutation",
       "exact_refusal_no_provider_call",
       false},
      {"cluster.security.validate_route_authority",
       "cluster.validate_insert_route_fence",
       true},
      {"cluster.job.start_controlled", "cluster.start_job", true},
      {"cluster.job.cancel_controlled", "cluster.cancel_job", true},
      {"cluster.job.throttle_workload", "cluster.throttle_workload", true},
      {"cluster.admin.inspect_status", "cluster.inspect_state", true},
      {"cluster.admin.run_maintenance", "cluster.run_maintenance", true},
      {"cluster.admin.refuse_donor_shell_control",
       "exact_refusal_no_provider_call",
       false},
      {"cluster.metrics.snapshot", "cluster.inspect_state", true},
      {"cluster.metrics.trace_route", "cluster.inspect_routing_plan", true},
      {"cluster.metrics.emit_event", "cluster.emit_event", true},
      {"cluster.metrics.inspect_provider", "cluster.inspect_provider", true},
      {"cluster.metrics.collect_support_bundle", "cluster.collect_support_bundle", true},
      {"cluster.query.plan_distributed", "cluster.query.plan_distributed", true},
      {"cluster.query.admit_cross_node", "cluster.query.admit_cross_node", true},
      {"cluster.query.route_shard_read", "cluster.query.route_shard_read", true},
      {"cluster.query.execute_fragment", "cluster.query.execute_fragment", true},
      {"cluster.query.fanout_search", "cluster.query.fanout_search", true},
      {"cluster.query.merge_results", "cluster.query.merge_results", true},
      {"cluster.query.aggregate_partial", "cluster.query.aggregate_partial", true},
      {"cluster.query.validate_safe_read", "cluster.query.validate_safe_read", true},
      {"cluster.query.refuse_local_query_as_cluster_authority",
       "exact_refusal_no_provider_call",
       false},
  };
  return commands;
}

inline const std::vector<std::string_view>& RequiredClusterProviderOperationSet() {
  static const std::vector<std::string_view> operations = [] {
    std::vector<std::string_view> result;
    const auto add_unique = [&result](std::string_view operation_id) {
      if (operation_id == "exact_refusal_no_provider_call") return;
      if (std::find(result.begin(), result.end(), operation_id) == result.end()) {
        result.push_back(operation_id);
      }
    };
    for (const auto& command : RequiredClusterProviderCommandBoundarySet()) {
      if (!command.provider_routed) continue;
      add_unique(command.normalized_command);
      add_unique(command.provider_operation_id);
    }
    return result;
  }();
  return operations;
}

inline const std::vector<std::string_view>&
RequiredClusterProviderPreAdmissionRefusalSet() {
  static const std::vector<std::string_view> refusals = {
      "cluster.security.refuse_local_cluster_mutation",
      "cluster.admin.refuse_donor_shell_control",
      "cluster.query.refuse_local_query_as_cluster_authority"};
  return refusals;
}

inline const std::vector<std::string_view>& RequiredClusterProviderFeatureFlags() {
  static const std::vector<std::string_view> feature_flags = {
      "cluster.catalog.manifest.v1",
      "cluster.catalog.codec.v1",
      "cluster.catalog.schema_version.v1",
      "cluster.route_admission.v1",
      "cluster.authority_domains.v1",
      "cluster.digest_compatibility.v1",
      "cluster.execution.external_provider_only"};
  return feature_flags;
}

inline const std::vector<std::string_view>& RequiredClusterProviderAuthorityDomains() {
  static const std::vector<std::string_view> authority_domains = {
      "cluster.catalog",
      "cluster.routing",
      "cluster.topology",
      "cluster.fence",
      "cluster.security",
      "cluster.metrics",
      "cluster.authority_provenance",
      "mga.transaction_inventory.finality"};
  return authority_domains;
}

inline bool ContainsProviderToken(const std::vector<std::string_view>& values,
                                  std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

inline void AddClusterProviderHandshakeIssue(ClusterProviderHandshakeResult* result,
                                             std::string_view diagnostic_code,
                                             std::string_view detail) {
  result->ok = false;
  result->route_admission_allowed = false;
  result->failed_closed = true;
  if (result->diagnostic_code.empty()) {
    result->diagnostic_code = std::string(diagnostic_code);
  }
  result->issues.push_back({std::string(diagnostic_code), std::string(detail)});
}

inline ClusterProviderHandshakeResult ValidateClusterProviderHandshake(
    const ClusterProviderInfo& info) {
  ClusterProviderHandshakeResult result;
  result.ok = true;
  result.route_admission_allowed = false;
  result.external_provider_required = false;
  result.failed_closed = false;

  if (info.compile_link_only) {
    result.external_provider_required = true;
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeStubCompileLinkOnlyCode,
                                     "cluster provider is compile/link-only");
  }
  if (!info.external_provider) {
    result.external_provider_required = true;
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeExternalProviderRequiredCode,
                                     "cluster production route requires external provider");
  }
  if (!info.supports_execution || !info.supports_route_admission) {
    result.external_provider_required = true;
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeExternalProviderRequiredCode,
                                     "provider did not advertise execution and route admission");
  }
  if (info.local_runtime_execution_enabled) {
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeLocalRuntimeRefusedCode,
                                     "local runtime cluster execution is not a public release authority");
  }
  if (info.mutable_by_local_core) {
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeLocalMutationRefusedCode,
                                     "local core mutation through cluster provider is refused");
  }
  if (info.provider_abi_version != kClusterProviderAbiVersionCurrent ||
      info.provider_contract_id != kClusterProviderContractId) {
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeAbiMismatchCode,
                                     "provider ABI version or contract id mismatch");
  }
  if (info.catalog_manifest_id != kClusterProviderCatalogManifestId ||
      info.catalog_manifest_version != kClusterProviderCatalogManifestVersionCurrent ||
      info.catalog_record_codec_version !=
          kClusterProviderCatalogRecordCodecVersionCurrent) {
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeCatalogMismatchCode,
                                     "cluster catalog manifest or codec version mismatch");
  }
  if (info.catalog_compatibility_digest !=
      kClusterProviderCatalogCompatibilityDigest) {
    AddClusterProviderHandshakeIssue(&result,
                                     kClusterHandshakeDigestMismatchCode,
                                     "cluster catalog compatibility digest mismatch");
  }

  for (const auto operation : RequiredClusterProviderOperationSet()) {
    if (!ContainsProviderToken(info.operation_ids, operation)) {
      AddClusterProviderHandshakeIssue(&result,
                                       kClusterHandshakeOperationSetIncompleteCode,
                                       operation);
    }
  }
  for (const auto feature_flag : RequiredClusterProviderFeatureFlags()) {
    if (!ContainsProviderToken(info.feature_flags, feature_flag)) {
      AddClusterProviderHandshakeIssue(&result,
                                       kClusterHandshakeFeatureFlagsIncompleteCode,
                                       feature_flag);
    }
  }
  for (const auto authority_domain : RequiredClusterProviderAuthorityDomains()) {
    if (!ContainsProviderToken(info.authority_domains, authority_domain)) {
      AddClusterProviderHandshakeIssue(
          &result,
          kClusterHandshakeAuthorityDomainsIncompleteCode,
          authority_domain);
    }
  }

  if (result.ok) {
    result.route_admission_allowed = true;
    result.failed_closed = false;
    result.diagnostic_code = std::string(kClusterHandshakeAcceptedCode);
  }
  return result;
}

inline ClusterProviderRouteAdmissionResult EvaluateClusterProviderRouteAdmission(
    const ClusterProviderInfo& info,
    std::string_view operation_id) {
  const auto handshake = ValidateClusterProviderHandshake(info);
  ClusterProviderRouteAdmissionResult result;
  result.handshake_ok = handshake.ok;
  if (!handshake.ok) {
    result.diagnostic_code = handshake.diagnostic_code;
    result.detail = "cluster provider handshake failed";
    return result;
  }
  if (!ContainsProviderToken(info.operation_ids, operation_id)) {
    result.diagnostic_code =
        std::string(kClusterRouteAdmissionUnsupportedOperationCode);
    result.detail = std::string(operation_id);
    return result;
  }
  result.ok = true;
  result.route_admitted = true;
  result.failed_closed = false;
  result.diagnostic_code = std::string(kClusterHandshakeAcceptedCode);
  result.detail = std::string(operation_id);
  return result;
}

inline ClusterProviderHandshakeResult ValidateCurrentClusterProviderHandshake() {
  return ValidateClusterProviderHandshake(DescribeClusterProvider());
}

inline ClusterProviderRouteAdmissionResult AdmitCurrentClusterProviderRoute(
    std::string_view operation_id) {
  return EvaluateClusterProviderRouteAdmission(DescribeClusterProvider(),
                                               operation_id);
}

}  // namespace scratchbird::engine::cluster_provider
