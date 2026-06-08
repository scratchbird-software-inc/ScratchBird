// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_family_maintenance_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace implemented_agents = scratchbird::core::agents::implemented_agents;
namespace api = scratchbird::engine::internal_api;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "ODF-077 UUID generation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction, 77000 + local_id),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "ODF-077 transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry Entry(platform::u64 local_id,
                                     mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = 1779507700000ull + local_id;
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = entry.begin_unix_epoch_millis + 1;
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest(
    bool inventory_complete = true,
    platform::u64 next_local_transaction_id = 61) {
  mga::AuthoritativeCleanupHorizonRequest request;
  for (platform::u64 local_id = 1; local_id < next_local_transaction_id; ++local_id) {
    request.inventory.entries.push_back(
        Entry(local_id, mga::TransactionState::committed));
  }
  request.inventory.next_local_transaction_id = next_local_transaction_id;
  request.inventory_authoritative = true;
  request.inventory_complete = inventory_complete;
  request.active_snapshot_inventory_authoritative = true;
  return request;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_077_gate_api.sbdb";
  context.database_uuid.canonical = "019df077-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df077-0000-7000-8000-000000000077";
  context.local_transaction_id = 77;
  context.security_context_present = true;
  return context;
}

api::EngineNoSqlMaintenanceGenerationCandidate Candidate(
    api::EngineNoSqlProviderFamily family,
    std::string generation_id,
    platform::u64 superseded_tx,
    platform::u64 expires_after_tx = 0) {
  api::EngineNoSqlMaintenanceGenerationCandidate candidate;
  candidate.family = family;
  candidate.generation_id = std::move(generation_id);
  candidate.generation_kind = api::EngineNoSqlProviderFamilyName(family);
  candidate.sealed_local_transaction_id = superseded_tx == 0 ? 0 : superseded_tx - 1;
  candidate.superseded_local_transaction_id = superseded_tx;
  candidate.expires_after_local_transaction_id = expires_after_tx;
  candidate.estimated_bytes = 4096;
  candidate.generation_evidence_authoritative = true;
  candidate.ttl_evidence_authoritative = expires_after_tx != 0;
  return candidate;
}

agents::DynamicCleanupDebtSchedulerPolicy Policy() {
  agents::DynamicCleanupDebtSchedulerPolicy policy;
  policy.max_total_work_units = 16;
  policy.max_scheduled_items = 8;
  policy.default_max_family_work_units = 2;
  policy.default_max_family_items = 2;
  policy.max_work_units_per_item = 1;
  policy.lease_duration_microseconds = 1000;
  return policy;
}

api::EnginePlanNoSqlFamilyMaintenanceRequest BaseRequest() {
  api::EnginePlanNoSqlFamilyMaintenanceRequest request;
  request.context = Context();
  request.horizon_request = HorizonRequest();
  request.scheduler_policy = Policy();
  request.now_microseconds = 770000;
  request.engine_mga_authoritative = true;
  request.execute_plan = false;
  request.candidates = {
      Candidate(api::EngineNoSqlProviderFamily::kKeyValue, "kv-gen-a", 12, 10),
      Candidate(api::EngineNoSqlProviderFamily::kDocument, "doc-gen-a", 20),
      Candidate(api::EngineNoSqlProviderFamily::kSearch, "search-gen-a", 22),
      Candidate(api::EngineNoSqlProviderFamily::kVector, "vector-gen-a", 24),
      Candidate(api::EngineNoSqlProviderFamily::kGraph, "graph-gen-a", 26),
      Candidate(api::EngineNoSqlProviderFamily::kTimeSeries, "ts-gen-a", 28),
  };
  return request;
}

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool RowMatches(const api::EngineApiResult& result,
                std::initializer_list<std::pair<std::string_view,
                                                std::string_view>> fields) {
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    bool matched = true;
    for (const auto& [field, expected] : fields) {
      if (RowField(result, i, field) != expected) {
        matched = false;
        break;
      }
    }
    if (matched) { return true; }
  }
  return false;
}

bool DiagnosticContains(const api::EngineApiResult& result,
                        std::string_view token) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(token) != std::string::npos ||
        diagnostic.detail.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind.find(kind) != std::string::npos &&
        item.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const api::EngineApiResult& result) {
  std::vector<std::string> values;
  for (const auto& item : result.evidence) {
    values.push_back(item.evidence_kind);
    values.push_back(item.evidence_id);
  }
  for (const auto& diagnostic : result.diagnostics) {
    values.push_back(diagnostic.code);
    values.push_back(diagnostic.message_key);
    values.push_back(diagnostic.detail);
  }
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    for (const auto& [name, value] : result.result_shape.rows[i].fields) {
      values.push_back(name);
      values.push_back(value.encoded_value);
    }
  }
  for (const auto& value : values) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references",
                                 "behavior_store_scan_selected=true",
                                 "descriptor_scan_selected=true",
                                 "provider_transaction_finality_authority=true",
                                 "provider_visibility_authority=true",
                                 "parser_transaction_finality_authority=true",
                                 "client_autocommit_authority=true"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-077 evidence leaked forbidden documentation or authority token");
    }
  }
}

