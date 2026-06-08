// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/time_series_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

api::EngineRequestContext Context(api::EngineApiU64 tx = 76) {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_076_gate_api.sbdb";
  context.database_uuid.canonical = "019df076-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df076-0000-7000-8000-000000000076";
  context.local_transaction_id = tx;
  context.security_context_present = true;
  return context;
}

api::EngineTimeSeriesPhysicalProof TimeSeriesProof() {
  api::EngineTimeSeriesPhysicalProof proof;
  proof.proof_supplied = true;
  proof.time_meta_bucket_store_proof = true;
  proof.columnar_metric_page_proof = true;
  proof.summary_min_max_count_sum_proof = true;
  proof.rollup_materialization_proof = true;
  proof.late_arrival_delta_merge_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kTimeSeries;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = "odf076.local.time_series.provider";
  proof.provider_contract.fallback_provider_id = "none";
  proof.provider_contract.local_provider_available = true;
  proof.provider_contract.descriptor_visibility.proof_present = true;
  proof.provider_contract.descriptor_visibility.visible_to_snapshot = true;
  proof.provider_contract.descriptor_visibility.descriptor_shape_compatible = true;
  proof.provider_contract.security_redaction.proof_present = true;
  proof.provider_contract.security_redaction.redaction_policy_bound = true;
  proof.provider_contract.security_redaction.security_snapshot_bound = true;
  proof.provider_contract.index_generation.proof_present = true;
  proof.provider_contract.index_generation.visible_to_snapshot = true;
  proof.provider_contract.index_generation.covers_predicate = true;
  proof.provider_contract.index_generation.required_generation = 76;
  proof.provider_contract.index_generation.available_generation = 76;
  proof.provider_contract.index_generation.index_uuid = "odf076-ts-bucket-index";
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

std::vector<api::EngineTimeSeriesPoint> Points() {
  const std::vector<api::EngineTimeSeriesPointTag> east_tags = {
      {"host", "a"}, {"region", "east"}};
  const std::vector<api::EngineTimeSeriesPointTag> west_tags = {
      {"host", "b"}, {"region", "west"}};
  return {
      {10, "temp", 20.0, east_tags},
      {20, "temp", 22.0, east_tags},
      {30, "humidity", 0.4, east_tags},
      {65, "temp", 30.0, east_tags},
      {5, "temp", 18.0, east_tags},
      {15, "temp", 50.0, west_tags},
  };
}

api::EngineTimeSeriesAppendRequest BaseRequest() {
  api::EngineTimeSeriesAppendRequest request;
  request.context = Context();
  request.physical_append = true;
  request.points = Points();
  request.bucket_duration_ns = 60;
  request.rollup_intervals_ns = {120};
  request.late_arrival_watermark_ns = 10;
  request.late_arrival_policy =
      api::EngineTimeSeriesLateArrivalPolicy::kDeltaMergeReopen;
  request.physical_proof = TimeSeriesProof();
  return request;
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

void RequireEvidenceHygiene(const api::EngineApiResult& result) {
  for (const auto& item : result.evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts",
          "behavior_store_scan_selected=true", "descriptor_scan_selected=true",
          "local_descriptor_scan", "specialized_descriptor_fallback",
          "parser_executes_sql=true", "wal_recovery_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "parser_transaction_finality_authority=true",
          "client_autocommit_authority=true"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "ODF-076 evidence leaked forbidden authority or fallback token");
    }
  }
}

