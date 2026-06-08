// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/shadow_index_build_agent.hpp"
#include "shadow_index_build_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_SHADOW_INDEX_BUILD_LIFECYCLE_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  Require(generated.ok(), "DPC-040 generated UUID creation failed");
  return generated.value;
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

void RequireDiagnostic(const idx::ShadowIndexLifecycleResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(result.diagnostic.diagnostic_code == code, message);
  Require(result.evidence.diagnostic_code == code,
          "DPC-040 evidence diagnostic did not match result diagnostic");
}

void RequireHidden(const idx::ShadowIndexBuildRecord& record,
                   std::string_view message) {
  const auto route = idx::EvaluateShadowIndexPlannerVisibility(record);
  Require(!record.planner_visible, message);
  Require(!record.read_visible, message);
  Require(!route.planner_visible, message);
  Require(!route.read_visible, message);
  Require(route.diagnostic.diagnostic_code ==
              "shadow_index_visibility_hidden_until_publish" ||
          route.diagnostic.diagnostic_code ==
              "shadow_index_visibility_publish_evidence_missing" ||
          route.diagnostic.diagnostic_code ==
              "shadow_index_visibility_publish_identity_mismatch" ||
          route.diagnostic.diagnostic_code ==
                  "shadow_index_visibility_invalid_identity",
          "DPC-040 hidden route diagnostic changed");
}

idx::ShadowIndexBuildRequest ValidRequest(platform::u64 seed) {
  idx::ShadowIndexBuildRequest request;
  request.shadow_index_uuid = NewUuid(platform::UuidKind::object, seed + 1);
  request.table_uuid = NewUuid(platform::UuidKind::object, seed + 2);
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:shadow_build:" + std::to_string(seed);
  request.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:shadow_build:" + std::to_string(seed);
  return request;
}

idx::ShadowIndexBuildRecord BuiltRecord(idx::ShadowIndexBuildLedger* ledger,
                                        platform::u64 seed) {
  auto requested = idx::RequestShadowIndexBuild(ledger, ValidRequest(seed));
  Require(requested.ok(), "DPC-040 request failed");
  auto record = requested.record;
  auto building = idx::StartShadowIndexBuild(ledger, &record);
  Require(building.ok(), "DPC-040 start failed");
  auto built = idx::CompleteShadowIndexBuild(ledger, &record);
  Require(built.ok(), "DPC-040 complete failed");
  return record;
}

idx::ShadowIndexBuildRecord PublishReadyRecord(idx::ShadowIndexBuildLedger* ledger,
                                               platform::u64 seed) {
  auto record = BuiltRecord(ledger, seed);
  idx::ShadowIndexValidationRequest validation;
  validation.validation_succeeded = true;
  validation.validation_evidence_ref =
      "validation_evidence:shadow_build:" + std::to_string(seed);
  validation.engine_mga_inventory_evidence_present = true;
  auto validated = idx::ValidateShadowIndexBuild(ledger, &record, validation);
  Require(validated.ok(), "DPC-040 validation failed");

  idx::ShadowIndexPublishBarrierRequest barrier;
  barrier.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:" + std::to_string(seed);
  barrier.engine_owned_mga_publish_barrier = true;
  auto ready = idx::MarkShadowIndexPublishReady(ledger, &record, barrier);
  Require(ready.ok(), "DPC-040 publish barrier failed");
  return record;
}