void SchedulesFamilySpecificNoSqlMaintenanceThroughDebtScheduler() {
  const auto result = api::EnginePlanNoSqlFamilyMaintenance(BaseRequest());
  Require(result.ok, "ODF-077 authoritative NoSQL maintenance plan failed");
  Require(result.agent_result.scheduler_result.ok(),
          "ODF-077 dynamic cleanup scheduler refused NoSQL maintenance sources");
  Require(result.agent_result.scheduler_result.scheduled_count == 7,
          "ODF-077 did not schedule KV TTL plus six family generation actions");
  Require(result.agent_result.actions.size() == 7,
          "ODF-077 action count did not match scheduler assignments");
  Require(EvidenceContains(result, "nosql_family_maintenance",
                           "authority_source=durable_mga_transaction_inventory"),
          "ODF-077 missing MGA inventory authority evidence");
  Require(EvidenceContains(result, "dynamic_cleanup_debt_scheduler",
                           "scheduled_count=7"),
          "ODF-077 missing scheduler integration evidence");
  Require(EvidenceContains(result, "nosql_surface", "family_maintenance"),
          "ODF-077 missing NoSQL surface evidence");
  Require(EvidenceContains(result, "parser_executes_sql", "false"),
          "ODF-077 parser SQL authority guard evidence missing");
  Require(EvidenceContains(result, "wal_recovery_authority", "false"),
          "ODF-077 WAL authority guard evidence missing");
  Require(EvidenceContains(result, "provider_transaction_finality_authority",
                           "false"),
          "ODF-077 provider finality authority guard evidence missing");
  Require(EvidenceContains(result, "provider_visibility_authority", "false"),
          "ODF-077 provider visibility authority guard evidence missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-077 row MGA recheck evidence missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-077 row security recheck evidence missing");

  const struct {
    const char* family;
    const char* action_kind;
    const char* scheduler_family;
    const char* scheduler_work_kind;
  } expected[] = {
      {"key_value", "kv_ttl_expired_record_retirement",
       "nosql_key_value", "nosql_key_value_ttl_retirement"},
      {"key_value", "kv_lsm_generation_compaction",
       "nosql_key_value", "nosql_key_value_generation_compaction"},
      {"document", "document_shape_fragment_generation_merge",
       "nosql_document", "nosql_document_generation_merge"},
      {"search", "search_segment_merge_and_retirement",
       "nosql_search", "nosql_search_segment_merge"},
      {"vector", "vector_index_generation_retirement",
       "nosql_vector", "nosql_vector_generation_retirement"},
      {"graph", "graph_adjacency_generation_compaction",
       "nosql_graph", "nosql_graph_adjacency_compaction"},
      {"time_series", "time_series_bucket_generation_merge_retirement",
       "nosql_time_series", "nosql_time_series_bucket_retirement"},
  };
  std::set<std::string> action_families;
  std::set<std::string> scheduler_families;
  for (const auto& item : expected) {
    Require(RowMatches(result,
                       {{"row_kind", "maintenance_action"},
                        {"family", item.family},
                        {"action_kind", item.action_kind},
                        {"policy_kind",
                         item.action_kind ==
                                 std::string("kv_ttl_expired_record_retirement")
                             ? "ttl_retirement_below_mga_cleanup_horizon"
                             : "generation_retirement_below_mga_cleanup_horizon"},
                        {"cleanup_horizon_local_transaction_id", "61"},
                        {"executed", "false"}}),
            "ODF-077 missing family-specific maintenance action");
    Require(RowMatches(result,
                       {{"row_kind", "dynamic_cleanup_assignment"},
                        {"cleanup_debt_family", item.scheduler_family},
                        {"cleanup_debt_work_kind", item.scheduler_work_kind},
                        {"decision", "scheduled"}}),
            "ODF-077 action was not represented as scheduler debt");
    action_families.insert(item.family);
    scheduler_families.insert(item.scheduler_family);
  }
  Require(action_families.size() == 6,
          "ODF-077 did not cover all six physical NoSQL families");
  Require(scheduler_families.size() == 6,
          "ODF-077 scheduler families were not NoSQL-family-specific");
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-077 maintenance planning used descriptor or behavior scans");
  RequireEvidenceHygiene(result);
}

