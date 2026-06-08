// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_workload_resource_quota.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

agents::WorkloadResourceVector AllResources(std::uint64_t value) {
  agents::WorkloadResourceVector resources;
  resources.memory_bytes = value;
  resources.worker_slots = value;
  resources.temp_bytes = value;
  resources.filespace_bytes = value;
  resources.active_requests = value;
  resources.open_cursors = value;
  resources.transaction_slots = value;
  resources.buffer_bytes = value;
  resources.udr_bytes = value;
  return resources;
}

agents::WorkloadQuotaLimits Limits(std::uint64_t hard, std::uint64_t soft = 0) {
  agents::WorkloadQuotaLimits limits;
  limits.hard = AllResources(hard);
  limits.soft = AllResources(soft);
  return limits;
}

agents::WorkloadResourcePoolConfig Pool(std::string pool_id,
                                        agents::WorkloadClass workload_class,
                                        agents::WorkloadQuotaLimits limits) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = workload_class;
  pool.limits = limits;
  return pool;
}

agents::WorkloadAdmissionRequest Request(std::string request_uuid,
                                         std::string pool_id,
                                         agents::WorkloadClass workload_class,
                                         agents::WorkloadAdmissionSource source,
                                         agents::WorkloadResourceVector resources) {
  agents::WorkloadAdmissionRequest request;
  request.request_uuid = std::move(request_uuid);
  request.pool_id = std::move(pool_id);
  request.workload_class = workload_class;
  request.source = source;
  request.requested = resources;
  request.principal_tag = "principal-secret-must-not-leak";
  return request;
}

void RequireZeroUsage(const agents::WorkloadResourceQuotaController& controller,
                      std::string_view pool_id,
                      std::string_view message) {
  const auto usage = controller.UsageForPool(std::string(pool_id));
  Require(usage.memory_bytes == 0 && usage.worker_slots == 0 && usage.temp_bytes == 0 &&
              usage.filespace_bytes == 0 && usage.active_requests == 0 && usage.open_cursors == 0 &&
              usage.transaction_slots == 0 && usage.buffer_bytes == 0 && usage.udr_bytes == 0,
          message);
}

void TestAllBudgetDimensionsAndReleaseReasons() {
  agents::WorkloadResourceQuotaController controller;
  Require(controller.RegisterPool(Pool("foreground", agents::WorkloadClass::foreground,
                                       Limits(1000, 900)))
              .ok,
          "foreground quota pool registration failed");

  auto success = controller.Admit(Request("success", "foreground", agents::WorkloadClass::foreground,
                                          agents::WorkloadAdmissionSource::engine, AllResources(1)));
  Require(success.status.ok, "success reservation was not admitted");
  Require(success.reservation_created(), "success path did not create reservation before work");
  auto usage = controller.UsageForPool("foreground");
  Require(usage.memory_bytes == 1 && usage.worker_slots == 1 && usage.temp_bytes == 1 &&
              usage.filespace_bytes == 1 && usage.active_requests == 1 && usage.open_cursors == 1 &&
              usage.transaction_slots == 1 && usage.buffer_bytes == 1 && usage.udr_bytes == 1,
          "all quota dimensions were not reserved");
  Require(controller.Release(success.reservation.token_id, agents::WorkloadReleaseReason::success).ok,
          "success release failed");
  RequireZeroUsage(controller, "foreground", "success release did not return all resources");
  const auto duplicate =
      controller.Release(success.reservation.token_id, agents::WorkloadReleaseReason::success);
  Require(!duplicate.ok &&
              duplicate.diagnostic_code == "WORKLOAD_RESOURCE.RELEASE_ALREADY_RECORDED",
          "duplicate release was not rejected exactly once");

  auto failure = controller.Admit(Request("failure", "foreground", agents::WorkloadClass::foreground,
                                          agents::WorkloadAdmissionSource::engine, AllResources(2)));
  Require(failure.status.ok, "failure-path reservation was not admitted");
  Require(controller.Release(failure.reservation.token_id, agents::WorkloadReleaseReason::failure).ok,
          "failure release failed");
  RequireZeroUsage(controller, "foreground", "failure release leaked resources");

  auto cancel = controller.Admit(Request("cancel", "foreground", agents::WorkloadClass::foreground,
                                         agents::WorkloadAdmissionSource::engine, AllResources(3)));
  Require(cancel.status.ok, "cancel-path reservation was not admitted");
  Require(controller.Cancel(cancel.reservation.token_id).ok, "cancellation cleanup failed");
  RequireZeroUsage(controller, "foreground", "cancellation cleanup leaked resources");

  auto shutdown = controller.Admit(Request("shutdown", "foreground", agents::WorkloadClass::foreground,
                                           agents::WorkloadAdmissionSource::engine, AllResources(4)));
  Require(shutdown.status.ok, "shutdown-path reservation was not admitted");
  controller.BeginShutdownDrain("operator shutdown");
  const auto drained = controller.DrainForShutdown();
  Require(drained.size() == 1 && drained.front().ok, "shutdown drain did not release active reservation");
  RequireZeroUsage(controller, "foreground", "shutdown drain leaked resources");

  const auto refused = controller.Admit(Request("after-drain", "foreground",
                                               agents::WorkloadClass::foreground,
                                               agents::WorkloadAdmissionSource::engine,
                                               AllResources(1)));
  Require(refused.decision == agents::WorkloadAdmissionDecisionClass::drain_refused,
          "shutdown drain did not refuse new admission");
  Require(!refused.reservation_created(), "shutdown drain refusal created a reservation");
}