void BatchAppendBuildsBucketsColumnsSummariesRollupsAndLateDelta() {
  const auto result = api::EngineTimeSeriesAppend(BaseRequest());
  Require(result.ok, "ODF-076 physical time-series append failed");
  Require(result.dml_summary.rows_changed == 6,
          "ODF-076 append row count summary drifted");
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-076 physical append reported descriptor/behavior row scans");
  Require(RowMatches(result,
                     {{"row_kind", "bucket"},
                      {"meta_key", "host=a;region=east"},
                      {"bucket_start_ns", "0"},
                      {"bucket_end_ns", "60"},
                      {"late_arrival_count", "1"}}),
          "ODF-076 time+meta bucket selection row missing");
  Require(RowMatches(result,
                     {{"row_kind", "column_page"},
                      {"meta_key", "host=a;region=east"},
                      {"bucket_start_ns", "0"},
                      {"metric_name", "temp"},
                      {"column_layout", "metric_value_columnar_page"},
                      {"values", "20.000000,22.000000,18.000000"}}),
          "ODF-076 metric values were not stored column-wise by metric");
  Require(RowMatches(result,
                     {{"row_kind", "summary"},
                      {"meta_key", "host=a;region=east"},
                      {"bucket_start_ns", "0"},
                      {"metric_name", "temp"},
                      {"min", "18.000000"},
                      {"max", "22.000000"},
                      {"count", "3"},
                      {"sum", "60.000000"}}),
          "ODF-076 bucket summary min/max/count/sum drifted");
  Require(RowMatches(result,
                     {{"row_kind", "rollup"},
                      {"rollup_interval_ns", "120"},
                      {"rollup_start_ns", "0"},
                      {"meta_key", "host=a;region=east"},
                      {"metric_name", "temp"},
                      {"aggregate", "min_max_count_sum"},
                      {"count", "4"},
                      {"sum", "90.000000"}}),
          "ODF-076 rollup materialization row missing or incorrect");
  Require(RowMatches(result,
                     {{"row_kind", "late_arrival_delta"},
                      {"timestamp_ns", "5"},
                      {"metric_name", "temp"},
                      {"late_path", "delta_page_merge_reopen_bucket"},
                      {"merge_policy", "delta_merge_reopen"}}),
          "ODF-076 late-arrival point did not use delta/merge/reopen path");
  Require(EvidenceContains(result, "time_series_physical_access",
                           "local_time_meta_bucket_provider"),
          "ODF-076 physical provider evidence missing");
  Require(EvidenceContains(result, "time_series_bucket_selection",
                           "time_bucket_duration_ns=60"),
          "ODF-076 bucket selection evidence missing");
  Require(EvidenceContains(result, "time_series_columnar_metric_pages",
                           "metric_pages="),
          "ODF-076 columnar metric page evidence missing");
  Require(EvidenceContains(result, "time_series_summary_maintenance",
                           "min_max_count_sum"),
          "ODF-076 summary maintenance evidence missing");
  Require(EvidenceContains(result, "time_series_rollup_materialization",
                           "rows="),
          "ODF-076 rollup evidence missing");
  Require(EvidenceContains(result, "time_series_late_arrival_policy",
                           "delta_rows=1"),
          "ODF-076 late-arrival merge evidence missing");
  Require(EvidenceContains(result, "time_series_late_arrival_policy",
                           "sealed_columnar_rewrite=false"),
          "ODF-076 late-arrival path rewrote sealed columnar pages");
  Require(EvidenceContains(result, "mga_finality_authority",
                           "engine_transaction_inventory"),
          "ODF-076 MGA finality authority evidence missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-076 MGA recheck evidence missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-076 security recheck evidence missing");
  Require(!EvidenceContains(result, "behavior_store_scan_selected", "true"),
          "ODF-076 physical path selected behavior-store fallback");
  Require(!EvidenceContains(result, "descriptor_scan_selected", "true"),
          "ODF-076 physical path selected descriptor fallback");
  RequireEvidenceHygiene(result);
}

void MissingProofsFailClosedWithExactDiagnostics() {
  auto request = BaseRequest();
  request.physical_proof = {};
  auto result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 missing physical proof did not fail closed");
  Require(DiagnosticContains(result, api::kTimeSeriesPhysicalProofMissing),
          "ODF-076 missing physical proof diagnostic changed");

  const struct {
    const char* diagnostic;
    void (*mutate)(api::EngineTimeSeriesPhysicalProof*);
  } cases[] = {
      {api::kTimeSeriesBucketStoreProofMissing,
       [](api::EngineTimeSeriesPhysicalProof* proof) {
         proof->time_meta_bucket_store_proof = false;
       }},
      {api::kTimeSeriesColumnarMetricPageProofMissing,
       [](api::EngineTimeSeriesPhysicalProof* proof) {
         proof->columnar_metric_page_proof = false;
       }},
      {api::kTimeSeriesSummaryProofMissing,
       [](api::EngineTimeSeriesPhysicalProof* proof) {
         proof->summary_min_max_count_sum_proof = false;
       }},
      {api::kTimeSeriesRollupProofMissing,
       [](api::EngineTimeSeriesPhysicalProof* proof) {
         proof->rollup_materialization_proof = false;
       }},
      {api::kTimeSeriesLateArrivalPolicyProofMissing,
       [](api::EngineTimeSeriesPhysicalProof* proof) {
         proof->late_arrival_delta_merge_proof = false;
       }},
  };
  for (const auto& item : cases) {
    request = BaseRequest();
    item.mutate(&request.physical_proof);
    result = api::EngineTimeSeriesAppend(request);
    Require(!result.ok, "ODF-076 missing proof flag did not fail closed");
    Require(DiagnosticContains(result, item.diagnostic),
            "ODF-076 missing proof flag diagnostic changed");
  }
}