void RequireLifecycleEvidenceRows(const idx::ShadowIndexBuildLedger& ledger) {
  Require(ledger.evidence.size() >= 6,
          "DPC-040 lifecycle evidence rows missing");
  bool saw_requested = false;
  bool saw_building = false;
  bool saw_built = false;
  bool saw_validated = false;
  bool saw_publish_ready = false;
  bool saw_published = false;
  for (const auto& row : ledger.evidence) {
    Require(!row.diagnostic_code.empty(), "DPC-040 diagnostic evidence missing");
    Require(!row.parser_finality_authority,
            "DPC-040 parser became finality authority");
    Require(!row.client_state_authority,
            "DPC-040 client state became finality authority");
    Require(!row.timestamp_ordering_authority,
            "DPC-040 timestamp ordering became finality authority");
    Require(!row.uuid_ordering_authority,
            "DPC-040 UUID ordering became finality authority");
    Require(!row.event_stream_authority,
            "DPC-040 event stream became finality authority");
    switch (row.state) {
      case idx::ShadowIndexBuildState::requested:
        saw_requested = true;
        Require(!row.planner_visible && !row.read_visible,
                "DPC-040 requested row was visible");
        break;
      case idx::ShadowIndexBuildState::building:
        saw_building = true;
        Require(!row.planner_visible && !row.read_visible,
                "DPC-040 building row was visible");
        break;
      case idx::ShadowIndexBuildState::built:
        saw_built = true;
        Require(!row.planner_visible && !row.read_visible,
                "DPC-040 built row was visible");
        break;
      case idx::ShadowIndexBuildState::validated:
        saw_validated = true;
        Require(row.validation_evidence_present,
                "DPC-040 validated row lacked validation evidence");
        Require(!row.planner_visible && !row.read_visible,
                "DPC-040 validated row was visible");
        break;
      case idx::ShadowIndexBuildState::publish_ready:
        saw_publish_ready = true;
        Require(row.validation_evidence_present,
                "DPC-040 publish-ready row lacked validation evidence");
        Require(row.publish_barrier_evidence_present,
                "DPC-040 publish-ready row lacked barrier evidence");
        Require(row.publish_barrier_engine_owned_mga,
                "DPC-040 publish-ready row lacked engine MGA barrier flag");
        Require(!row.planner_visible && !row.read_visible,
                "DPC-040 publish-ready row was visible before publish");
        break;
      case idx::ShadowIndexBuildState::published:
        saw_published = true;
        Require(row.validation_evidence_present,
                "DPC-040 published row lacked validation evidence");
        Require(row.publish_barrier_evidence_present,
                "DPC-040 published row lacked barrier evidence");
        Require(row.planner_visible && row.read_visible,
                "DPC-040 published row was not visible");
        break;
      case idx::ShadowIndexBuildState::cancelled:
      case idx::ShadowIndexBuildState::refused:
        Require(!row.planner_visible && !row.read_visible,
                "DPC-040 terminal unsafe row was visible");
        break;
    }
  }
  Require(saw_requested && saw_building && saw_built && saw_validated &&
              saw_publish_ready && saw_published,
          "DPC-040 lifecycle evidence did not cover every publish state");
}

std::string AgentEvidenceValue(
    const std::vector<agents::ShadowIndexBuildAgentEvidenceField>& evidence,
    std::string_view key) {
  for (const auto& field : evidence) {
    if (field.key == key) {
      return field.value;
    }
  }
  return {};
}

void ProveInvisibleUntilPublishedAndPublishSuccess() {
  idx::ShadowIndexBuildLedger ledger;
  auto requested = idx::RequestShadowIndexBuild(&ledger, ValidRequest(1000));
  Require(requested.ok(), "DPC-040 requested state refused");
  RequireDiagnostic(requested,
                    "shadow_index_build_requested",
                    "DPC-040 requested diagnostic changed");
  auto record = requested.record;
  Require(record.build_id.valid(), "DPC-040 build UUID was not generated");
  RequireHidden(record, "DPC-040 requested state was visible");

  auto building = idx::StartShadowIndexBuild(&ledger, &record);
  Require(building.ok(), "DPC-040 building transition failed");
  RequireDiagnostic(building,
                    "shadow_index_build_building",
                    "DPC-040 building diagnostic changed");
  RequireHidden(record, "DPC-040 building state was visible");

  auto built = idx::CompleteShadowIndexBuild(&ledger, &record);
  Require(built.ok(), "DPC-040 built transition failed");
  RequireDiagnostic(built,
                    "shadow_index_build_built",
                    "DPC-040 built diagnostic changed");
  RequireHidden(record, "DPC-040 built state was visible");

  idx::ShadowIndexValidationRequest validation;
  validation.validation_succeeded = true;
  validation.validation_evidence_ref = "validation_evidence:shadow_build:1000";
  validation.engine_mga_inventory_evidence_present = true;
  auto validated = idx::ValidateShadowIndexBuild(&ledger, &record, validation);
  Require(validated.ok(), "DPC-040 validation transition failed");
  RequireHidden(record, "DPC-040 validated state was visible");

  idx::ShadowIndexPublishBarrierRequest barrier;
  barrier.publish_barrier_evidence_ref = "publish_barrier:engine_mga:1000";
  barrier.engine_owned_mga_publish_barrier = true;
  auto ready = idx::MarkShadowIndexPublishReady(&ledger, &record, barrier);
  Require(ready.ok(), "DPC-040 publish-ready transition failed");
  RequireHidden(record, "DPC-040 publish-ready state was visible");

  const auto shadow_uuid = record.shadow_index_uuid;
  agents::ShadowIndexBuildAgentPublishRequest agent_request;
  agent_request.engine_mga_authoritative = true;
  agent_request.agent_evidence_ref = "agent_evidence:dpc040:publish";
  auto published =
      agents::PublishShadowIndexBuildAgentStep(&ledger, &record, agent_request);
  Require(published.ok(), "DPC-040 publish agent refused valid lifecycle");
  Require(published.lifecycle.diagnostic.diagnostic_code ==
              "shadow_index_publish_success",
          "DPC-040 publish success diagnostic changed");
  Require(record.state == idx::ShadowIndexBuildState::published,
          "DPC-040 record did not publish");
  Require(record.planner_visible && record.read_visible,
          "DPC-040 published record was not visible");
  Require(SameUuid(record.published_index_uuid, shadow_uuid),
          "DPC-040 publish did not preserve generated shadow index UUID");

  const auto route = idx::EvaluateShadowIndexPlannerVisibility(record);
  Require(route.ok(), "DPC-040 planner route did not admit published index");
  Require(SameUuid(route.visible_index_uuid, shadow_uuid),
          "DPC-040 planner route changed published UUID identity");

  Require(AgentEvidenceValue(published.evidence, "lifecycle_state") ==
              "published",
          "DPC-040 agent evidence lacked published state");
  Require(AgentEvidenceValue(published.evidence, "planner_visible") == "true",
          "DPC-040 agent evidence lacked planner visibility");
  Require(AgentEvidenceValue(published.evidence,
                             "validation_evidence_present") == "true",
          "DPC-040 agent evidence lacked validation evidence");
  Require(AgentEvidenceValue(published.evidence,
                             "publish_barrier_evidence_present") == "true",
          "DPC-040 agent evidence lacked publish barrier evidence");
  Require(AgentEvidenceValue(published.evidence,
                             "parser_finality_authority") == "false",
          "DPC-040 agent evidence changed parser authority");
  Require(AgentEvidenceValue(published.evidence,
                             "timestamp_ordering_authority") == "false",
          "DPC-040 agent evidence changed timestamp authority");
  RequireLifecycleEvidenceRows(ledger);
}