void TestSoftThrottleQueueHardDenialAndMaintenanceOverride() {
  agents::WorkloadResourceQuotaController throttle_controller;
  Require(throttle_controller.RegisterPool(Pool("foreground", agents::WorkloadClass::foreground,
                                                Limits(200, 100)))
              .ok,
          "throttle pool registration failed");
  auto first = throttle_controller.Admit(Request("first", "foreground", agents::WorkloadClass::foreground,
                                                agents::WorkloadAdmissionSource::engine,
                                                agents::WorkloadResourceVector{90, 1, 0, 0, 1, 0, 0, 0, 0}));
  Require(first.status.ok, "initial throttle setup reservation failed");
  auto throttled = throttle_controller.Admit(
      Request("throttled", "foreground", agents::WorkloadClass::foreground,
              agents::WorkloadAdmissionSource::engine,
              agents::WorkloadResourceVector{20, 1, 0, 0, 1, 0, 0, 0, 0}));
  Require(throttled.status.ok &&
              throttled.decision == agents::WorkloadAdmissionDecisionClass::throttled,
          "soft quota did not produce throttled admission");
  Require(throttled.reservation_created(), "soft throttle did not create an explicit reservation");
  Require(throttle_controller.Release(first.reservation.token_id,
                                      agents::WorkloadReleaseReason::success)
              .ok,
          "initial throttle setup release failed");
  Require(throttle_controller.Release(throttled.reservation.token_id,
                                      agents::WorkloadReleaseReason::success)
              .ok,
          "throttled release failed");

  auto hard = throttle_controller.Admit(
      Request("hard", "foreground", agents::WorkloadClass::foreground,
              agents::WorkloadAdmissionSource::engine,
              agents::WorkloadResourceVector{201, 1, 0, 0, 1, 0, 0, 0, 0}));
  Require(!hard.status.ok && hard.decision == agents::WorkloadAdmissionDecisionClass::rejected,
          "hard quota did not deny admission");
  Require(!hard.reservation_created(), "hard denial created a reservation");

  auto base = throttle_controller.Admit(Request("base", "foreground", agents::WorkloadClass::foreground,
                                               agents::WorkloadAdmissionSource::engine,
                                               agents::WorkloadResourceVector{90, 1, 0, 0, 1, 0, 0, 0, 0}));
  Require(base.status.ok, "maintenance setup reservation failed");
  auto maintenance_request =
      Request("maintenance", "foreground", agents::WorkloadClass::foreground,
              agents::WorkloadAdmissionSource::manager,
              agents::WorkloadResourceVector{20, 1, 0, 0, 1, 0, 0, 0, 0});
  maintenance_request.maintenance_override = true;
  auto maintenance = throttle_controller.Admit(maintenance_request);
  Require(maintenance.status.ok &&
              maintenance.decision == agents::WorkloadAdmissionDecisionClass::admitted &&
              maintenance.diagnostic.diagnostic_code ==
                  "WORKLOAD_RESOURCE.MAINTENANCE_OVERRIDE_ADMITTED",
          "maintenance override did not bypass only the soft throttle");
  Require(throttle_controller.Release(base.reservation.token_id,
                                      agents::WorkloadReleaseReason::success)
              .ok,
          "maintenance setup release failed");
  Require(throttle_controller.Release(maintenance.reservation.token_id,
                                      agents::WorkloadReleaseReason::success)
              .ok,
          "maintenance release failed");

  agents::WorkloadQuotaLimits queue_limits = Limits(200, 100);
  queue_limits.queue_on_soft_limit = true;
  queue_limits.max_queued_requests = 1;
  agents::WorkloadResourceQuotaController queue_controller;
  Require(queue_controller.RegisterPool(Pool("queued", agents::WorkloadClass::foreground,
                                             queue_limits))
              .ok,
          "queue pool registration failed");
  auto queue_base = queue_controller.Admit(Request("queue-base", "queued",
                                                  agents::WorkloadClass::foreground,
                                                  agents::WorkloadAdmissionSource::engine,
                                                  agents::WorkloadResourceVector{90, 1, 0, 0, 1, 0, 0, 0, 0}));
  Require(queue_base.status.ok, "queue setup reservation failed");
  const auto queued = queue_controller.Admit(Request("queued-soft", "queued",
                                                    agents::WorkloadClass::foreground,
                                                    agents::WorkloadAdmissionSource::engine,
                                                    agents::WorkloadResourceVector{20, 1, 0, 0, 1, 0, 0, 0, 0}));
  Require(queued.status.ok && queued.decision == agents::WorkloadAdmissionDecisionClass::queued,
          "soft quota did not queue when queue policy was enabled");
  Require(!queued.reservation_created(), "queued request created a reservation before admission");
  Require(queue_controller.QueuedRequestCount("queued") == 1, "queued request count mismatch");
  const auto queue_full = queue_controller.Admit(Request("queue-full", "queued",
                                                        agents::WorkloadClass::foreground,
                                                        agents::WorkloadAdmissionSource::engine,
                                                        agents::WorkloadResourceVector{20, 1, 0, 0, 1, 0, 0, 0, 0}));
  Require(!queue_full.status.ok &&
              queue_full.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.QUEUE_FULL",
          "full quota queue did not reject");
}

