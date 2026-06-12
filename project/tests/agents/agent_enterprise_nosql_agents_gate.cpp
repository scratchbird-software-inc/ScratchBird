// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_production_classification.hpp"
#include "agents/nosql_backpressure_debt_agent.hpp"
#include "agents/nosql_family_maintenance_agent.hpp"
#include "transaction_inventory.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace mga = scratchbird::transaction::mga;

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest() {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = mga::MakeEmptyLocalTransactionInventory();
  request.inventory.next_local_transaction_id = 20;
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  request.always_active_session_inventory_authoritative = true;
  return request;
}

void TestNoSqlHelpersAreNotCanonicalProductionAgents() {
  Require(!agents::FindAgentType("nosql_family_maintenance_agent").has_value(),
          "NoSQL maintenance helper was exposed as a canonical agent");
  Require(!agents::FindAgentType("nosql_backpressure_debt_agent").has_value(),
          "NoSQL backpressure helper was exposed as a canonical agent");

  const auto records = agents::ClassifyAllCanonicalAgentProductionExposures();
  for (const auto& record : records) {
    Require(record.agent_type_id != "nosql_family_maintenance_agent",
            "NoSQL maintenance helper appeared in production exposure matrix");
    Require(record.agent_type_id != "nosql_backpressure_debt_agent",
            "NoSQL backpressure helper appeared in production exposure matrix");
  }
}

void TestNoSqlFamilyMaintenanceExecutesAuthoritativeWork() {
  impl::NoSqlFamilyMaintenanceAgentRequest request;
  request.horizon_request = HorizonRequest();
  request.engine_mga_authoritative = true;
  request.now_microseconds = 1000;
  request.execute_plan = true;
  request.scheduler_policy.max_scheduled_items = 4;
  request.scheduler_policy.max_total_work_units = 8;

  impl::NoSqlFamilyMaintenanceCandidate candidate;
  candidate.family = impl::NoSqlFamilyMaintenanceFamily::document;
  candidate.generation_id = "document-generation-001";
  candidate.generation_kind = "shape_fragment_generation";
  candidate.sealed_local_transaction_id = 4;
  candidate.superseded_local_transaction_id = 5;
  candidate.estimated_bytes = 4096;
  candidate.generation_evidence_authoritative = true;
  request.candidates.push_back(candidate);

  const auto result = impl::RunNoSqlFamilyMaintenanceAgent(request);
  Require(result.ok(), "NoSQL maintenance authoritative work was refused");
  Require(result.decision == impl::NoSqlFamilyMaintenanceDecisionKind::executed,
          "NoSQL maintenance did not execute eligible work");
  Require(result.actions.size() == 1, "NoSQL maintenance action was not produced");
  Require(result.actions.front().action_kind ==
              "document_shape_fragment_generation_merge",
          "NoSQL maintenance selected wrong action kind");
  Require(result.actions.front().cleanup_horizon_local_transaction_id == 20,
          "NoSQL maintenance did not consume MGA cleanup horizon");
}

void TestNoSqlFamilyMaintenanceRefusesUnsafeAuthority() {
  impl::NoSqlFamilyMaintenanceAgentRequest request;
  request.horizon_request = HorizonRequest();
  request.engine_mga_authoritative = false;

  impl::NoSqlFamilyMaintenanceCandidate candidate;
  candidate.family = impl::NoSqlFamilyMaintenanceFamily::graph;
  candidate.generation_id = "graph-generation-001";
  candidate.sealed_local_transaction_id = 4;
  candidate.generation_evidence_authoritative = true;
  request.candidates.push_back(candidate);

  const auto result = impl::RunNoSqlFamilyMaintenanceAgent(request);
  Require(!result.ok(), "NoSQL maintenance accepted non-authoritative MGA path");
  Require(result.diagnostic.diagnostic_code ==
              impl::kNoSqlMaintenanceCleanupHorizonNotAuthoritative,
          "NoSQL maintenance refusal diagnostic drifted");
}

impl::NoSqlBackpressureDebtAuthority SafeBackpressureAuthority() {
  impl::NoSqlBackpressureDebtAuthority authority;
  authority.engine_mga_authoritative = true;
  authority.request_context_authoritative = true;
  authority.security_snapshot_bound = true;
  authority.grants_proven = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  return authority;
}

void TestNoSqlBackpressureSuppressesUnsafeResults() {
  impl::NoSqlBackpressureDebtAgentRequest request;
  request.authority = SafeBackpressureAuthority();
  request.now_microseconds = 2000;

  impl::NoSqlBackpressureDebtEntry entry;
  entry.family = impl::NoSqlBackpressureDebtFamily::vector;
  entry.debt_kind = impl::NoSqlBackpressureDebtKind::result_suppression_policy;
  entry.object_uuid = "vector-family-001";
  entry.result_id = "candidate-result-001";
  entry.evidence_epoch = 2;
  entry.required_epoch = 5;
  entry.observed_cost_units = 90;
  entry.budget_cost_units = 40;
  entry.evidence_authoritative = true;
  entry.stale_result = true;
  entry.over_budget_result = true;
  request.entries.push_back(entry);

  const auto result = impl::RunNoSqlBackpressureDebtAgent(request);
  Require(result.ok(), "NoSQL backpressure refused authoritative suppression");
  Require(result.decision == impl::NoSqlBackpressureDebtDecisionKind::suppressed_result,
          "NoSQL backpressure did not suppress stale/over-budget result");
  Require(result.suppressions.size() == 1,
          "NoSQL backpressure suppression was not recorded");
  Require(result.suppressions.front().diagnostic_code ==
              impl::kNoSqlBackpressureDebtResultStale,
          "NoSQL backpressure diagnostic drifted");
}

void TestNoSqlBackpressureRefusesParserOrProviderAuthority() {
  impl::NoSqlBackpressureDebtAgentRequest request;
  request.authority = SafeBackpressureAuthority();
  request.authority.parser_or_reference_authority = true;

  impl::NoSqlBackpressureDebtEntry entry;
  entry.family = impl::NoSqlBackpressureDebtFamily::search;
  entry.debt_kind = impl::NoSqlBackpressureDebtKind::merge_compaction;
  entry.object_uuid = "search-segment-001";
  entry.evidence_authoritative = true;
  request.entries.push_back(entry);

  const auto result = impl::RunNoSqlBackpressureDebtAgent(request);
  Require(!result.ok(), "NoSQL backpressure accepted parser/reference authority");
  Require(result.diagnostic.diagnostic_code ==
              impl::kNoSqlBackpressureDebtUnsafeAuthority,
          "NoSQL backpressure unsafe-authority diagnostic drifted");
}

int main() {
  try {
    TestNoSqlHelpersAreNotCanonicalProductionAgents();
    TestNoSqlFamilyMaintenanceExecutesAuthoritativeWork();
    TestNoSqlFamilyMaintenanceRefusesUnsafeAuthority();
    TestNoSqlBackpressureSuppressesUnsafeResults();
    TestNoSqlBackpressureRefusesParserOrProviderAuthority();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