void RejectLateArrivalPolicyFailsClosed() {
  auto request = BaseRequest();
  request.late_arrival_policy = api::EngineTimeSeriesLateArrivalPolicy::kReject;
  const auto result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 reject late-arrival policy accepted late point");
  Require(DiagnosticContains(result, api::kTimeSeriesLateArrivalRejected),
          "ODF-076 reject late-arrival diagnostic changed");
}

void ProviderContractRefusalsFailClosed() {
  auto request = BaseRequest();
  request.physical_proof.provider_contract.descriptor_visibility
      .descriptor_scan_selected = true;
  auto result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok,
          "ODF-076 descriptor scan was accepted as physical time-series append");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderDescriptorScanNotPhysicalProvider),
          "ODF-076 descriptor scan refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.descriptor_visibility
      .behavior_store_scan_selected = true;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok,
          "ODF-076 behavior-store scan was accepted as physical append");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderBehaviorScanNotPhysicalProvider),
          "ODF-076 behavior-store scan refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.security_redaction.proof_present =
      false;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 missing security proof did not fail closed");
  Require(DiagnosticContains(result, api::kNoSqlProviderSecurityProofMissing),
          "ODF-076 missing security proof diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck.proof_present = false;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 missing MGA proof did not fail closed");
  Require(DiagnosticContains(result, api::kNoSqlProviderMgaRecheckProofMissing),
          "ODF-076 missing MGA proof diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .provider_claims_transaction_finality_authority = true;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 provider finality authority was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderFinalityAuthorityRefused),
          "ODF-076 provider finality refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .provider_claims_visibility_authority = true;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 provider visibility authority was accepted");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderVisibilityAuthorityRefused),
          "ODF-076 provider visibility refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .parser_claims_transaction_finality_authority = true;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 parser finality authority was accepted");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderParserFinalityAuthorityRefused),
          "ODF-076 parser finality refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .write_ahead_log_claims_transaction_finality_authority = true;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 write-ahead finality authority was accepted");
  Require(DiagnosticContains(
              result,
              api::kNoSqlProviderWriteAheadFinalityAuthorityRefused),
          "ODF-076 write-ahead finality refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.family =
      api::EngineNoSqlProviderFamily::kDocument;
  result = api::EngineTimeSeriesAppend(request);
  Require(!result.ok, "ODF-076 non-time-series provider was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderFamilyUnsupported),
          "ODF-076 provider family refusal diagnostic changed");
}

void LegacyEmptyRequestFallbackStillWorks() {
  api::EngineTimeSeriesAppendRequest request;
  request.context = Context(7600);
  request.target_object.uuid.canonical = "legacy-time-series";
  request.target_object.object_kind = "time_series";
  const auto result = api::EngineTimeSeriesAppend(request);
  Require(result.ok, "ODF-076 legacy empty append fallback failed");
  Require(RowMatches(result,
                     {{"object_kind", "time_series_point"},
                      {"state", "appended"}}),
          "ODF-076 legacy append result row changed");
  Require(EvidenceContains(result, "nosql_behavior",
                           "persisted_time_series_append"),
          "ODF-076 legacy append evidence changed");
}

}  // namespace

int main() {
  BatchAppendBuildsBucketsColumnsSummariesRollupsAndLateDelta();
  MissingProofsFailClosedWithExactDiagnostics();
  RejectLateArrivalPolicyFailsClosed();
  ProviderContractRefusalsFailClosed();
  LegacyEmptyRequestFallbackStillWorks();
  return EXIT_SUCCESS;
}