void TestHardDenialForEachBudget() {
  struct Case {
    const char* name;
    agents::WorkloadResourceVector hard;
    agents::WorkloadResourceVector requested;
  };
  const std::vector<Case> cases = {
      {"memory_bytes", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{11, 0, 0, 0, 0, 0, 0, 0, 0}},
      {"worker_slots", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 11, 0, 0, 0, 0, 0, 0, 0}},
      {"temp_bytes", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 0, 11, 0, 0, 0, 0, 0, 0}},
      {"filespace_bytes", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 0, 0, 11, 0, 0, 0, 0, 0}},
      {"active_requests", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 0, 0, 0, 11, 0, 0, 0, 0}},
      {"open_cursors", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 0, 0, 0, 0, 11, 0, 0, 0}},
      {"transaction_slots", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 0, 0, 0, 0, 0, 11, 0, 0}},
      {"buffer_bytes", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 0, 0, 0, 0, 0, 0, 11, 0}},
      {"udr_bytes", agents::WorkloadResourceVector{10, 10, 10, 10, 10, 10, 10, 10, 10},
       agents::WorkloadResourceVector{0, 0, 0, 0, 0, 0, 0, 0, 11}},
  };

  for (const auto& test_case : cases) {
    agents::WorkloadQuotaLimits limits;
    limits.hard = test_case.hard;
    agents::WorkloadResourceQuotaController controller;
    Require(controller.RegisterPool(Pool(test_case.name, agents::WorkloadClass::foreground,
                                         limits))
                .ok,
            "budget-specific pool registration failed");
    const auto denied = controller.Admit(Request(test_case.name, test_case.name,
                                                agents::WorkloadClass::foreground,
                                                agents::WorkloadAdmissionSource::engine,
                                                test_case.requested));
    Require(!denied.status.ok && denied.decision == agents::WorkloadAdmissionDecisionClass::rejected,
            "budget-specific hard denial did not reject");
    Require(denied.diagnostic.resource_name == test_case.name,
            "budget-specific hard denial named the wrong resource");
  }
}