void ProvePublishRefusalsRemainInvisible() {
  {
    idx::ShadowIndexBuildLedger ledger;
    auto request = ValidRequest(1900);
    request.engine_mga_inventory_evidence_ref.clear();
    auto refused = idx::RequestShadowIndexBuild(&ledger, request);
    Require(!refused.ok(), "DPC-040 missing MGA evidence request succeeded");
    RequireDiagnostic(refused,
                      "shadow_index_build_mga_evidence_missing",
                      "DPC-040 missing MGA evidence diagnostic changed");
    RequireHidden(refused.record,
                  "DPC-040 missing MGA evidence request became visible");
  }
  {
    idx::ShadowIndexBuildLedger ledger;
    auto record = BuiltRecord(&ledger, 2000);
    auto refused = idx::PublishShadowIndexBuild(&ledger, &record);
    Require(!refused.ok(), "DPC-040 publish without validation succeeded");
    RequireDiagnostic(refused,
                      "shadow_index_publish_validation_missing",
                      "DPC-040 missing validation diagnostic changed");
    Require(record.state == idx::ShadowIndexBuildState::refused,
            "DPC-040 missing validation did not refuse record");
    RequireHidden(record, "DPC-040 missing validation became visible");
  }
  {
    idx::ShadowIndexBuildLedger ledger;
    auto record = BuiltRecord(&ledger, 2100);
    idx::ShadowIndexValidationRequest validation;
    validation.validation_succeeded = true;
    validation.validation_evidence_ref = "validation_evidence:shadow_build:2100";
    validation.engine_mga_inventory_evidence_present = true;
    auto validated = idx::ValidateShadowIndexBuild(&ledger, &record, validation);
    Require(validated.ok(), "DPC-040 validation setup failed");
    auto refused = idx::PublishShadowIndexBuild(&ledger, &record);
    Require(!refused.ok(), "DPC-040 publish without barrier succeeded");
    RequireDiagnostic(refused,
                      "shadow_index_publish_barrier_missing",
                      "DPC-040 missing barrier diagnostic changed");
    RequireHidden(record, "DPC-040 missing barrier became visible");
  }
  {
    const std::vector<std::string> cases = {"build", "index", "table"};
    for (std::size_t i = 0; i < cases.size(); ++i) {
      idx::ShadowIndexBuildLedger ledger;
      auto record = PublishReadyRecord(&ledger, 2200 + i * 10);
      if (cases[i] == "build") {
        record.build_id = {};
      } else if (cases[i] == "index") {
        record.shadow_index_uuid = {};
      } else {
        record.table_uuid = {};
      }
      auto refused = idx::PublishShadowIndexBuild(&ledger, &record);
      Require(!refused.ok(), "DPC-040 invalid identity publish succeeded");
      RequireDiagnostic(refused,
                        "shadow_index_publish_invalid_identity",
                        "DPC-040 invalid identity diagnostic changed");
      RequireHidden(record, "DPC-040 invalid identity became visible");
    }
  }
  {
    idx::ShadowIndexBuildLedger ledger;
    auto record = PublishReadyRecord(&ledger, 2300);
    record.index_kind = idx::SecondaryIndexKind::unique;
    auto refused = idx::PublishShadowIndexBuild(&ledger, &record);
    Require(!refused.ok(), "DPC-040 unsafe index kind publish succeeded");
    RequireDiagnostic(refused,
                      "shadow_index_publish_unsafe_index_kind",
                      "DPC-040 unsafe index kind diagnostic changed");
    RequireHidden(record, "DPC-040 unsafe index kind became visible");
  }
  {
    idx::ShadowIndexBuildLedger ledger;
    auto record = PublishReadyRecord(&ledger, 2400);
    record.state = idx::ShadowIndexBuildState::published;
    record.validation_evidence_present = false;
    record.validation_evidence_ref.clear();
    record.planner_visible = true;
    record.read_visible = true;
    record.published_index_uuid = record.shadow_index_uuid;
    const auto route = idx::EvaluateShadowIndexPlannerVisibility(record);
    Require(!route.ok(),
            "DPC-040 published route without validation evidence succeeded");
    Require(route.diagnostic.diagnostic_code ==
                "shadow_index_visibility_publish_evidence_missing",
            "DPC-040 published missing evidence route diagnostic changed");
  }
  {
    idx::ShadowIndexBuildLedger ledger;
    auto record = PublishReadyRecord(&ledger, 2500);
    record.state = idx::ShadowIndexBuildState::published;
    record.planner_visible = true;
    record.read_visible = true;
    record.published_index_uuid =
        NewUuid(platform::UuidKind::object, 2599);
    const auto route = idx::EvaluateShadowIndexPlannerVisibility(record);
    Require(!route.ok(),
            "DPC-040 published route with changed UUID identity succeeded");
    Require(route.diagnostic.diagnostic_code ==
                "shadow_index_visibility_publish_identity_mismatch",
            "DPC-040 published identity mismatch diagnostic changed");
  }
}

