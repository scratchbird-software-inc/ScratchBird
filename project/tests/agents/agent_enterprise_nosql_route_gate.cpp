// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "nosql/nosql_backpressure_debt_api.hpp"
#include "nosql/nosql_family_maintenance_api.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "transaction_inventory.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace mga = scratchbird::transaction::mga;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const api::EngineApiResult& result,
                 const std::string& kind,
                 const std::string& id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.security_context_present = true;
  context.database_uuid.canonical = "019f0087-0000-7000-8000-000000000027";
  context.principal_uuid.canonical = "019f0087-0000-7000-8000-000000100027";
  context.transaction_uuid.canonical = "019f0087-0000-7000-8000-000000200027";
  context.local_transaction_id = 27;
  return context;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest() {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = mga::MakeEmptyLocalTransactionInventory();
  request.inventory.next_local_transaction_id = 50;
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  request.always_active_session_inventory_authoritative = true;
  return request;
}

api::EngineNoSqlMaintenanceGenerationCandidate Candidate(
    api::EngineNoSqlProviderFamily family,
    const std::string& generation_id,
    const std::string& generation_kind,
    api::EngineApiU64 governing_local_id) {
  api::EngineNoSqlMaintenanceGenerationCandidate candidate;
  candidate.family = family;
  candidate.generation_id = generation_id;
  candidate.generation_kind = generation_kind;
  candidate.sealed_local_transaction_id = governing_local_id;
  candidate.superseded_local_transaction_id = governing_local_id + 1;
  candidate.estimated_bytes = 4096 + governing_local_id;
  candidate.generation_evidence_authoritative = true;
  return candidate;
}

api::EnginePlanNoSqlFamilyMaintenanceRequest MaintenanceRequest() {
  api::EnginePlanNoSqlFamilyMaintenanceRequest request;
  request.context = Context();
  request.horizon_request = HorizonRequest();
  request.engine_mga_authoritative = true;
  request.now_microseconds = 270000;
  request.execute_plan = true;
  request.scheduler_policy.max_scheduled_items = 16;
  request.scheduler_policy.max_total_work_units = 32;
  request.candidates.push_back(Candidate(api::EngineNoSqlProviderFamily::kKeyValue,
                                         "kv-generation-027",
                                         "kv_generation", 4));
  request.candidates.push_back(Candidate(api::EngineNoSqlProviderFamily::kDocument,
                                         "document-generation-027",
                                         "shape_fragment_generation", 5));
  request.candidates.push_back(Candidate(api::EngineNoSqlProviderFamily::kSearch,
                                         "search-segment-027",
                                         "sealed_segment", 6));
  request.candidates.push_back(Candidate(api::EngineNoSqlProviderFamily::kVector,
                                         "vector-generation-027",
                                         "hnsw_generation", 7));
  request.candidates.push_back(Candidate(api::EngineNoSqlProviderFamily::kGraph,
                                         "graph-generation-027",
                                         "adjacency_generation", 8));
  request.candidates.push_back(Candidate(api::EngineNoSqlProviderFamily::kTimeSeries,
                                         "timeseries-bucket-027",
                                         "bucket_generation", 9));
  return request;
}

void TestFamilyMaintenanceEngineRoute() {
  const auto result = api::EnginePlanNoSqlFamilyMaintenance(MaintenanceRequest());
  Require(result.ok, "AEIC-027 NoSQL maintenance engine route refused");
  Require(result.agent_result.actions.size() == 6,
          "AEIC-027 NoSQL maintenance did not plan all family actions");
  Require(result.dml_summary.rows_changed == 6,
          "AEIC-027 NoSQL maintenance rows_changed did not reflect executed plan");
  Require(HasEvidence(result, "nosql_surface", "family_maintenance"),
          "AEIC-027 NoSQL maintenance omitted surface evidence");
  Require(HasEvidence(result, "provider_transaction_finality_authority", "false"),
          "AEIC-027 NoSQL maintenance omitted provider non-finality evidence");
  std::set<std::string> families;
  for (const auto& action : result.agent_result.actions) {
    families.insert(impl::NoSqlFamilyMaintenanceFamilyName(action.family));
    Require(action.cleanup_horizon_local_transaction_id == 50,
            "AEIC-027 NoSQL maintenance did not bind cleanup horizon");
    Require(action.executed, "AEIC-027 NoSQL maintenance action was not executed");
  }
  for (const std::string expected :
       {"key_value", "document", "search", "vector", "graph", "time_series"}) {
    Require(families.count(expected) == 1,
            "AEIC-027 NoSQL maintenance missing family: " + expected);
  }

  auto unsafe = MaintenanceRequest();
  unsafe.engine_mga_authoritative = false;
  const auto refused = api::EnginePlanNoSqlFamilyMaintenance(unsafe);
  Require(!refused.ok &&
              HasDiagnostic(refused, impl::kNoSqlMaintenanceCleanupHorizonNotAuthoritative),
          "AEIC-027 NoSQL maintenance accepted non-authoritative MGA route");

  auto cluster = MaintenanceRequest();
  cluster.option_envelopes.push_back("cluster_route:true");
  const auto cluster_refused =
      api::EnginePlanNoSqlFamilyMaintenance(cluster);
  Require(!cluster_refused.ok && cluster_refused.cluster_authority_required,
          "AEIC-027 NoSQL maintenance did not fail closed on core cluster route");
}