void TestRouteSourcesCannotBypassQuotas() {
  struct SourceCase {
    const char* pool_id;
    agents::WorkloadClass workload_class;
    agents::WorkloadAdmissionSource source;
  };
  const std::vector<SourceCase> cases = {
      {"parser", agents::WorkloadClass::parser, agents::WorkloadAdmissionSource::parser},
      {"listener", agents::WorkloadClass::listener, agents::WorkloadAdmissionSource::listener},
      {"manager", agents::WorkloadClass::manager, agents::WorkloadAdmissionSource::manager},
      {"udr", agents::WorkloadClass::udr, agents::WorkloadAdmissionSource::udr_runtime},
  };

  for (const auto& test_case : cases) {
    agents::WorkloadResourceQuotaController controller;
    Require(controller.RegisterPool(Pool(test_case.pool_id, test_case.workload_class,
                                         Limits(1, 0)))
                .ok,
            "source-specific pool registration failed");
    const auto denied = controller.Admit(Request(test_case.pool_id, test_case.pool_id,
                                                test_case.workload_class, test_case.source,
                                                AllResources(2)));
    Require(!denied.status.ok && denied.decision == agents::WorkloadAdmissionDecisionClass::rejected,
            "route source bypassed hard quota");
    Require(!denied.reservation_created(), "route-source denial created a reservation");
  }

  agents::WorkloadResourceQuotaController controller;
  Require(controller.RegisterPool(Pool("engine", agents::WorkloadClass::foreground, Limits(10, 0)))
              .ok,
          "engine pool registration failed");
  const auto empty = controller.Admit(Request("empty", "engine", agents::WorkloadClass::foreground,
                                             agents::WorkloadAdmissionSource::parser,
                                             agents::WorkloadResourceVector{}));
  Require(!empty.status.ok &&
              empty.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.EMPTY_RESERVATION_REFUSED",
          "empty routed work was accepted without a reservation");
}

void TestClusterPathFailsClosedAndCancellationBeforeWork() {
  agents::WorkloadResourceQuotaController controller;
  auto cluster_pool = Pool("cluster", agents::WorkloadClass::cluster, Limits(10, 0));
  cluster_pool.cluster_only = true;
  Require(controller.RegisterPool(cluster_pool).ok, "cluster pool registration failed");

  auto standalone = Request("standalone-cluster", "cluster", agents::WorkloadClass::cluster,
                            agents::WorkloadAdmissionSource::cluster_remote, AllResources(1));
  standalone.cluster_scoped = true;
  standalone.cluster_authority_available = false;
  const auto fail_closed = controller.Admit(standalone);
  Require(!fail_closed.status.ok &&
              fail_closed.decision == agents::WorkloadAdmissionDecisionClass::failed_closed,
          "standalone cluster path did not fail closed");
  Require(!fail_closed.reservation_created(), "standalone cluster failure created a reservation");

  auto admitted_cluster = standalone;
  admitted_cluster.request_uuid = "cluster-admitted";
  admitted_cluster.cluster_authority_available = true;
  const auto admitted = controller.Admit(admitted_cluster);
  Require(admitted.status.ok && admitted.reservation_created(),
          "cluster path with authority was not admitted");
  Require(controller.Release(admitted.reservation.token_id,
                             agents::WorkloadReleaseReason::success)
              .ok,
          "cluster admission release failed");

  agents::WorkloadResourceQuotaController cancel_controller;
  Require(cancel_controller.RegisterPool(Pool("foreground", agents::WorkloadClass::foreground,
                                              Limits(10, 0)))
              .ok,
          "cancel pool registration failed");
  auto cancelled = Request("cancel-before-work", "foreground", agents::WorkloadClass::foreground,
                           agents::WorkloadAdmissionSource::engine, AllResources(1));
  cancelled.cancellation_requested = true;
  const auto refused = cancel_controller.Admit(cancelled);
  Require(!refused.status.ok &&
              refused.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.ADMISSION_CANCELLED",
          "pre-work cancellation did not refuse admission");
  Require(!refused.reservation_created(), "pre-work cancellation created a reservation");
  RequireZeroUsage(cancel_controller, "foreground", "pre-work cancellation changed usage");
}

