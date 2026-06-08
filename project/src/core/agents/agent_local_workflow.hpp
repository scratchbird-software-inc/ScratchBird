// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_LOCAL_OPERATIONAL_WORKFLOW_LEDGER
// Local non-cluster operational agents use this ledger to bind workflow
// decisions to durable agent/catalog/MGA evidence. The ledger is operational
// evidence only; it is not transaction finality, visibility, recovery,
// security, parser, donor, client, or cluster authority.

#include "agent_runtime.hpp"
#include "agent_durable_catalog.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentLocalWorkflowDomain {
  backup,
  archive,
  restore_drill,
  pitr,
  export_adapter,
  identity,
  session_control,
  job_control
};

enum class AgentLocalWorkflowState {
  prepared,
  applied,
  verified,
  cancelled,
  idempotent_replay,
  refused
};

struct AgentLocalWorkflowAuthority {
  std::string database_uuid;
  std::string principal_uuid;
  std::string subject_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  u64 local_transaction_id = 0;
  u64 catalog_generation = 0;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool storage_snapshot_authoritative = false;
  bool metadata_authoritative = false;
  bool security_catalog_authoritative = false;
  bool session_registry_authoritative = false;
  bool job_queue_authoritative = false;
  bool redaction_policy_valid = false;
  bool residency_policy_valid = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
  bool recovery_authority = false;
  bool cluster_route_requested = false;
};

struct AgentLocalWorkflowRequest {
  AgentLocalWorkflowDomain domain = AgentLocalWorkflowDomain::backup;
  std::string operation_id;
  std::string idempotency_key;
  AgentLocalWorkflowAuthority authority;
  std::string input_digest;
  bool dry_run = false;
  bool subsystem_precondition_satisfied = false;
  bool intended_state_observed = false;
};

struct AgentLocalWorkflowRecord {
  std::string workflow_uuid;
  AgentLocalWorkflowDomain domain = AgentLocalWorkflowDomain::backup;
  AgentLocalWorkflowState state = AgentLocalWorkflowState::refused;
  std::string operation_id;
  std::string subject_uuid;
  std::string idempotency_key;
  std::string input_digest;
  std::string verification_evidence_uuid;
  std::string diagnostic_code;
  u64 generation = 0;
  bool dry_run = false;
  bool outcome_verified = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
  bool recovery_authority = false;
  bool cluster_authority = false;
};

struct AgentLocalWorkflowApplyResult {
  AgentRuntimeStatus status;
  AgentLocalWorkflowRecord record;
  bool ok = false;
  bool idempotent = false;
  bool failed_closed = true;
  std::vector<std::string> evidence;
};

class AgentLocalWorkflowLedger {
 public:
  AgentLocalWorkflowLedger() = default;
  explicit AgentLocalWorkflowLedger(DurableAgentCatalogImage* catalog)
      : durable_catalog_(catalog) {}

  void BindDurableCatalog(DurableAgentCatalogImage* catalog) {
    durable_catalog_ = catalog;
  }

  AgentLocalWorkflowApplyResult Apply(const AgentLocalWorkflowRequest& request);
  const std::vector<AgentLocalWorkflowRecord>& records() const { return records_; }

 private:
  AgentRuntimeStatus AppendDurableRecord(const AgentLocalWorkflowRequest& request,
                                         const AgentLocalWorkflowRecord& record);

  std::vector<AgentLocalWorkflowRecord> records_;
  DurableAgentCatalogImage* durable_catalog_ = nullptr;
};

const char* AgentLocalWorkflowDomainName(AgentLocalWorkflowDomain domain);
const char* AgentLocalWorkflowStateName(AgentLocalWorkflowState state);
std::string AgentLocalWorkflowInputDigest(const AgentLocalWorkflowRequest& request);
AgentRuntimeStatus ValidateAgentLocalWorkflowAuthority(
    const AgentLocalWorkflowRequest& request);

}  // namespace scratchbird::core::agents
