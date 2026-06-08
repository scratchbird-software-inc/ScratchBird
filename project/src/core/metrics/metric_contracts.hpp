// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_PRODUCER_CONTRACTS
// Contract-ready producer entry points for current and future runtime owners.
// These functions never fabricate samples. If the owning runtime is not wired,
// the descriptor readiness causes the underlying producer call to fail closed.

#include "metric_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::metrics {

struct MetricProducerContractStatus {
  std::string family;
  std::string producer_owner;
  MetricReadiness readiness = MetricReadiness::contract_ready_unwired;
  bool cluster_only = false;
  bool writable_now = false;
};

std::vector<MetricProducerContractStatus> MetricProducerContractsForOwner(const std::string& producer_owner);

MetricValidationResult PublishIdentitySessionsActive(double active_sessions,
                                                     std::string provider_family,
                                                     std::string visibility_scope,
                                                     MetricLabelSet labels = {});
MetricValidationResult PublishIdentityUsersOnline(double active_users,
                                                  std::string provider_family,
                                                  MetricLabelSet labels = {});

MetricValidationResult PublishParserSessionsActive(double active_sessions,
                                                   std::string parser_family,
                                                   std::string interface_family);
MetricValidationResult RecordParserFailure(std::string parser_family, std::string reason);
MetricValidationResult RecordParserCrash(std::string parser_family, std::string reason);
MetricValidationResult ObserveParserPolicyAttachLatency(double latency_microseconds,
                                                        std::string parser_family,
                                                        std::string result);
MetricValidationResult PublishListenerSessionsActive(double active_sessions,
                                                     std::string listener_family,
                                                     std::string interface_family);
MetricValidationResult RecordListenerReject(std::string listener_family, std::string reason);
MetricValidationResult ObserveListenerQueueDelay(double delay_microseconds,
                                                 std::string listener_family,
                                                 std::string result);
MetricValidationResult RecordManagementFrontendRequest(std::string request_class, std::string result);
MetricValidationResult ObserveManagementFrontendLatency(double latency_microseconds,
                                                       std::string request_class,
                                                       std::string result);

MetricValidationResult ObserveIndexLookupLatency(double latency_microseconds,
                                                 std::string index_kind,
                                                 std::string operation,
                                                 std::string result);
MetricValidationResult RecordIndexSplit(std::string index_kind, std::string reason);
MetricValidationResult PublishIndexReadAmplificationRatio(double ratio, std::string index_kind);

// SEARCH_KEY: SB_DATATYPE_METRIC_CONTRACTS
MetricValidationResult RecordDatatypeOperation(std::string canonical_type,
                                               std::string operation,
                                               std::string result,
                                               std::string reason);
MetricValidationResult RecordDatatypeCast(std::string source_type,
                                           std::string target_type,
                                           std::string result,
                                           std::string reason);
MetricValidationResult RecordDatatypeNumericBackend(std::string numeric_backend,
                                                    std::string canonical_type,
                                                    std::string operation,
                                                    std::string result,
                                                    std::string reason);
MetricValidationResult PublishDatatypeCatalogDescriptorCount(double descriptor_count,
                                                             std::string result);
MetricValidationResult RecordDomainMethodInvocation(std::string domain_uuid,
                                                    std::string method,
                                                    std::string result,
                                                    std::string reason);

// SEARCH_KEY: SB_INSERT_OPTIMIZATION_METRIC_CONTRACTS
MetricValidationResult RecordInsertBatchStarted(std::string object_uuid,
                                                std::string insert_mode,
                                                std::string result);
MetricValidationResult RecordInsertBatchFallback(std::string object_uuid,
                                                 std::string insert_mode,
                                                 std::string reason);
MetricValidationResult RecordInsertRowsInserted(double rows,
                                                std::string object_uuid,
                                                std::string insert_mode);
MetricValidationResult ObserveInsertRowsPerBatch(double rows,
                                                 std::string object_uuid,
                                                 std::string insert_mode);
MetricValidationResult RecordInsertTraceEvent(std::string object_uuid,
                                              std::string insert_mode,
                                              std::string phase);
MetricValidationResult RecordInsertCancel(std::string object_uuid,
                                          std::string insert_mode,
                                          std::string reason);

// SEARCH_KEY: SB_CLUSTER_INSERT_METRIC_CONTRACTS
MetricValidationResult RecordClusterInsertRouteCheck(std::string database_uuid,
                                                     std::string table_uuid,
                                                     std::string route_epoch,
                                                     std::string result,
                                                     std::string reason);
MetricValidationResult RecordClusterInsertStaleRouteRejection(std::string database_uuid,
                                                              std::string table_uuid,
                                                              std::string route_epoch,
                                                              std::string owner_node_uuid);
