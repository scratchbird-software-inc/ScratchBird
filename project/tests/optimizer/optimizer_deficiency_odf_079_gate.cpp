// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_backpressure_debt_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_079_gate_api.sbdb";
  context.database_uuid.canonical = "019df079-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df079-0000-7000-8000-000000000079";
  context.local_transaction_id = 79;
  context.security_context_present = true;
  return context;
}

api::EngineNoSqlBackpressureDebtEntry Entry(
    api::EngineNoSqlProviderFamily family,
    api::EngineNoSqlBackpressureDebtKind kind,
    std::string suffix) {
  api::EngineNoSqlBackpressureDebtEntry entry;
  entry.family = family;
  entry.debt_kind = kind;
  entry.object_uuid = "019df079-0000-7000-8000-" + std::move(suffix);
  entry.result_id = "result-" + entry.object_uuid;
  entry.evidence_epoch = 100;
  entry.required_epoch = 100;
  entry.debt_units = 3;
  entry.observed_cost_units = 40;
  entry.budget_cost_units = 50;
  entry.evidence_authoritative = true;
  return entry;
}

std::vector<api::EngineNoSqlProviderFamily> Families() {
  return {api::EngineNoSqlProviderFamily::kKeyValue,
          api::EngineNoSqlProviderFamily::kDocument,
          api::EngineNoSqlProviderFamily::kSearch,
          api::EngineNoSqlProviderFamily::kVector,
          api::EngineNoSqlProviderFamily::kGraph,
          api::EngineNoSqlProviderFamily::kTimeSeries};
}

std::vector<api::EngineNoSqlBackpressureDebtKind> DebtKinds() {
  return {api::EngineNoSqlBackpressureDebtKind::kRefresh,
          api::EngineNoSqlBackpressureDebtKind::kMergeCompaction,
          api::EngineNoSqlBackpressureDebtKind::kGenerationBuild,
          api::EngineNoSqlBackpressureDebtKind::kPayloadPolicy,
          api::EngineNoSqlBackpressureDebtKind::kSlowdownBackpressurePolicy,
          api::EngineNoSqlBackpressureDebtKind::kStrictBulkRedirectPolicy,
          api::EngineNoSqlBackpressureDebtKind::kResultSuppressionPolicy};
}

api::EnginePlanNoSqlBackpressureDebtRequest BaseRequest() {
  api::EnginePlanNoSqlBackpressureDebtRequest request;
  request.context = Context();
  request.now_microseconds = 790000;
  request.engine_mga_authoritative = true;
  request.request_context_authoritative = true;
  request.security_snapshot_bound = true;
  request.grants_proven = true;
  request.row_mga_recheck_required = true;
  request.row_security_recheck_required = true;

  int suffix = 100001;
  for (const auto family : Families()) {
    for (const auto debt_kind : DebtKinds()) {
      auto entry = Entry(family, debt_kind, std::to_string(suffix++));
      if (debt_kind ==
          api::EngineNoSqlBackpressureDebtKind::kResultSuppressionPolicy) {
        switch (family) {
          case api::EngineNoSqlProviderFamily::kKeyValue:
          case api::EngineNoSqlProviderFamily::kVector:
            entry.evidence_epoch = 99;
            entry.required_epoch = 100;
            break;
          case api::EngineNoSqlProviderFamily::kDocument:
          case api::EngineNoSqlProviderFamily::kGraph:
            entry.observed_cost_units = 70;
            entry.budget_cost_units = 50;
            break;
          case api::EngineNoSqlProviderFamily::kSearch:
          case api::EngineNoSqlProviderFamily::kTimeSeries:
            entry.unsafe_result = true;
            break;
          case api::EngineNoSqlProviderFamily::kSpatial:
          case api::EngineNoSqlProviderFamily::kColumnar:
          case api::EngineNoSqlProviderFamily::kUnknown:
            break;
        }
      }
      request.entries.push_back(std::move(entry));
    }
  }
  return request;
}

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) {
    return {};
  }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) {
      return value.encoded_value;
    }
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
    if (matched) {
      return true;
    }
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
                                 "parser_executes_sql=true",
                                 "client_autocommit_authority=true",
                                 "wal_recovery_authority=true"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-079 evidence leaked forbidden documentation or authority token");
    }
  }
}