void ProveCancelAndExplicitRefuseRemainInvisible() {
  {
    idx::ShadowIndexBuildLedger ledger;
    auto requested = idx::RequestShadowIndexBuild(&ledger, ValidRequest(3000));
    Require(requested.ok(), "DPC-040 cancel setup failed");
    auto record = requested.record;
    auto cancelled =
        idx::CancelShadowIndexBuild(&ledger, &record, "operator_cancelled");
    Require(cancelled.ok(), "DPC-040 cancel did not complete");
    RequireDiagnostic(cancelled,
                      "shadow_index_build_cancelled",
                      "DPC-040 cancel diagnostic changed");
    Require(record.state == idx::ShadowIndexBuildState::cancelled,
            "DPC-040 cancel state changed");
    RequireHidden(record, "DPC-040 cancelled record became visible");
  }
  {
    idx::ShadowIndexBuildLedger ledger;
    auto requested = idx::RequestShadowIndexBuild(&ledger, ValidRequest(3100));
    Require(requested.ok(), "DPC-040 refuse setup failed");
    auto record = requested.record;
    auto refused = idx::RefuseShadowIndexBuild(
        &ledger,
        &record,
        "shadow_index_build_policy_refused",
        "policy denied shadow build");
    Require(!refused.ok(), "DPC-040 explicit refusal did not fail closed");
    RequireDiagnostic(refused,
                      "shadow_index_build_policy_refused",
                      "DPC-040 explicit refusal diagnostic changed");
    Require(record.state == idx::ShadowIndexBuildState::refused,
            "DPC-040 explicit refusal state changed");
    RequireHidden(record, "DPC-040 refused record became visible");
  }
}

}  // namespace

int main() {
  Require(kGateSearchKey == "DPC_SHADOW_INDEX_BUILD_LIFECYCLE_GATE",
          "DPC-040 gate search key changed");
  ProveInvisibleUntilPublishedAndPublishSuccess();
  ProvePublishRefusalsRemainInvisible();
  ProveCancelAndExplicitRefuseRemainInvisible();
  return EXIT_SUCCESS;
}