MetricValidationResult RecordClusterInsertParticipantAdmission(std::string database_uuid,
                                                               std::string table_uuid,
                                                               std::string participant_node_uuid,
                                                               std::string result,
                                                               std::string reason);
MetricValidationResult RecordClusterInsertRemoteRequest(std::string database_uuid,
                                                        std::string table_uuid,
                                                        std::string participant_node_uuid,
                                                        std::string result,
                                                        std::string retry_class);
MetricValidationResult ObserveClusterInsertFinalityWait(double latency_microseconds,
                                                        std::string database_uuid,
                                                        std::string table_uuid,
                                                        std::string participant_count,
                                                        std::string result);
MetricValidationResult RecordClusterInsertRowsMutated(double rows,
                                                      std::string database_uuid,
                                                      std::string table_uuid,
                                                      std::string participant_node_uuid,
                                                      std::string insert_mode);
MetricValidationResult RecordClusterInsertFailClosed(std::string database_uuid,
                                                     std::string table_uuid,
                                                     std::string authority_family,
                                                     std::string reason);
MetricValidationResult RecordClusterInsertBadStatsSuppressed(std::string database_uuid,
                                                             std::string table_uuid,
                                                             std::string reason);

// SEARCH_KEY: SB_FILESPACE_PAGE_METRIC_CONTRACTS
MetricValidationResult PublishFilespaceCapacitySnapshot(double total_bytes,
                                                        double used_bytes,
                                                        double free_bytes,
                                                        std::string database_uuid,
                                                        std::string filespace_uuid,
                                                        std::string node_uuid,
                                                        std::string filespace_role,
                                                        std::string device_class);
MetricValidationResult PublishFilespaceReservedBytes(double reserved_bytes,
                                                     std::string database_uuid,
                                                     std::string filespace_uuid,
                                                     std::string node_uuid,
                                                     std::string filespace_role,
                                                     std::string device_class,
                                                     std::string reason_class);
MetricValidationResult PublishFilespaceHealthState(double state_value,
                                                   std::string state_text,
                                                   std::string database_uuid,
                                                   std::string filespace_uuid,
                                                   std::string node_uuid,
                                                   std::string filespace_role,
                                                   std::string device_class);
MetricValidationResult PublishFilespaceRoleState(double state_value,
                                                 std::string state_text,
                                                 std::string database_uuid,
                                                 std::string filespace_uuid,
                                                 std::string node_uuid,
                                                 std::string filespace_role,
                                                 std::string device_class);
MetricValidationResult ObserveFilespaceDeviceReadLatency(double latency_microseconds,
                                                         std::string database_uuid,
                                                         std::string filespace_uuid,
                                                         std::string node_uuid,
                                                         std::string filespace_role,
                                                         std::string device_class);
MetricValidationResult ObserveFilespaceDeviceWriteLatency(double latency_microseconds,
                                                          std::string database_uuid,
                                                          std::string filespace_uuid,
                                                          std::string node_uuid,
                                                          std::string filespace_role,
                                                          std::string device_class);
MetricValidationResult ObserveFilespaceFsyncLatency(double latency_microseconds,
                                                    std::string database_uuid,
                                                    std::string filespace_uuid,
                                                    std::string node_uuid,
                                                    std::string filespace_role,
                                                    std::string device_class);
MetricValidationResult RecordFilespaceDeviceError(std::string error_class,
                                                  std::string database_uuid,
                                                  std::string filespace_uuid,
                                                  std::string node_uuid,
                                                  std::string filespace_role,
                                                  std::string device_class);
MetricValidationResult PublishPageAllocationSnapshot(double free_count,
                                                     double allocated_count,
                                                     std::string database_uuid,
                                                     std::string filespace_uuid,
                                                     std::string node_uuid,
                                                     std::string page_family,
                                                     std::string page_type);
MetricValidationResult PublishPageReleasedFreeCount(double released_free_count,
                                                    std::string database_uuid,
                                                    std::string filespace_uuid,
                                                    std::string node_uuid,
                                                    std::string page_family,
                                                    std::string page_type);
MetricValidationResult PublishPageReservedCount(double reserved_count,
                                                std::string database_uuid,
                                                std::string filespace_uuid,
                                                std::string node_uuid,
                                                std::string page_family,
                                                std::string page_type,
                                                std::string reason_class);
MetricValidationResult ObservePageAllocationLatency(double latency_microseconds,
                                                    std::string database_uuid,
                                                    std::string filespace_uuid,
                                                    std::string node_uuid,
                                                    std::string page_family,
                                                    std::string page_type);
MetricValidationResult RecordPageAllocationFailure(std::string error_class,
                                                   std::string database_uuid,
                                                   std::string filespace_uuid,
                                                   std::string node_uuid,
                                                   std::string page_family,
                                                   std::string page_type);