void FamilyDebtLedgerPlansAllDebtKindsAndSuppressesResultsExplicitly() {
  const auto result = api::EnginePlanNoSqlBackpressureDebt(BaseRequest());
  Require(result.ok, "ODF-079 authoritative NoSQL backpressure plan failed");
  Require(result.dml_summary.benchmark_clean,
          "ODF-079 result was not benchmark clean");
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-079 used descriptor or behavior scans as debt authority");
  Require(result.agent_result.ledger_rows.size() == 42,
          "ODF-079 did not ledger seven debt kinds for all six families");
  Require(result.agent_result.actions.size() == 36,
          "ODF-079 did not plan six non-suppression debt actions per family");
  Require(result.agent_result.suppressions.size() == 6,
          "ODF-079 did not suppress one unsafe/stale/over-budget result per family");

  Require(EvidenceContains(result, "nosql_surface", "backpressure_debt"),
          "ODF-079 missing NoSQL surface evidence");
  Require(EvidenceContains(result, "nosql_backpressure_debt",
                           "ledger_row_count=42"),
          "ODF-079 missing ledger count evidence");
  Require(EvidenceContains(result, "parser_executes_sql", "false"),
          "ODF-079 parser SQL authority guard evidence missing");
  Require(EvidenceContains(result, "wal_recovery_authority", "false"),
          "ODF-079 WAL authority guard evidence missing");
  Require(EvidenceContains(result, "provider_transaction_finality_authority",
                           "false"),
          "ODF-079 provider finality guard evidence missing");
  Require(EvidenceContains(result, "provider_visibility_authority", "false"),
          "ODF-079 provider visibility guard evidence missing");
  Require(EvidenceContains(result, "client_autocommit_authority", "false"),
          "ODF-079 client autocommit authority guard evidence missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-079 row MGA recheck evidence missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-079 row security recheck evidence missing");

  const struct {
    const char* family;
    const char* suppression_code;
  } expected_families[] = {
      {"key_value", agents::kNoSqlBackpressureDebtResultStale},
      {"document", agents::kNoSqlBackpressureDebtResultOverBudget},
      {"search", agents::kNoSqlBackpressureDebtResultUnsafe},
      {"vector", agents::kNoSqlBackpressureDebtResultStale},
      {"graph", agents::kNoSqlBackpressureDebtResultOverBudget},
      {"time_series", agents::kNoSqlBackpressureDebtResultUnsafe},
  };
  const char* debt_kinds[] = {"refresh",
                              "merge_compaction",
                              "generation_build",
                              "payload_policy",
                              "slowdown_backpressure_policy",
                              "strict_bulk_redirect_policy",
                              "result_suppression_policy"};
  std::set<std::string> families;
  std::set<std::string> kinds;
  for (const auto& item : expected_families) {
    families.insert(item.family);
    for (const auto debt_kind : debt_kinds) {
      kinds.insert(debt_kind);
      Require(RowMatches(result,
                         {{"row_kind", "debt_ledger"},
                          {"family", item.family},
                          {"debt_kind", debt_kind}}),
              "ODF-079 missing family debt ledger row");
    }
    Require(RowMatches(result,
                       {{"row_kind", "result_suppression"},
                        {"family", item.family},
                        {"diagnostic_code", item.suppression_code},
                        {"result_returned", "false"}}),
            "ODF-079 missing exact result suppression row");
    Require(DiagnosticContains(result, item.suppression_code),
            "ODF-079 missing exact result suppression diagnostic");
  }
  Require(families.size() == 6,
          "ODF-079 did not cover all six physical NoSQL families");
  Require(kinds.size() == 7,
          "ODF-079 did not cover all seven required debt kinds");

  Require(RowMatches(result,
                     {{"row_kind", "debt_plan_action"},
                      {"debt_kind", "slowdown_backpressure_policy"},
                      {"action_kind", "apply_slowdown_backpressure_policy"},
                      {"policy_kind", "slowdown_backpressure_policy"}}),
          "ODF-079 did not plan slowdown/backpressure policy");
  Require(RowMatches(result,
                     {{"row_kind", "debt_plan_action"},
                      {"debt_kind", "strict_bulk_redirect_policy"},
                      {"action_kind", "redirect_strict_bulk_to_deferred_bulk"},
                      {"policy_kind", "strict_bulk_redirect_policy"}}),
          "ODF-079 did not plan strict bulk redirect policy");
  RequireEvidenceHygiene(result);
}

