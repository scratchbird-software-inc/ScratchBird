// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

void RequireMode(const agents::AgentPolicyBootstrapRecord& record,
                 bool enabled,
                 std::string_view action_mode,
                 agents::AgentActivationProfile activation) {
  Require(record.enabled == enabled, "enable-state mismatch for " + record.policy_uuid);
  Require(record.action_mode == std::string(action_mode),
          "action-mode mismatch for " + record.policy_uuid + ": " + record.action_mode);
  Require(record.activation == activation, "activation mismatch for " + record.policy_uuid);
}

bool DatabaseApplicableNonCluster(const agents::AgentTypeDescriptor& descriptor) {
  return descriptor.scope.find("database") != std::string::npos &&
         descriptor.deployment != agents::AgentDeployment::cluster &&
         !descriptor.cluster_only;
}

const agents::AgentPolicyBootstrapRecord* FindRecord(
    const std::vector<agents::AgentPolicyBootstrapRecord>& records,
    std::string_view agent_type_id,
    std::string_view policy_family) {
  for (const auto& record : records) {
    if (record.agent_type_id == agent_type_id && record.policy_family == policy_family) {
      return &record;
    }
  }
  return nullptr;
}

agents::AgentRuntimeActivationEvidence ValidEvidence() {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "019e0f2a-002c-7000-8000-000000000002";
  evidence.engine_instance_uuid = "engine-instance:019e0f2a-002c";
  evidence.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  evidence.policy_generation = 7;
  evidence.catalog_generation = 1;
  evidence.security_generation = 1;
  evidence.filespace_generation = 1;
  evidence.agent_set_generation = 3;
  evidence.health_generation = 1;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  return evidence;
}

void TestBootstrapRecordsCoverDatabaseApplicablePolicies() {
  const auto records = agents::DatabaseApplicableBaselinePolicyBootstrapRecords(7);
  Require(!records.empty(), "no baseline policy bootstrap records were produced");

  int checked_agents = 0;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    if (!DatabaseApplicableNonCluster(descriptor)) { continue; }
    ++checked_agents;
    const auto families = agents::RequiredPolicyFamiliesForAgent(descriptor);
    Require(!families.empty(), "missing required policy families for " + descriptor.type_id);
    for (const auto& family : families) {
      const auto* record = FindRecord(records, descriptor.type_id, family);
      Require(record != nullptr, "missing bootstrap record for " + descriptor.type_id + ":" + family);
      Require(record->policy_generation == 7, "policy generation mismatch for " + record->policy_uuid);
      if (family != "export_default_baseline") {
        Require(record->enabled, "baseline policy disabled for " + record->policy_uuid);
      }
      Require(record->run_interval_microseconds > 0, "missing cadence for " + record->policy_uuid);
      Require(record->cooldown_microseconds > 0, "missing cooldown for " + record->policy_uuid);
      Require(!record->action_mode.empty(), "missing action mode for " + record->policy_uuid);
      Require(!record->invalid_policy_behavior.empty(),
              "missing invalid-policy behavior for " + record->policy_uuid);
      Require(!record->required_fields.empty(), "missing required config fields for " + record->policy_uuid);
      for (const auto& field : record->required_fields) {
        const auto found = record->config_fields.find(field);
        Require(found != record->config_fields.end() && !found->second.empty(),
                "missing config field " + field + " for " + record->policy_uuid);
      }
    }
  }
  Require(checked_agents > 0, "no database-applicable non-cluster agents checked");
}

void TestSpecDefaultModesArePolicyFamilyDriven() {
  const auto records = agents::DatabaseApplicableBaselinePolicyBootstrapRecords(7);

  const auto* export_default = FindRecord(records, "export_adapter_manager", "export_default_baseline");
  Require(export_default != nullptr, "export default baseline missing");
  RequireMode(*export_default, false, "disabled", agents::AgentActivationProfile::disabled);

  const auto* job = FindRecord(records, "job_control_manager", "job_control_policy");
  Require(job != nullptr, "job control policy missing");
  RequireMode(*job, true, "operator_only", agents::AgentActivationProfile::disabled);

  const auto* restore = FindRecord(records, "restore_drill_manager", "restore_drill_policy");
  Require(restore != nullptr, "restore drill policy missing");
  RequireMode(*restore, true, "operator_only", agents::AgentActivationProfile::disabled);

  const auto* identity = FindRecord(records, "identity_manager", "identity_lifecycle_policy");
  Require(identity != nullptr, "identity lifecycle policy missing");
  RequireMode(*identity, true, "operator_only", agents::AgentActivationProfile::disabled);

  const auto* session = FindRecord(records, "session_control_manager", "session_control_policy");
  Require(session != nullptr, "session control policy missing");
  RequireMode(*session, true, "operator_only", agents::AgentActivationProfile::disabled);

  const auto* filespace = FindRecord(records, "filespace_capacity_manager", "filespace_capacity_policy");
  Require(filespace != nullptr, "filespace capacity policy missing");
  RequireMode(*filespace, true, "recommend_only", agents::AgentActivationProfile::recommend_only);

  const auto* shadow = FindRecord(records, "filespace_capacity_manager", "filespace_shadow_promotion_policy");
  Require(shadow != nullptr, "filespace shadow promotion policy missing");
  RequireMode(*shadow, true, "recommend_only", agents::AgentActivationProfile::recommend_only);

  const auto* preallocation = FindRecord(records, "page_allocation_manager", "page_preallocation_policy");
  Require(preallocation != nullptr, "page preallocation policy missing");
  RequireMode(*preallocation, true, "recommend_only", agents::AgentActivationProfile::recommend_only);

  const auto* relocation = FindRecord(records, "page_allocation_manager", "page_relocation_policy");
  Require(relocation != nullptr, "page relocation policy missing");
  RequireMode(*relocation, true, "recommend_only", agents::AgentActivationProfile::recommend_only);
}

void TestManagerSnapshotExposesBootstrapRecords() {
  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;

  agents::DatabaseLocalAgentRuntimeManager manager;
  const auto snapshot = manager.Start(ValidEvidence(), config);
  Require(snapshot.status.ok, "manager start failed: " + snapshot.status.diagnostic_code);
  Require(!snapshot.policy_bootstrap_records.empty(), "snapshot did not expose bootstrap records");
  Require(!snapshot.policy_attachments.empty(), "snapshot did not expose baseline attachments");

  const auto* filespace = FindRecord(snapshot.policy_bootstrap_records,
                                     "filespace_capacity_manager",
                                     "filespace_capacity_policy");
  Require(filespace != nullptr, "filespace capacity bootstrap policy was not inspectable");
  Require(filespace->config_fields.find("minimum_free_bytes") != filespace->config_fields.end(),
          "filespace capacity required threshold was absent");

  const auto* page = FindRecord(snapshot.policy_bootstrap_records,
                                "page_allocation_manager",
                                "page_relocation_policy");
  Require(page != nullptr, "page relocation bootstrap policy was not inspectable");
  Require(page->config_fields.find("require_transaction_holds_clear") != page->config_fields.end(),
          "page relocation MGA hold blocker field was absent");
}

}  // namespace

int main() {
  TestBootstrapRecordsCoverDatabaseApplicablePolicies();
  TestSpecDefaultModesArePolicyFamilyDriven();
  TestManagerSnapshotExposesBootstrapRecords();
  return EXIT_SUCCESS;
}
