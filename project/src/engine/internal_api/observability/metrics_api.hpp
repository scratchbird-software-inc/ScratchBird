// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_OBSERVABILITY_METRICS_API
struct EngineShowMetricsRequest : EngineApiRequest {};
struct EngineShowMetricsResult : EngineApiResult {};
EngineShowMetricsResult EngineShowMetrics(const EngineShowMetricsRequest& request);

struct EngineSysMetricsRegistryRequest : EngineApiRequest {};
struct EngineSysMetricsRegistryResult : EngineApiResult {};
EngineSysMetricsRegistryResult EngineSysMetricsRegistry(const EngineSysMetricsRegistryRequest& request);

struct EngineSysMetricsCurrentRequest : EngineApiRequest {};
struct EngineSysMetricsCurrentResult : EngineApiResult {};
EngineSysMetricsCurrentResult EngineSysMetricsCurrent(const EngineSysMetricsCurrentRequest& request);

struct EngineSysMetricsHistoryRequest : EngineApiRequest {};
struct EngineSysMetricsHistoryResult : EngineApiResult {};
EngineSysMetricsHistoryResult EngineSysMetricsHistory(const EngineSysMetricsHistoryRequest& request);

struct EngineSysMetricsPersistentHistoryRequest : EngineApiRequest {};
struct EngineSysMetricsPersistentHistoryResult : EngineApiResult {};
EngineSysMetricsPersistentHistoryResult EngineSysMetricsPersistentHistory(
    const EngineSysMetricsPersistentHistoryRequest& request);

struct EngineSysMetricsRollupsRequest : EngineApiRequest {};
struct EngineSysMetricsRollupsResult : EngineApiResult {};
EngineSysMetricsRollupsResult EngineSysMetricsRollups(const EngineSysMetricsRollupsRequest& request);

struct EngineSysMetricsSeriesRequest : EngineApiRequest {};
struct EngineSysMetricsSeriesResult : EngineApiResult {};
EngineSysMetricsSeriesResult EngineSysMetricsSeries(const EngineSysMetricsSeriesRequest& request);

struct EngineSysMetricsRetentionPoliciesRequest : EngineApiRequest {};
struct EngineSysMetricsRetentionPoliciesResult : EngineApiResult {};
EngineSysMetricsRetentionPoliciesResult EngineSysMetricsRetentionPolicies(
    const EngineSysMetricsRetentionPoliciesRequest& request);

struct EngineAlterMetricRetentionPolicyRequest : EngineApiRequest {};
struct EngineAlterMetricRetentionPolicyResult : EngineApiResult {};
EngineAlterMetricRetentionPolicyResult EngineAlterMetricRetentionPolicy(
    const EngineAlterMetricRetentionPolicyRequest& request);

struct EngineSysMetricsLabelsRequest : EngineApiRequest {};
struct EngineSysMetricsLabelsResult : EngineApiResult {};
EngineSysMetricsLabelsResult EngineSysMetricsLabels(const EngineSysMetricsLabelsRequest& request);

struct EngineSysMetricsProducersRequest : EngineApiRequest {};
struct EngineSysMetricsProducersResult : EngineApiResult {};
EngineSysMetricsProducersResult EngineSysMetricsProducers(const EngineSysMetricsProducersRequest& request);

struct EngineClusterSysMetricsRegistryRequest : EngineApiRequest {};
struct EngineClusterSysMetricsRegistryResult : EngineApiResult {};
EngineClusterSysMetricsRegistryResult EngineClusterSysMetricsRegistry(const EngineClusterSysMetricsRegistryRequest& request);

struct EngineClusterSysMetricsCurrentRequest : EngineApiRequest {};
struct EngineClusterSysMetricsCurrentResult : EngineApiResult {};
EngineClusterSysMetricsCurrentResult EngineClusterSysMetricsCurrent(const EngineClusterSysMetricsCurrentRequest& request);

struct EngineClusterSysMetricsHistoryRequest : EngineApiRequest {};
struct EngineClusterSysMetricsHistoryResult : EngineApiResult {};
EngineClusterSysMetricsHistoryResult EngineClusterSysMetricsHistory(
    const EngineClusterSysMetricsHistoryRequest& request);

struct EngineInspectMetricAdapterContractRequest : EngineApiRequest {};
struct EngineInspectMetricAdapterContractResult : EngineApiResult {};
EngineInspectMetricAdapterContractResult EngineInspectMetricAdapterContract(
    const EngineInspectMetricAdapterContractRequest& request);

struct EngineRecordLifecycleMetricRequest : EngineApiRequest {
  std::string operation_key;
  std::string outcome;
  std::string diagnostic_code;
  std::string route_family;
  std::string cache_family;
  std::string cache_reason;
  bool cache_invalidation_required = false;
};
struct EngineRecordLifecycleMetricResult : EngineApiResult {
  bool metric_recorded = false;
  bool cache_invalidation_recorded = false;
};
EngineRecordLifecycleMetricResult EngineRecordLifecycleMetric(
    const EngineRecordLifecycleMetricRequest& request);

}  // namespace scratchbird::engine::internal_api