void SecurityClusterAndAuthorityGuardsFailClosed() {
  auto request = BaseRequest();
  request.context.security_context_present = false;
  auto result = api::EnginePlanNoSqlBackpressureDebt(request);
  Require(!result.ok, "ODF-079 missing security context did not fail closed");
  Require(DiagnosticContains(result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "ODF-079 security context diagnostic changed");

  request = BaseRequest();
  request.option_envelopes.push_back("cluster.authority=required");
  result = api::EnginePlanNoSqlBackpressureDebt(request);
  Require(!result.ok && result.cluster_authority_required,
          "ODF-079 cluster-scoped debt route did not require cluster authority");
  Require(DiagnosticContains(result, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE"),
          "ODF-079 cluster authority diagnostic changed");

  request = BaseRequest();
  request.provider_claims_transaction_finality_authority = true;
  result = api::EnginePlanNoSqlBackpressureDebt(request);
  Require(!result.ok, "ODF-079 provider finality authority did not fail closed");
  Require(DiagnosticContains(result,
                             agents::kNoSqlBackpressureDebtUnsafeAuthority),
          "ODF-079 unsafe authority diagnostic changed");

  request = BaseRequest();
  request.entries.front().evidence_authoritative = false;
  result = api::EnginePlanNoSqlBackpressureDebt(request);
  Require(!result.ok,
          "ODF-079 non-authoritative debt evidence did not fail closed");
  Require(DiagnosticContains(
              result,
              agents::kNoSqlBackpressureDebtEvidenceNotAuthoritative),
          "ODF-079 non-authoritative debt diagnostic changed");
}

void SblrDispatchRouteReachesBackpressureDebtApi() {
  const auto* opcode =
      sblr::LookupSblrOperation("nosql.backpressure_debt_plan");
  Require(opcode != nullptr,
          "ODF-079 SBLR opcode registry entry missing");
  Require(opcode->requires_security_context,
          "ODF-079 SBLR opcode does not require security context");
  Require(opcode->requires_transaction_context,
          "ODF-079 SBLR opcode does not require transaction context");

  sblr::SblrDispatchRequest request;
  request.context = Context();
  request.envelope = sblr::MakeSblrEnvelope(
      "nosql.backpressure_debt_plan",
      "SBLR_NOSQL_BACKPRESSURE_DEBT_PLAN",
      "ODF-079");

  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.accepted && result.envelope_validated &&
              result.dispatched_to_api,
          "ODF-079 SBLR route did not dispatch to API");
  Require(result.api_result.operation_id == "nosql.backpressure_debt_plan",
          "ODF-079 SBLR route reached the wrong API operation");
  Require(DiagnosticContains(result.api_result,
                             agents::kNoSqlBackpressureDebtUnsafeAuthority),
          "ODF-079 SBLR route did not execute API authority guards");
}

}  // namespace

int main() {
  FamilyDebtLedgerPlansAllDebtKindsAndSuppressesResultsExplicitly();
  SecurityClusterAndAuthorityGuardsFailClosed();
  SblrDispatchRouteReachesBackpressureDebtApi();
  return EXIT_SUCCESS;
}