void TestDiagnosticsAndRedactedEvidence() {
  agents::WorkloadResourceQuotaController controller;
  Require(controller.RegisterPool(Pool("foreground", agents::WorkloadClass::foreground,
                                       Limits(10, 5)))
              .ok,
          "evidence pool registration failed");
  const auto admitted = controller.Admit(Request("evidence", "foreground",
                                                agents::WorkloadClass::foreground,
                                                agents::WorkloadAdmissionSource::engine,
                                                AllResources(1)));
  Require(admitted.status.ok, "evidence reservation failed");
  const auto serialized = agents::SerializeWorkloadQuotaEvidence(admitted.evidence);
  Require(Contains(serialized, "redaction_class=operational_redacted"),
          "evidence did not declare redaction");
  Require(Contains(serialized, "principal=redacted"), "evidence did not redact principal");
  Require(!Contains(serialized, "principal-secret-must-not-leak"),
          "evidence leaked principal tag");
  Require(Contains(serialized, "memory_bytes=1") &&
              Contains(serialized, "transaction_slots=1") &&
              Contains(serialized, "udr_bytes=1"),
          "evidence omitted quota dimensions");
  Require(controller.Release(admitted.reservation.token_id,
                             agents::WorkloadReleaseReason::success)
              .ok,
          "evidence release failed");
}

void TestNoExternalFinalityAuthoritySurface() {
  agents::WorkloadResourceQuotaController controller;
  Require(controller.RegisterPool(Pool("foreground", agents::WorkloadClass::foreground,
                                       Limits(10, 0)))
              .ok,
          "authority pool registration failed");
  const auto admitted = controller.Admit(Request("authority", "foreground",
                                                agents::WorkloadClass::foreground,
                                                agents::WorkloadAdmissionSource::engine,
                                                AllResources(1)));
  Require(admitted.status.ok, "authority reservation failed");
  const auto serialized = agents::SerializeWorkloadQuotaEvidence(admitted.evidence);
  Require(Contains(serialized, "source=engine"), "quota evidence did not identify engine source");
  Require(!Contains(serialized, "parser_finality") &&
              !Contains(serialized, "external_storage") &&
              !Contains(serialized, "listener_finality") &&
              !Contains(serialized, "manager_finality"),
          "quota evidence exposed an external finality surface");
  Require(controller.Release(admitted.reservation.token_id,
                             agents::WorkloadReleaseReason::success)
              .ok,
          "authority release failed");
}

}  // namespace

int main() {
  TestAllBudgetDimensionsAndReleaseReasons();
  TestSoftThrottleQueueHardDenialAndMaintenanceOverride();
  TestHardDenialForEachBudget();
  TestRouteSourcesCannotBypassQuotas();
  TestClusterPathFailsClosedAndCancellationBeforeWork();
  TestDiagnosticsAndRedactedEvidence();
  TestNoExternalFinalityAuthoritySurface();
  return EXIT_SUCCESS;
}