MetricValidationResult PublishPageCacheSnapshot(double resident_pages,
                                                double resident_bytes,
                                                double pinned_pages,
                                                double dirty_pages,
                                                std::string database_uuid,
                                                std::string filespace_uuid,
                                                std::string page_family);
MetricValidationResult RecordPageCacheEviction(std::string database_uuid,
                                               std::string filespace_uuid,
                                               std::string page_family,
                                               std::string result);
MetricValidationResult PublishPageCacheContextSnapshot(double resident_pages,
                                                       double resident_bytes,
                                                       double pinned_pages,
                                                       double dirty_pages,
                                                       std::string database_uuid,
                                                       std::string filespace_uuid,
                                                       std::string page_family,
                                                       std::string context,
                                                       std::string result,
                                                       std::string reason);
MetricValidationResult RecordPageCacheContextAdmission(double admissions,
                                                       std::string database_uuid,
                                                       std::string filespace_uuid,
                                                       std::string page_family,
                                                       std::string context,
                                                       std::string result,
                                                       std::string reason);
MetricValidationResult RecordPageCacheContextReuse(double reuses,
                                                   std::string database_uuid,
                                                   std::string filespace_uuid,
                                                   std::string page_family,
                                                   std::string context,
                                                   std::string result,
                                                   std::string reason);
MetricValidationResult RecordPageCacheContextEviction(double evictions,
                                                      std::string database_uuid,
                                                      std::string filespace_uuid,
                                                      std::string page_family,
                                                      std::string context,
                                                      std::string result,
                                                      std::string reason);
MetricValidationResult RecordPageCacheContextProtectedNormalHotSkip(double skips,
                                                                    std::string database_uuid,
                                                                    std::string filespace_uuid,
                                                                    std::string page_family,
                                                                    std::string context,
                                                                    std::string result,
                                                                    std::string reason);
MetricValidationResult RecordPageCacheContextRefusal(double refusals,
                                                     std::string database_uuid,
                                                     std::string filespace_uuid,
                                                     std::string page_family,
                                                     std::string context,
                                                     std::string result,
                                                     std::string reason);

// SEARCH_KEY: SB_OPTIMIZER_METRIC_FEEDBACK_CONTRACTS
MetricValidationResult PublishOptimizerPlanEstimateErrorRatio(double ratio,
                                                              std::string operator_family,
                                                              std::string plan_shape);

// SEARCH_KEY: SB_OPTIMIZER_FEEDBACK_METRIC_CONTRACTS
struct OptimizerRuntimeFeedbackMetricSample {
  double estimated_rows = 0.0;
  double actual_rows = 0.0;
  double estimated_pages = 0.0;
  double actual_pages = 0.0;
  double estimated_io_operations = 0.0;
  double actual_io_operations = 0.0;
  double estimated_visibility_recheck_rows = 0.0;
  double actual_visibility_recheck_rows = 0.0;
  double estimated_spill_bytes = 0.0;
  double actual_spill_bytes = 0.0;
  double memory_grant_bytes = 0.0;
  double peak_memory_bytes = 0.0;
  double recommended_memory_grant_bytes = 0.0;
  double estimated_latency_microseconds = 0.0;
  double actual_latency_microseconds = 0.0;
  double estimated_resource_units = 0.0;
  double actual_resource_units = 0.0;
};

MetricValidationResult PublishOptimizerRuntimeFeedbackSample(
    const OptimizerRuntimeFeedbackMetricSample& sample,
    std::string operator_family,
    std::string plan_shape);

// SEARCH_KEY: ODFR_CONTENTION_TELEMETRY_CONTRACT
struct LockLatchContentionSample {
  double wait_count = 0.0;
  double wait_microseconds = 0.0;
  std::string subsystem;
  std::string wait_class;
  std::string evidence_surface;
};

std::vector<std::string> LockLatchContentionRequiredWaitClasses();
MetricValidationResult RecordLockLatchContentionWait(
    const LockLatchContentionSample& sample);
MetricValidationResult ObserveRemoteFragmentLatency(double latency_microseconds,
                                                    std::string fragment_kind,
                                                    std::string route_class);
MetricValidationResult ObserveQueryFragmentQueueDelay(double delay_microseconds,
                                                      std::string fragment_kind,
                                                      std::string route_class);
MetricValidationResult ObserveQueryFragmentPropagationDelay(double delay_microseconds,
                                                            std::string fragment_kind,
                                                            std::string route_class);
MetricValidationResult ObserveQueryFragmentLocalConnectionDelay(double delay_microseconds,
                                                               std::string fragment_kind,
                                                               std::string route_class);