void MissingMgaHorizonEvidenceFailsClosedWithExactDiagnostic() {
  auto request = BaseRequest();
  request.horizon_request = HorizonRequest(false);
  const auto result = api::EnginePlanNoSqlFamilyMaintenance(request);
  Require(!result.ok, "ODF-077 missing cleanup horizon evidence did not fail closed");
  Require(DiagnosticContains(result, "SB-MGA-CLEANUP-HORIZON-INVENTORY-MISSING"),
          "ODF-077 missing cleanup horizon diagnostic changed");

  request = BaseRequest();
  request.engine_mga_authoritative = false;
  const auto missing_authority = api::EnginePlanNoSqlFamilyMaintenance(request);
  Require(!missing_authority.ok,
          "ODF-077 missing engine MGA authority did not fail closed");
  Require(DiagnosticContains(
              missing_authority,
              implemented_agents::kNoSqlMaintenanceCleanupHorizonNotAuthoritative),
          "ODF-077 missing engine authority diagnostic changed");
}

void UnsafeGenerationsAndTtlAreSuppressedByMgaHorizon() {
  auto request = BaseRequest();
  request.candidates = {
      Candidate(api::EngineNoSqlProviderFamily::kSearch, "search-too-new", 61),
  };
  auto result = api::EnginePlanNoSqlFamilyMaintenance(request);
  Require(result.ok, "ODF-077 safe suppression should not fail closed");
  Require(result.agent_result.actions.empty(),
          "ODF-077 scheduled generation at cleanup horizon");
  Require(RowMatches(result,
                     {{"row_kind", "maintenance_suppression"},
                      {"family", "search"},
                      {"generation_id", "search-too-new"},
                      {"diagnostic_code",
                       implemented_agents::kNoSqlMaintenanceGenerationNotBelowMgaHorizon},
                      {"cleanup_horizon_local_transaction_id", "61"},
                      {"governing_local_transaction_id", "61"}}),
          "ODF-077 stale generation suppression evidence missing");

  request = BaseRequest();
  request.candidates = {
      Candidate(api::EngineNoSqlProviderFamily::kKeyValue, "kv-ttl-too-new", 0, 61),
  };
  result = api::EnginePlanNoSqlFamilyMaintenance(request);
  Require(result.ok, "ODF-077 TTL horizon suppression should not fail closed");
  Require(result.agent_result.actions.empty(),
          "ODF-077 scheduled TTL retirement at cleanup horizon");
  Require(RowMatches(result,
                     {{"row_kind", "maintenance_suppression"},
                      {"family", "key_value"},
                      {"generation_id", "kv-ttl-too-new"},
                      {"diagnostic_code",
                       implemented_agents::kNoSqlMaintenanceTtlNotBelowMgaHorizon},
                      {"cleanup_horizon_local_transaction_id", "61"},
                      {"governing_local_transaction_id", "61"}}),
          "ODF-077 TTL horizon suppression evidence missing");
}

void NonAuthoritativeGenerationAndTtlEvidenceFailClosed() {
  auto request = BaseRequest();
  request.candidates.front().generation_evidence_authoritative = false;
  auto result = api::EnginePlanNoSqlFamilyMaintenance(request);
  Require(!result.ok,
          "ODF-077 non-authoritative generation evidence did not fail closed");
  Require(DiagnosticContains(
              result,
              implemented_agents::kNoSqlMaintenanceGenerationEvidenceNotAuthoritative),
          "ODF-077 generation authority diagnostic changed");

  request = BaseRequest();
  request.candidates = {
      Candidate(api::EngineNoSqlProviderFamily::kKeyValue, "kv-ttl-unsafe", 0, 10),
  };
  request.candidates.front().ttl_evidence_authoritative = false;
  result = api::EnginePlanNoSqlFamilyMaintenance(request);
  Require(!result.ok,
          "ODF-077 non-authoritative TTL evidence did not fail closed");
  Require(DiagnosticContains(
              result,
              implemented_agents::kNoSqlMaintenanceTtlEvidenceNotAuthoritative),
          "ODF-077 TTL authority diagnostic changed");
}

void ClusterScopedMaintenanceFailsClosedThroughNoSqlGuard() {
  auto request = BaseRequest();
  request.option_envelopes.push_back("cluster");
  const auto result = api::EnginePlanNoSqlFamilyMaintenance(request);
  Require(!result.ok && result.cluster_authority_required,
          "ODF-077 cluster-scoped maintenance did not require cluster authority");
  Require(DiagnosticContains(result, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE"),
          "ODF-077 cluster authority diagnostic changed");
}

}  // namespace

int main() {
  SchedulesFamilySpecificNoSqlMaintenanceThroughDebtScheduler();
  MissingMgaHorizonEvidenceFailsClosedWithExactDiagnostic();
  UnsafeGenerationsAndTtlAreSuppressedByMgaHorizon();
  NonAuthoritativeGenerationAndTtlEvidenceFailClosed();
  ClusterScopedMaintenanceFailsClosedThroughNoSqlGuard();
  return EXIT_SUCCESS;
}
