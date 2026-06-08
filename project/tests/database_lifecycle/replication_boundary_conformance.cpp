// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/replication_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

api::EngineUuid Uuid(std::string value) {
  return api::EngineUuid{std::move(value)};
}

api::EngineObjectReference Obj(std::string uuid, std::string kind) {
  api::EngineObjectReference object;
  object.uuid = Uuid(std::move(uuid));
  object.object_kind = std::move(kind);
  return object;
}

api::EngineReplicationBoundaryRequest BaseRequest(std::string kind) {
  api::EngineReplicationBoundaryRequest request;
  request.boundary_kind = std::move(kind);
  request.engine_authoritative = true;
  request.security_authorized = true;
  request.event_channel_authorized = true;
  request.backup_archive_hold_satisfied = true;
  request.capability_profile_allows = true;
  request.context.security_context_present = true;
  request.context.cluster_authority_available = true;
  request.context.local_transaction_id = 42;
  request.context.transaction_uuid = Uuid("019e0fc9-repl-7000-8000-000000000001");
  request.target_object = Obj("019e0fc9-table-7000-8000-000000000001", "table");
  request.publication = Obj("019e0fc9-pub-7000-8000-000000000001", "publication");
  request.subscription = Obj("019e0fc9-sub-7000-8000-000000000001", "subscription");
  request.slot = Obj("019e0fc9-slot-7000-8000-000000000001", "replication_slot");
  request.route_epoch = 7;
  request.route_generation = 3;
  request.retention_horizon_local_transaction_id = 12;
  request.policy_snapshot_uuid = "019e0fc9-policy-7000-8000-000000000001";
  request.idempotency_key = "replication-boundary-conformance";
  return request;
}

void TestStandaloneFailClosed() {
  auto request = BaseRequest("cdc_changefeed");
  request.context.cluster_authority_available = false;
  const auto result = api::EngineEvaluateReplicationBoundary(request);
  Require(!result.ok, "DBLC-013AK standalone replication boundary was accepted");
  Require(result.fail_closed && result.standalone_fail_closed,
          "DBLC-013AK standalone boundary did not fail closed");
  Require(result.cluster_authority_required,
          "DBLC-013AK standalone boundary did not require cluster authority");
  Require(!result.evidence.empty(), "DBLC-013AK standalone evidence missing");
}

void TestSecurityAndRetentionRefusals() {
  auto security = BaseRequest("subscription");
  security.security_authorized = false;
  const auto security_result = api::EngineEvaluateReplicationBoundary(security);
  Require(!security_result.ok, "DBLC-013AK security-denied subscription was accepted");
  Require(security_result.refusal_reason == "security_authorization_required",
          "DBLC-013AK security refusal mismatch");

  auto retention = BaseRequest("cdc_changefeed");
  retention.retention_horizon_local_transaction_id = 0;
  const auto retention_result = api::EngineEvaluateReplicationBoundary(retention);
  Require(!retention_result.ok, "DBLC-013AK CDC without retention horizon was accepted");
  Require(retention_result.refusal_reason == "retention_horizon_required",
          "DBLC-013AK retention diagnostic mismatch");

  auto backup_hold = BaseRequest("cdc_changefeed");
  backup_hold.backup_archive_hold_satisfied = false;
  const auto backup_result = api::EngineEvaluateReplicationBoundary(backup_hold);
  Require(!backup_result.ok, "DBLC-013AK CDC without backup/archive hold was accepted");
  Require(backup_result.refusal_reason == "backup_archive_hold_required",
          "DBLC-013AK backup/archive diagnostic mismatch");
}

void TestValidatedBoundaryStillFailsClosedUntilMapping() {
  const auto cdc = api::EngineEvaluateReplicationBoundary(BaseRequest("cdc_changefeed"));
  Require(!cdc.ok, "DBLC-013AK valid CDC boundary bypassed provider refusal");
  Require(cdc.fail_closed && cdc.standalone_fail_closed && !cdc.route_activation_allowed,
          "DBLC-013AK CDC route did not fail closed at provider boundary");
  Require(cdc.refusal_reason == "cluster_provider_boundary_closed",
          "DBLC-013AK CDC provider-boundary refusal mismatch");
  Require(HasEvidence(cdc, "cluster_provider_boundary", "provider_invoked") &&
              HasEvidence(cdc, "cluster_provider_boundary_result", "failed_closed"),
          "DBLC-013AK CDC provider-boundary evidence missing");
  Require(HasEvidence(cdc, "cluster_provider_boundary_operation",
                      "cluster.inspect_replication") &&
              HasEvidence(cdc, "cluster_provider_boundary_api_operation",
                          "replication.evaluate_boundary"),
          "DBLC-013AK CDC provider/API operation evidence mismatch");
  Require(cdc.publication_checked && cdc.subscription_checked && cdc.slot_checked &&
              cdc.changefeed_checked && cdc.retention_checked,
          "DBLC-013AK CDC coverage flags missing");

  auto live = BaseRequest("live_ingest");
  live.live_ingest_requested = true;
  const auto live_result = api::EngineEvaluateReplicationBoundary(live);
  Require(!live_result.ok, "DBLC-013AK live ingest boundary bypassed provider refusal");
  Require(live_result.live_ingest_checked && live_result.fail_closed &&
              live_result.standalone_fail_closed,
          "DBLC-013AK live ingest boundary did not fail closed at provider boundary");
  Require(live_result.refusal_reason == "cluster_provider_boundary_closed",
          "DBLC-013AK live ingest provider-boundary refusal mismatch");

  live.capability_profile_allows = false;
  const auto live_denied = api::EngineEvaluateReplicationBoundary(live);
  Require(!live_denied.ok, "DBLC-013AK live ingest capability denial was accepted");
  Require(live_denied.refusal_reason == "live_ingest_capability_required",
          "DBLC-013AK live ingest capability diagnostic mismatch");
}

void TestInspectBoundary() {
  api::EngineInspectReplicationRequest request;
  request.context.cluster_authority_available = false;
  const auto standalone = api::EngineInspectReplication(request);
  Require(!standalone.ok && standalone.standalone_fail_closed,
          "DBLC-013AK inspect did not fail closed in standalone");

  request.context.cluster_authority_available = true;
  const auto inspected = api::EngineInspectReplication(request);
  Require(!inspected.ok && inspected.replication_boundary_present &&
              inspected.standalone_fail_closed,
          "DBLC-013AK inspect bypassed provider refusal");
  Require(HasEvidence(inspected, "cluster_provider_boundary", "provider_invoked") &&
              HasEvidence(inspected, "cluster_provider_boundary_result", "failed_closed"),
          "DBLC-013AK inspect provider-boundary evidence missing");
  Require(HasEvidence(inspected, "cluster_provider_boundary_operation",
                      "cluster.inspect_replication") &&
              HasEvidence(inspected, "cluster_provider_boundary_api_operation",
                          "cluster.inspect_replication"),
          "DBLC-013AK inspect provider/API operation evidence mismatch");
}

}  // namespace

int main() {
  TestStandaloneFailClosed();
  TestSecurityAndRetentionRefusals();
  TestValidatedBoundaryStillFailsClosedUntilMapping();
  TestInspectBoundary();
  return EXIT_SUCCESS;
}