api::EngineNoSqlBackpressureDebtEntry DebtEntry(
    api::EngineNoSqlProviderFamily family,
    api::EngineNoSqlBackpressureDebtKind kind,
    const std::string& object_uuid,
    const std::string& result_id) {
  api::EngineNoSqlBackpressureDebtEntry entry;
  entry.family = family;
  entry.debt_kind = kind;
  entry.object_uuid = object_uuid;
  entry.result_id = result_id;
  entry.evidence_epoch = 2;
  entry.required_epoch = 5;
  entry.debt_units = 3;
  entry.observed_cost_units = 90;
  entry.budget_cost_units = 40;
  entry.evidence_authoritative = true;
  return entry;
}

api::EnginePlanNoSqlBackpressureDebtRequest BackpressureRequest() {
  api::EnginePlanNoSqlBackpressureDebtRequest request;
  request.context = Context();
  request.engine_mga_authoritative = true;
  request.request_context_authoritative = true;
  request.security_snapshot_bound = true;
  request.grants_proven = true;
  request.row_mga_recheck_required = true;
  request.row_security_recheck_required = true;
  request.now_microseconds = 275000;
  auto suppression = DebtEntry(
      api::EngineNoSqlProviderFamily::kVector,
      api::EngineNoSqlBackpressureDebtKind::kResultSuppressionPolicy,
      "vector-family-027",
      "vector-result-027");
  suppression.stale_result = true;
  suppression.over_budget_result = true;
  request.entries.push_back(suppression);
  request.entries.push_back(DebtEntry(
      api::EngineNoSqlProviderFamily::kSearch,
      api::EngineNoSqlBackpressureDebtKind::kMergeCompaction,
      "search-segment-027",
      "search-result-027"));
  request.entries.push_back(DebtEntry(
      api::EngineNoSqlProviderFamily::kDocument,
      api::EngineNoSqlBackpressureDebtKind::kRefresh,
      "document-family-027",
      "document-result-027"));
  return request;
}

void TestBackpressureEngineRoute() {
  const auto result = api::EnginePlanNoSqlBackpressureDebt(BackpressureRequest());
  Require(result.ok, "AEIC-027 NoSQL backpressure engine route refused");
  Require(result.agent_result.ledger_rows.size() == 3,
          "AEIC-027 NoSQL backpressure ledger rows missing");
  Require(result.agent_result.suppressions.size() == 1,
          "AEIC-027 NoSQL backpressure suppression missing");
  Require(!result.agent_result.actions.empty(),
          "AEIC-027 NoSQL backpressure planned actions missing");
  Require(HasEvidence(result, "nosql_surface", "backpressure_debt"),
          "AEIC-027 NoSQL backpressure omitted surface evidence");
  Require(HasEvidence(result, "parser_executes_sql", "false"),
          "AEIC-027 NoSQL backpressure omitted parser non-authority evidence");
  Require(HasDiagnostic(result, impl::kNoSqlBackpressureDebtResultStale),
          "AEIC-027 NoSQL backpressure omitted suppression diagnostic");

  auto unsafe = BackpressureRequest();
  unsafe.parser_or_donor_authority = true;
  const auto refused = api::EnginePlanNoSqlBackpressureDebt(unsafe);
  Require(!refused.ok &&
              HasDiagnostic(refused, impl::kNoSqlBackpressureDebtUnsafeAuthority),
          "AEIC-027 NoSQL backpressure accepted parser/donor authority");

  auto provider = BackpressureRequest();
  provider.provider_claims_transaction_finality_authority = true;
  const auto provider_refused = api::EnginePlanNoSqlBackpressureDebt(provider);
  Require(!provider_refused.ok &&
              HasDiagnostic(provider_refused,
                            impl::kNoSqlBackpressureDebtUnsafeAuthority),
          "AEIC-027 NoSQL backpressure accepted provider finality authority");

  auto cluster = BackpressureRequest();
  cluster.option_envelopes.push_back("distributed_route:true");
  const auto cluster_refused = api::EnginePlanNoSqlBackpressureDebt(cluster);
  Require(!cluster_refused.ok && cluster_refused.cluster_authority_required,
          "AEIC-027 NoSQL backpressure did not fail closed on core cluster route");
}

}  // namespace

int main() {
  TestFamilyMaintenanceEngineRoute();
  TestBackpressureEngineRoute();
  std::cout << "AEIC027_NOSQL_FAMILY_MAINTENANCE_ROUTE "
            << "AEIC027_NOSQL_BACKPRESSURE_DEBT_ROUTE ok\n";
  return EXIT_SUCCESS;
}