MetricValidationResult PublishQueryFragmentSampleFreshness(double freshness_microseconds,
                                                           std::string fragment_kind,
                                                           std::string route_class);

MetricValidationResult PublishArchiveLagBytes(double lag_bytes, std::string archive_class, std::string reason_class);
MetricValidationResult PublishArchiveSliceCount(double slice_count, std::string archive_class, std::string reason_class);
MetricValidationResult PublishArchiveSliceBytes(double slice_bytes, std::string archive_class, std::string reason_class);
MetricValidationResult PublishArchiveSliceAge(double age_microseconds, std::string archive_class, std::string reason_class);
MetricValidationResult PublishArchiveSliceMaxAge(double max_age_microseconds, std::string archive_class);
MetricValidationResult PublishArchiveHealthState(double state_value, std::string state_text, std::string archive_class);
MetricValidationResult PublishArchiveQueueDepth(double queue_depth, std::string archive_class, std::string reason_class);
MetricValidationResult PublishArchiveDeltaLagTransactions(double lag_transactions, std::string archive_class, std::string reason_class);
MetricValidationResult PublishArchiveDeltaApplyLagTransactions(double lag_transactions, std::string archive_class, std::string reason_class);
MetricValidationResult RecordArchiveChecksumFailure(std::string archive_class, std::string reason_class);
MetricValidationResult RecordArchiveRestoreRefusal(std::string archive_class, std::string reason_class);
MetricValidationResult PublishBackupInProgress(double in_progress, std::string operation);
MetricValidationResult PublishBackupProgressPercent(double progress_percent, std::string operation);
MetricValidationResult ObserveRestoreDrillDuration(double duration_microseconds, std::string result);
MetricValidationResult PublishPitrWindowAvailableSeconds(double available_seconds, std::string archive_class);
MetricValidationResult PublishSchedulerQueueDepth(double queue_depth, std::string scheduler_class);
MetricValidationResult RecordJobControlAction(std::string action_class, std::string result);

MetricValidationResult PublishClusterNodeCpuFeatureAvailable(std::string feature, bool available);
MetricValidationResult PublishClusterNodeRoleState(double state_value, std::string state_text, std::string node_class);
MetricValidationResult PublishClusterNodeSaturationRatio(double ratio, std::string node_class, std::string resource_class);
MetricValidationResult RecordClusterAdmissionDenied(std::string deny_reason, std::string workload_class);
MetricValidationResult PublishClusterLimboTransactions(double count);
MetricValidationResult PublishClusterRollingUpgradeReadiness(double state_value, std::string state_text);
MetricValidationResult PublishClusterSchedulerQueueDepth(double queue_depth, std::string scheduler_class);

MetricValidationResult RecordAgentAction(std::string agent_type, std::string action_class, std::string result);
MetricValidationResult ObserveAgentDecisionLatency(double latency_microseconds,
                                                   std::string agent_type,
                                                   std::string decision_class);
MetricValidationResult RecordAgentRuntimeServiceEvent(std::string event_class,
                                                      std::string result,
                                                      std::string diagnostic_code);
MetricValidationResult PublishAgentRuntimeServiceLeaseCount(double lease_count,
                                                            std::string state);
MetricValidationResult PublishAgentRuntimeServiceActionCount(double action_count,
                                                             std::string state);
MetricValidationResult PublishAgentRuntimeServiceHistoryCount(double history_count);
MetricValidationResult PublishAgentRuntimeServiceCatalogGeneration(double catalog_generation);
MetricValidationResult RecordFilespaceAgentCapacityRequest(std::string filespace_uuid,
                                                           std::string request_class,
                                                           std::string result);
MetricValidationResult PublishFilespaceAgentFreeReservePages(double pages,
                                                             std::string filespace_uuid,
                                                             std::string reserve_class);
MetricValidationResult ObserveFilespaceAgentDecisionLatency(double latency_microseconds,
                                                            std::string filespace_uuid,
                                                            std::string action_class);
MetricValidationResult RecordPageAllocationAgentRequest(std::string filespace_uuid,
                                                        std::string page_family,
                                                        std::string request_class,
                                                        std::string result);
MetricValidationResult PublishPageAllocationAgentPreallocatedPages(double pages,
                                                                   std::string filespace_uuid,
                                                                   std::string page_family);
MetricValidationResult RecordPageAllocationAgentRelocatedPages(double pages,
                                                               std::string filespace_uuid,
                                                               std::string page_family,
                                                               std::string result);
MetricValidationResult RecordAlertFired(std::string severity, std::string health_state, std::string owner_group);
MetricValidationResult RecordExportAdapterFailure(std::string adapter_family, std::string reason);
MetricValidationResult RecordExportAdapterRetry(std::string adapter_family, std::string reason);

}  // namespace scratchbird::core::metrics
