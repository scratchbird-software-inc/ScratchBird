// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "observability/metrics_api.hpp"
#include "security/authentication_api.hpp"

#include <iostream>
#include <string>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

scratchbird::engine::internal_api::EngineRequestContext MetricsContext(bool cluster = false) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = true;
  context.cluster_authority_available = cluster;
  context.database_uuid.canonical = "db-probe";
  context.cluster_uuid.canonical = cluster ? "cluster-probe" : "";
  context.session_uuid.canonical = "session-probe";
  context.principal_uuid.canonical = "principal-probe";
  context.trace_tags = {"right:OBS_METRICS_READ_FAMILY", "right:OBS_METRICS_EXPORT"};
  return context;
}

bool ContainsValue(const scratchbird::engine::internal_api::EngineApiResult& result,
                   const std::string& value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.second.encoded_value.find(value) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

int main() {
  using namespace scratchbird::core::metrics;
  using namespace scratchbird::engine::internal_api;
  bool ok = true;

  EngineSysMetricsRegistryRequest registry_request;
  registry_request.context = MetricsContext(false);
  auto registry_result = EngineSysMetricsRegistry(registry_request);
  ok &= Require(registry_result.ok, "sys.metrics.registry succeeds with metrics read right");
  ok &= Require(!ContainsValue(registry_result, "cluster.sys.metrics"), "local sys.metrics registry excludes cluster descriptors");

  EngineClusterSysMetricsRegistryRequest cluster_request;
  cluster_request.context = MetricsContext(false);
  auto cluster_fail = EngineClusterSysMetricsRegistry(cluster_request);
  ok &= Require(!cluster_fail.ok && cluster_fail.cluster_authority_required,
                "cluster.sys.metrics fails closed without cluster authority");
  cluster_request.context = MetricsContext(true);
  auto cluster_ok = EngineClusterSysMetricsRegistry(cluster_request);
  ok &= Require(cluster_ok.ok && ContainsValue(cluster_ok, "cluster.sys.metrics"),
                "cluster.sys.metrics registry is available with cluster authority");

  (void)PublishIdentitySessionsActive(
      1.0,
      "local_password",
      "self",
      Labels({{"session_uuid", "session-secret"}, {"principal_uuid", "principal-secret"}}));
  EngineSysMetricsCurrentRequest current_request;
  current_request.context = MetricsContext(false);
  current_request.context.trace_tags = {"right:OBS_METRICS_READ_FAMILY"};
  auto current = EngineSysMetricsCurrent(current_request);
  ok &= Require(current.ok, "sys.metrics.current succeeds with family read right");
  ok &= Require(!ContainsValue(current, "session-secret") && ContainsValue(current, "<redacted>"),
                "sys.metrics.current redacts sensitive labels without broad right");

  EngineInspectMetricAdapterContractRequest adapter_request;
  adapter_request.context = MetricsContext(false);
  adapter_request.option_envelopes.push_back("format:openmetrics");
  auto adapter_ok = EngineInspectMetricAdapterContract(adapter_request);
  ok &= Require(adapter_ok.ok && ContainsValue(adapter_ok, "core_metrics_registry"),
                "metric adapter contract accepts OpenMetrics");
  adapter_request.option_envelopes.clear();
  adapter_request.option_envelopes.push_back("format:unsupported");
  auto adapter_fail = EngineInspectMetricAdapterContract(adapter_request);
  ok &= Require(!adapter_fail.ok, "metric adapter contract rejects unsupported formats");

  EngineAuthenticateRequest auth_request;
  auth_request.context = MetricsContext(false);
  auth_request.provider_family = "local_password";
  auth_request.principal_claim = "alice";
  auth_request.credential_evidence_present = true;
  auto auth = EngineAuthenticate(auth_request);
  ok &= Require(auth.ok && auth.authenticated, "authentication publishes identity metrics on success");

  ok &= Require(PublishArchiveLagBytes(1.0, "primary", "probe").ok,
                "archive producer contract emits bounded sample");
  ok &= Require(RecordAgentAction("memory_governor", "probe", "ok").ok,
                "agent producer API emits real agent metrics without implementing consumers");

  if (!ok) {
    return 1;
  }
  std::cout << "metrics runtime probe passed\n";
  return 0;
}
