// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-114 mixed-family fusion benchmark closure gate.

#include "candidate_set.hpp"
#include "candidate_set_executor.hpp"
#include "operator_fusion_executor.hpp"
#include "runtime_filter_pushdown.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

#ifndef ODF114_OUTPUT_JSON
#define ODF114_OUTPUT_JSON "optimizer_deficiency_odf_114_gate.json"
#endif

constexpr std::string_view kSblrPlanId =
    "odf114.sblr.mixed_family.fusion.plan.v1";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid RowUuid(platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(1710000114000ull);
  Require(generated.ok(), "ODF-114 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0x14;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(platform::UuidKind::row, generated.value);
  Require(typed.ok(), "ODF-114 typed row UUID creation failed");
  return typed.value;
}

idx::CandidateSetAuthorityContext Authority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

idx::CandidateSetRow Row(platform::byte suffix,
                         std::string source,
                         double score,
                         bool exact = true,
                         bool visible = true,
                         bool authorized = true,
                         bool payload = true) {
  idx::CandidateSetRow row;
  row.row_uuid = RowUuid(suffix);
  row.source = std::move(source);
  row.score = score;
  row.exact_predicate_match = exact;
  row.mga_visible = visible;
  row.security_authorized = authorized;
  row.exact_payload_available = payload;
  return row;
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence,
                            std::string_view scenario) {
  for (const auto& item : evidence) {
    for (const auto token : {"docs" "/execution-plans",
                             "docs" "/findings",
                             "public_release_evidence",
                             "docs/reference",
                             "execution_plan",
                             "findings",
                             "contracts",
                             "references",
                             "source_sql_text_authoritative=true",
                             "parser_finality_authority=true",
                             "provider_finality_authority=true",
                             "provider_finality_or_visibility_authority=true",
                             "client_finality_or_visibility_authority=true",
                             "write_ahead_log_finality_or_visibility_authority=true"}) {
      if (item.find(token) != std::string::npos) {
        std::cerr << "Forbidden ODF-114 evidence token in " << scenario << ": "
                  << item << '\n';
        Fail("ODF-114 evidence hygiene failed");
      }
    }
  }
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<unsigned>(
                         static_cast<unsigned char>(ch));
          out += escaped.str();
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

std::string Quote(std::string_view value) {
  return "\"" + JsonEscape(value) + "\"";
}

std::string StableHash(std::string_view input) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : input) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

struct ScenarioEvidence {
  std::string name;
  std::vector<std::string> families;
  std::string sblr_plan_id = std::string(kSblrPlanId);
  std::uint64_t fused_stage_count = 0;
  std::uint64_t materialization_count = 0;
  std::uint64_t materialization_barriers_avoided = 0;
  std::uint64_t candidate_rows = 0;
  std::uint64_t output_rows = 0;
  std::uint64_t exact_recheck_count = 0;
  std::uint64_t mga_recheck_count = 0;
  std::uint64_t security_recheck_count = 0;
  std::uint64_t runtime_filter_count = 0;
  bool benchmark_clean = true;
  bool live_speed_numbers = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::vector<std::pair<std::string, std::string>> proofs;
  std::string result_hash;
};

void AddProof(ScenarioEvidence* scenario,
              std::string key,
              std::string value) {
  scenario->proofs.emplace_back(std::move(key), std::move(value));
}

std::string Join(const std::vector<std::string>& values, char delimiter) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << delimiter;
    }
    out << values[i];
  }
  return out.str();
}

std::string ScenarioHashSeed(const ScenarioEvidence& scenario) {
  std::ostringstream seed;
  seed << scenario.name << '|' << Join(scenario.families, ',') << '|'
       << scenario.sblr_plan_id << '|' << scenario.fused_stage_count << '|'
       << scenario.materialization_count << '|'
       << scenario.materialization_barriers_avoided << '|'
       << scenario.candidate_rows << '|' << scenario.output_rows << '|'
       << scenario.exact_recheck_count << '|' << scenario.mga_recheck_count
       << '|' << scenario.security_recheck_count << '|'
       << scenario.runtime_filter_count << '|'
       << (scenario.benchmark_clean ? "true" : "false") << '|'
       << (scenario.live_speed_numbers ? "true" : "false") << '|'
       << (scenario.fail_closed ? "true" : "false") << '|'
       << scenario.diagnostic_code;
  for (const auto& proof : scenario.proofs) {
    seed << '|' << proof.first << '=' << proof.second;
  }
  return seed.str();
}

void FinalizeScenario(ScenarioEvidence* scenario) {
  scenario->result_hash = StableHash(ScenarioHashSeed(*scenario));
}

std::vector<exec::OperatorFusionStageKind> Stages(
    exec::OperatorFusionPipelineKind kind) {
  using Kind = exec::OperatorFusionPipelineKind;
  using Stage = exec::OperatorFusionStageKind;
  switch (kind) {
    case Kind::kScanFilterProject:
      return {Stage::kScan, Stage::kFilter, Stage::kProject};
    case Kind::kIndexVisibilityProject:
      return {Stage::kIndexProbe, Stage::kVisibilityRecheck, Stage::kProject};
    case Kind::kSearchScoreTopK:
      return {Stage::kSearchCandidate, Stage::kScore, Stage::kTopK};
    case Kind::kVectorRerank:
      return {Stage::kVectorCandidate, Stage::kRerank, Stage::kProject};
    case Kind::kGraphFrontier:
      return {Stage::kGraphSeed, Stage::kGraphFrontier, Stage::kProject};
    case Kind::kTimeAggregate:
      return {Stage::kTimeBucketScan, Stage::kAggregate, Stage::kProject};
    case Kind::kUnknown:
      break;
  }
  return {Stage::kUnknown};
}

exec::OperatorFusionPipelinePlan Plan(exec::OperatorFusionPipelineKind kind,
                                      std::string family,
                                      platform::u64 input_rows = 12) {
  exec::OperatorFusionPipelinePlan plan;
  plan.kind = kind;
  plan.pipeline_id = std::string(kSblrPlanId) + "." + family + "." +
                     exec::OperatorFusionPipelineKindName(kind);
  plan.plan_node_id = plan.pipeline_id + ".node";
  plan.provider_id = "provider." + family + ".physical";
  plan.descriptor_id = "descriptor." + family + ".fusion";
  plan.expected_row_descriptor_id = "row_descriptor.odf114." + family;
  plan.output_descriptor_id = "output_descriptor.odf114." + family;
  plan.stages = Stages(kind);
  plan.projected_column_names = {"row_uuid", "score", "family"};
  plan.descriptor_generation = 114;
  plan.required_descriptor_generation = 113;
  plan.input_rows = input_rows;
  plan.limit_k = 2;
  if (kind == exec::OperatorFusionPipelineKind::kSearchScoreTopK ||
      kind == exec::OperatorFusionPipelineKind::kVectorRerank) {
    plan.lossy_or_approximate = true;
    plan.exact_fallback_available = true;
  }
  return plan;
}

exec::OperatorFusionProviderResult ProviderRows(
    const exec::OperatorFusionProviderRequest& request,
    std::vector<idx::CandidateSetRow> rows,
    std::vector<std::string> proof_evidence = {}) {
  exec::OperatorFusionProviderResult result;
  result.status = {platform::StatusCode::ok, platform::Severity::info,
                   platform::Subsystem::engine};
  result.descriptor_generation = request.plan.descriptor_generation;
  result.row_descriptor_id = request.plan.expected_row_descriptor_id;
  result.projected_column_names = request.plan.projected_column_names;
  result.exact_recheck_evidence_present = true;
  result.mga_recheck_evidence_present = true;
  result.security_recheck_evidence_present = true;
  result.provider_authority_evidence_present = true;
  result.candidate_rows = std::move(rows);
  result.evidence.push_back("operator_fusion.provider=odf114_physical_provider");
  result.evidence.push_back("operator_fusion.provider_authority=physical_only");
  result.evidence.push_back("operator_fusion.provider_returns_final_rows=false");
  result.evidence.push_back("source_sql_text_authoritative=false");
  result.evidence.push_back("parser_finality_authority=false");
  result.evidence.push_back("provider_finality_authority=false");
  result.evidence.push_back(
      "operator_fusion.provider_finality_or_visibility_authority=false");
  for (auto& proof : proof_evidence) {
    result.evidence.push_back(std::move(proof));
  }
  return result;
}

exec::OperatorFusionProviderSet Providers(
    exec::OperatorFusionProvider primary,
    exec::RuntimeFilterProviderSet runtime_filter_providers = {}) {
  exec::OperatorFusionProviderSet providers;
  providers.primary_provider = std::move(primary);
  providers.exact_fallback_provider = providers.primary_provider;
  providers.runtime_filter_providers = std::move(runtime_filter_providers);
  providers.rerank_scorer = [](const idx::CandidateSetRow& row) {
    return 1000.0 - static_cast<double>(row.row_uuid.value.bytes[15]);
  };
  return providers;
}

exec::RuntimeFilterProviderResult RuntimeRows(
    const exec::RuntimeFilterProviderRequest& request,
    std::vector<idx::CandidateSetRow> rows) {
  exec::RuntimeFilterProviderResult result;
  result.status = {platform::StatusCode::ok, platform::Severity::info,
                   platform::Subsystem::engine};
  result.exact_recheck_evidence_present = true;
  result.mga_recheck_evidence_present = true;
  result.security_recheck_evidence_present = true;
  result.candidate_rows = std::move(rows);
  result.evidence.push_back("runtime_filter.provider=odf114_physical_filter");
  result.evidence.push_back(
      std::string("runtime_filter.provider_family=") +
      opt::RuntimeFilterFamilyName(request.descriptor.family));
  result.evidence.push_back("runtime_filter.provider_returns_final_rows=false");
  return result;
}

opt::RuntimeFilterDescriptor RuntimeDescriptor(
    opt::RuntimeFilterFamily family,
    opt::RuntimeFilterRoute route,
    std::string id,
    platform::u64 input_rows = 12,
    platform::u64 candidate_rows = 2,
    platform::u64 pruned_rows = 10) {
  opt::RuntimeFilterDescriptor descriptor;
  descriptor.family = family;
  descriptor.route = route;
  descriptor.filter_id = "odf114.runtime_filter." + std::move(id);
  descriptor.plan_node_id = std::string(kSblrPlanId) + ".runtime.node";
  descriptor.provider_id = "odf114.runtime.physical_provider";
  descriptor.predicate_digest = "odf114.runtime.digest";
  descriptor.descriptor_generation = 114;
  descriptor.required_descriptor_generation = 113;
  descriptor.input_rows = input_rows;
  descriptor.estimated_candidate_rows = candidate_rows;
  descriptor.estimated_pruned_rows = pruned_rows;
  descriptor.baseline_cost_units = input_rows * 10;
  descriptor.filter_cost_units = candidate_rows;
  descriptor.exact_recheck_cost_units = candidate_rows;
  descriptor.plan_shape_supported = true;
  descriptor.provider_supports_runtime_filters =
      route == opt::RuntimeFilterRoute::kProvider;
  descriptor.candidate_set_available = true;
  descriptor.security_context_present = true;
  descriptor.security_snapshot_bound = true;
  descriptor.grants_proven = true;
  descriptor.engine_mga_authoritative = true;
  descriptor.exact_recheck_available = true;
  descriptor.exact_fallback_available = true;
  descriptor.mga_visibility_recheck_required = true;
  descriptor.security_authorization_recheck_required = true;
  return descriptor;
}

void RequireSuccessfulFusion(const exec::OperatorFusionExecutionResult& result,
                             exec::OperatorFusionPipelineKind kind,
                             std::string_view scenario) {
  Require(result.ok(), "ODF-114 fusion pipeline failed");
  Require(result.candidate_rows.final_rows_authorized == false,
          "ODF-114 candidate rows became final before executor recheck");
  Require(result.counters.fused_stages == 3,
          "ODF-114 fused-stage counter changed");
  Require(result.counters.materialization_count == 0,
          "ODF-114 fusion materialized between stages");
  Require(result.counters.materialization_barriers_avoided > 0,
          "ODF-114 materialization barriers were not avoided");
  Require(result.counters.exact_recheck_count == result.counters.candidate_rows,
          "ODF-114 exact recheck counter did not cover candidates");
  Require(result.counters.mga_recheck_count == result.counters.candidate_rows,
          "ODF-114 MGA recheck counter did not cover candidates");
  Require(result.counters.security_recheck_count == result.counters.candidate_rows,
          "ODF-114 security recheck counter did not cover candidates");
  Require(result.output_batch.row_count == result.final_row_uuids.size(),
          "ODF-114 vectorized output batch count changed");
  Require(EvidenceHas(result.evidence,
                      std::string("operator_fusion.pipeline=") +
                          exec::OperatorFusionPipelineKindName(kind)),
          "ODF-114 pipeline-kind evidence missing");
  Require(EvidenceHas(result.evidence,
                      "operator_fusion.materializes_between_stages=false"),
          "ODF-114 materialization evidence missing");
  Require(EvidenceHas(result.evidence,
                      "result_batch.data_transport_only=true"),
          "ODF-114 result batch transport evidence missing");
  Require(EvidenceHas(result.evidence,
                      "mga_finality_authority=engine_transaction_inventory"),
          "ODF-114 MGA authority evidence missing");
  RequireEvidenceHygiene(result.evidence, scenario);
}

ScenarioEvidence PipelineScenario(
    std::string name,
    std::string family,
    exec::OperatorFusionPipelineKind kind,
    exec::OperatorFusionProvider provider,
    std::vector<std::string> required_evidence) {
  const auto plan = Plan(kind, family);
  const auto result =
      exec::ExecuteOperatorFusionPipeline(plan, Authority(), Providers(provider));
  RequireSuccessfulFusion(result, kind, name);
  for (const auto& token : required_evidence) {
    Require(EvidenceHas(result.evidence, token),
            "ODF-114 required pipeline proof evidence missing");
  }

  ScenarioEvidence scenario;
  scenario.name = std::move(name);
  scenario.families = {std::move(family)};
  scenario.fused_stage_count = result.counters.fused_stages;
  scenario.materialization_count = result.counters.materialization_count;
  scenario.materialization_barriers_avoided =
      result.counters.materialization_barriers_avoided;
  scenario.candidate_rows = result.counters.candidate_rows;
  scenario.output_rows = result.counters.output_rows;
  scenario.exact_recheck_count = result.counters.exact_recheck_count;
  scenario.mga_recheck_count = result.counters.mga_recheck_count;
  scenario.security_recheck_count = result.counters.security_recheck_count;
  scenario.runtime_filter_count = result.counters.runtime_filter_use_count;
  AddProof(&scenario,
           "pipeline_kind",
           exec::OperatorFusionPipelineKindName(kind));
  AddProof(&scenario,
           "operator_fusion.materializes_between_stages",
           "false");
  AddProof(&scenario, "result_batch.data_transport_only", "true");
  AddProof(&scenario,
           "executor_final_row_uuids",
           std::to_string(result.final_row_uuids.size()));
  AddProof(&scenario, "source_sql_text_authoritative", "false");
  AddProof(&scenario, "parser_finality_authority", "false");
  AddProof(&scenario, "provider_finality_authority", "false");
  AddProof(&scenario,
           "finality_authority",
           "engine_transaction_inventory");
  for (const auto& token : required_evidence) {
    AddProof(&scenario, "executor_evidence", token);
  }
  FinalizeScenario(&scenario);
  return scenario;
}

struct FusionResults {
  exec::OperatorFusionExecutionResult sql_scan;
  exec::OperatorFusionExecutionResult document_index;
  exec::OperatorFusionExecutionResult search_top_k;
  exec::OperatorFusionExecutionResult vector_rerank;
  exec::OperatorFusionExecutionResult graph_frontier;
  exec::OperatorFusionExecutionResult time_aggregate;
};

FusionResults BuildFusionResults() {
  FusionResults results;
  results.sql_scan = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "sql_scan"),
      Authority(),
      Providers([](const exec::OperatorFusionProviderRequest& request) {
        return ProviderRows(
            request,
            {Row(0x10, "sql.scan", 10.0),
             Row(0x11, "sql.filter", 9.0, false),
             Row(0x12, "sql.project", 8.0)},
            {"sql_scan_filter_project_candidate_input=true"});
      }));
  RequireSuccessfulFusion(results.sql_scan,
                          exec::OperatorFusionPipelineKind::kScanFilterProject,
                          "sql_scan_filter_project");

  results.document_index = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
           "document_index"),
      Authority(),
      Providers([](const exec::OperatorFusionProviderRequest& request) {
        return ProviderRows(
            request,
            {Row(0x20, "document.path_index", 20.0),
             Row(0x21, "document.visibility", 19.0, true, false),
             Row(0x22, "document.project", 18.0)},
            {"document_path_index_candidate_input=true",
             "document_index_visibility_candidate_input=true"});
      }));
  RequireSuccessfulFusion(
      results.document_index,
      exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
      "document_index_visibility_project");

  results.search_top_k = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kSearchScoreTopK, "search"),
      Authority(),
      Providers([](const exec::OperatorFusionProviderRequest& request) {
        return ProviderRows(
            request,
            {Row(0x30, "search.segment", 30.0),
             Row(0x31, "search.mutable", 25.0),
             Row(0x32, "search.low_score", 1.0)},
            {"search_candidate_input=segment_and_mutable_buffer"});
      }));
  RequireSuccessfulFusion(results.search_top_k,
                          exec::OperatorFusionPipelineKind::kSearchScoreTopK,
                          "search_score_top_k");
  Require(EvidenceHas(results.search_top_k.evidence, "top_k.action=score_prune"),
          "ODF-114 search TopK proof missing");

  results.vector_rerank = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kVectorRerank, "vector"),
      Authority(),
      Providers([](const exec::OperatorFusionProviderRequest& request) {
        return ProviderRows(
            request,
            {Row(0x40, "vector.ann", 40.0),
             Row(0x41, "vector.exact_payload", 39.0),
             Row(0x42, "vector.filtered", 38.0)},
            {"vector_candidate_input=tiered_ann",
             "vector_exact_payload_available=true"});
      }));
  RequireSuccessfulFusion(results.vector_rerank,
                          exec::OperatorFusionPipelineKind::kVectorRerank,
                          "vector_rerank");
  Require(EvidenceHas(results.vector_rerank.evidence,
                      "rerank.action=exact_payload_score"),
          "ODF-114 vector exact rerank proof missing");

  results.graph_frontier = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kGraphFrontier, "graph"),
      Authority(),
      Providers([](const exec::OperatorFusionProviderRequest& request) {
        return ProviderRows(
            request,
            {Row(0x50, "graph.seed", 50.0),
             Row(0x51, "graph.frontier", 49.0),
             Row(0x52, "graph.filtered", 48.0, true, true, false)},
            {"graph_frontier_candidate_input=adjacency_pages"});
      }));
  RequireSuccessfulFusion(results.graph_frontier,
                          exec::OperatorFusionPipelineKind::kGraphFrontier,
                          "graph_frontier");

  results.time_aggregate = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kTimeAggregate, "time_series"),
      Authority(),
      Providers([](const exec::OperatorFusionProviderRequest& request) {
        return ProviderRows(
            request,
            {Row(0x60, "time.bucket", 60.0),
             Row(0x61, "time.aggregate", 59.0),
             Row(0x62, "time.redacted", 58.0, true, true, false)},
            {"time_series_candidate_input=meta_bucket_column_pages"});
      }));
  RequireSuccessfulFusion(results.time_aggregate,
                          exec::OperatorFusionPipelineKind::kTimeAggregate,
                          "time_aggregate");

  return results;
}

std::vector<ScenarioEvidence> PipelineScenarios() {
  return {
      PipelineScenario(
          "sql_scan_filter_project_pipeline",
          "sql_scan",
          exec::OperatorFusionPipelineKind::kScanFilterProject,
          [](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(
                request,
                {Row(0x10, "sql.scan", 10.0),
                 Row(0x11, "sql.filter", 9.0, false),
                 Row(0x12, "sql.project", 8.0)},
                {"sql_scan_filter_project_candidate_input=true"});
          },
          {"sql_scan_filter_project_candidate_input=true"}),
      PipelineScenario(
          "document_index_visibility_project_pipeline",
          "document",
          exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
          [](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(
                request,
                {Row(0x20, "document.path_index", 20.0),
                 Row(0x21, "document.visibility", 19.0, true, false),
                 Row(0x22, "document.project", 18.0)},
                {"document_path_index_candidate_input=true",
                 "document_index_visibility_candidate_input=true"});
          },
          {"document_path_index_candidate_input=true",
           "document_index_visibility_candidate_input=true"}),
      PipelineScenario(
          "search_score_topk_pipeline",
          "search",
          exec::OperatorFusionPipelineKind::kSearchScoreTopK,
          [](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(
                request,
                {Row(0x30, "search.segment", 30.0),
                 Row(0x31, "search.mutable", 25.0),
                 Row(0x32, "search.low_score", 1.0)},
                {"search_candidate_input=segment_and_mutable_buffer"});
          },
          {"search_candidate_input=segment_and_mutable_buffer",
           "top_k.action=score_prune",
           "top_k.k=2"}),
      PipelineScenario(
          "vector_exact_rerank_pipeline",
          "vector",
          exec::OperatorFusionPipelineKind::kVectorRerank,
          [](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(
                request,
                {Row(0x40, "vector.ann", 40.0),
                 Row(0x41, "vector.exact_payload", 39.0),
                 Row(0x42, "vector.filtered", 38.0)},
                {"vector_candidate_input=tiered_ann",
                 "vector_exact_payload_available=true"});
          },
          {"vector_candidate_input=tiered_ann",
           "rerank.action=exact_payload_score"}),
      PipelineScenario(
          "graph_frontier_pipeline",
          "graph",
          exec::OperatorFusionPipelineKind::kGraphFrontier,
          [](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(
                request,
                {Row(0x50, "graph.seed", 50.0),
                 Row(0x51, "graph.frontier", 49.0),
                 Row(0x52, "graph.filtered", 48.0, true, true, false)},
                {"graph_frontier_candidate_input=adjacency_pages"});
          },
          {"graph_frontier_candidate_input=adjacency_pages"}),
      PipelineScenario(
          "time_series_aggregate_pipeline",
          "time_series",
          exec::OperatorFusionPipelineKind::kTimeAggregate,
          [](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(
                request,
                {Row(0x60, "time.bucket", 60.0),
                 Row(0x61, "time.aggregate", 59.0),
                 Row(0x62, "time.redacted", 58.0, true, true, false)},
                {"time_series_candidate_input=meta_bucket_column_pages"});
          },
          {"time_series_candidate_input=meta_bucket_column_pages"})};
}

ScenarioEvidence RuntimeFilterMixedScenario() {
  auto plan =
      Plan(exec::OperatorFusionPipelineKind::kGraphFrontier,
           "mixed_runtime_filter",
           16);
  plan.runtime_filters.push_back(RuntimeDescriptor(
      opt::RuntimeFilterFamily::kGraph, opt::RuntimeFilterRoute::kProvider,
      "graph"));
  plan.runtime_filters.push_back(RuntimeDescriptor(
      opt::RuntimeFilterFamily::kSearch, opt::RuntimeFilterRoute::kProvider,
      "search"));
  plan.runtime_filters.push_back(RuntimeDescriptor(
      opt::RuntimeFilterFamily::kVector, opt::RuntimeFilterRoute::kProvider,
      "vector"));
  plan.runtime_filters.push_back(RuntimeDescriptor(
      opt::RuntimeFilterFamily::kTimeSeries,
      opt::RuntimeFilterRoute::kProvider,
      "time_series"));
  plan.runtime_filters.push_back(RuntimeDescriptor(
      opt::RuntimeFilterFamily::kCandidateSet,
      opt::RuntimeFilterRoute::kScan,
      "candidate_set"));

  const auto common = Row(0x70, "mixed.common", 70.0);
  exec::RuntimeFilterProviderSet runtime_providers;
  runtime_providers.scan_provider =
      [common](const exec::RuntimeFilterProviderRequest& request) {
        return RuntimeRows(request,
                           {common, Row(0x78, "runtime.scan.extra", 1.0)});
      };
  runtime_providers.physical_provider =
      [common](const exec::RuntimeFilterProviderRequest& request) {
        return RuntimeRows(request,
                           {common, Row(0x79, "runtime.provider.extra", 1.0)});
      };
  runtime_providers.exact_fallback_provider =
      [common](const exec::RuntimeFilterProviderRequest& request) {
        return RuntimeRows(request,
                           {common, Row(0x7A, "runtime.fallback.extra", 1.0)});
      };

  const auto result = exec::ExecuteOperatorFusionPipeline(
      plan,
      Authority(),
      Providers(
          [common](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(
                request,
                {common, Row(0x71, "mixed.provider.only", 2.0)},
                {"mixed_family_candidate_input=graph_search_vector_time_series"});
          },
          runtime_providers));
  RequireSuccessfulFusion(result,
                          exec::OperatorFusionPipelineKind::kGraphFrontier,
                          "mixed_runtime_filter");
  Require(result.counters.runtime_filter_use_count == 5,
          "ODF-114 mixed runtime-filter count changed");
  Require(result.counters.candidate_rows == 1,
          "ODF-114 mixed runtime-filter did not intersect candidates");
  Require(EvidenceHas(result.evidence, "runtime_filter.executor=pushdown_v1"),
          "ODF-114 runtime filter executor evidence missing");
  Require(EvidenceHas(result.evidence,
                      "operator_fusion.runtime_filter_applied=true"),
          "ODF-114 runtime filter fusion evidence missing");
  Require(EvidenceHas(result.evidence, "operation=intersect"),
          "ODF-114 runtime filter candidate intersect evidence missing");

  ScenarioEvidence scenario;
  scenario.name = "mixed_runtime_filter_pushdown_intersection";
  scenario.families = {"graph", "search", "vector", "time_series",
                       "candidate_set"};
  scenario.fused_stage_count = result.counters.fused_stages;
  scenario.materialization_count = result.counters.materialization_count;
  scenario.materialization_barriers_avoided =
      result.counters.materialization_barriers_avoided;
  scenario.candidate_rows = result.counters.candidate_rows;
  scenario.output_rows = result.counters.output_rows;
  scenario.exact_recheck_count = result.counters.exact_recheck_count;
  scenario.mga_recheck_count = result.counters.mga_recheck_count;
  scenario.security_recheck_count = result.counters.security_recheck_count;
  scenario.runtime_filter_count = result.counters.runtime_filter_use_count;
  AddProof(&scenario, "runtime_filter.executor", "pushdown_v1");
  AddProof(&scenario, "operator_fusion.runtime_filter_applied", "true");
  AddProof(&scenario,
           "operator_fusion.materializes_between_stages",
           "false");
  AddProof(&scenario, "result_batch.data_transport_only", "true");
  AddProof(&scenario, "candidate_algebra", "intersect");
  AddProof(&scenario,
           "executor_final_row_uuids",
           std::to_string(result.final_row_uuids.size()));
  AddProof(&scenario, "source_sql_text_authoritative", "false");
  AddProof(&scenario, "parser_finality_authority", "false");
  AddProof(&scenario, "provider_finality_authority", "false");
  AddProof(&scenario,
           "finality_authority",
           "engine_transaction_inventory");
  FinalizeScenario(&scenario);
  return scenario;
}

ScenarioEvidence MixedSblrCandidateAlgebraScenario(
    const FusionResults& results) {
  auto authority = Authority();
  auto sql_document = idx::UnionCandidateSets(results.sql_scan.candidate_rows,
                                             results.document_index.candidate_rows,
                                             authority);
  Require(sql_document.ok(), "ODF-114 SQL/document candidate union failed");
  auto search_vector = idx::UnionCandidateSets(
      results.search_top_k.candidate_rows,
      results.vector_rerank.candidate_rows,
      authority);
  Require(search_vector.ok(), "ODF-114 search/vector candidate union failed");
  auto graph_time = idx::UnionCandidateSets(
      results.graph_frontier.candidate_rows,
      results.time_aggregate.candidate_rows,
      authority);
  Require(graph_time.ok(), "ODF-114 graph/time candidate union failed");
  auto document_search =
      idx::UnionCandidateSets(sql_document.output, search_vector.output,
                              authority);
  Require(document_search.ok(),
          "ODF-114 SQL/document/search/vector candidate union failed");
  auto all_candidates =
      idx::UnionCandidateSets(document_search.output, graph_time.output,
                              authority);
  Require(all_candidates.ok(), "ODF-114 all-family candidate union failed");

  std::vector<idx::CandidateSetRow> shared_rows = {
      Row(0x10, "exact.shared.sql", 100.0),
      Row(0x20, "exact.shared.document", 99.0),
      Row(0x30, "exact.shared.search", 98.0),
      Row(0x40, "exact.shared.vector", 97.0),
      Row(0x50, "exact.shared.graph", 96.0),
      Row(0x60, "exact.shared.time", 95.0)};
  auto exact_subset =
      idx::MakeExactRowUuidOrderedCandidateSet(shared_rows, authority, false);
  Require(exact_subset.ok(), "ODF-114 exact subset candidate build failed");
  auto intersected =
      idx::IntersectCandidateSets(all_candidates.output, exact_subset.output,
                                  authority);
  Require(intersected.ok(), "ODF-114 mixed candidate intersection failed");
  auto top_k = idx::TopKCandidateSet(intersected.output, 4, authority);
  Require(top_k.ok(), "ODF-114 mixed candidate TopK failed");
  auto reranked = idx::RerankCandidateSet(
      top_k.output,
      [](const idx::CandidateSetRow& row) {
        return 1000.0 - static_cast<double>(row.row_uuid.value.bytes[15]);
      },
      authority);
  Require(reranked.ok(), "ODF-114 mixed candidate rerank failed");
  Require(!reranked.output.final_rows_authorized,
          "ODF-114 mixed algebra returned final rows before executor finalize");
  auto finalized =
      exec::FinalizeCandidateSetForExecutor(reranked.output, authority);
  Require(finalized.ok(), "ODF-114 mixed candidate finalization failed");
  Require(finalized.final_row_uuids.size() == 4,
          "ODF-114 mixed final row UUID count changed");
  Require(EvidenceHas(sql_document.evidence, "operation=union"),
          "ODF-114 union evidence missing");
  Require(EvidenceHas(intersected.evidence, "operation=intersect"),
          "ODF-114 intersect evidence missing");
  Require(EvidenceHas(top_k.evidence, "top_k.action=score_prune"),
          "ODF-114 topK evidence missing");
  Require(EvidenceHas(reranked.evidence, "rerank.action=exact_payload_score"),
          "ODF-114 rerank evidence missing");
  Require(EvidenceHas(finalized.evidence,
                      "executor.final_result_requires_mga_recheck=true"),
          "ODF-114 final executor MGA recheck evidence missing");
  RequireEvidenceHygiene(finalized.evidence, "mixed_candidate_algebra");

  ScenarioEvidence scenario;
  scenario.name = "single_sblr_plan_mixed_family_candidate_algebra";
  scenario.families = {"sql_scan", "document", "search", "vector", "graph",
                       "time_series"};
  scenario.fused_stage_count = results.sql_scan.counters.fused_stages +
                               results.document_index.counters.fused_stages +
                               results.search_top_k.counters.fused_stages +
                               results.vector_rerank.counters.fused_stages +
                               results.graph_frontier.counters.fused_stages +
                               results.time_aggregate.counters.fused_stages;
  scenario.materialization_count =
      results.sql_scan.counters.materialization_count +
      results.document_index.counters.materialization_count +
      results.search_top_k.counters.materialization_count +
      results.vector_rerank.counters.materialization_count +
      results.graph_frontier.counters.materialization_count +
      results.time_aggregate.counters.materialization_count;
  scenario.materialization_barriers_avoided =
      results.sql_scan.counters.materialization_barriers_avoided +
      results.document_index.counters.materialization_barriers_avoided +
      results.search_top_k.counters.materialization_barriers_avoided +
      results.vector_rerank.counters.materialization_barriers_avoided +
      results.graph_frontier.counters.materialization_barriers_avoided +
      results.time_aggregate.counters.materialization_barriers_avoided;
  scenario.candidate_rows = reranked.output.rows.size();
  scenario.output_rows = finalized.final_row_uuids.size();
  scenario.exact_recheck_count = reranked.output.rows.size();
  scenario.mga_recheck_count = reranked.output.rows.size();
  scenario.security_recheck_count = reranked.output.rows.size();
  AddProof(&scenario, "candidate_algebra", "union");
  AddProof(&scenario, "candidate_algebra", "intersect");
  AddProof(&scenario, "candidate_algebra", "topK");
  AddProof(&scenario, "candidate_algebra", "rerank");
  AddProof(&scenario, "candidate_algebra", "finalize");
  AddProof(&scenario,
           "operator_fusion.materializes_between_stages",
           "false");
  AddProof(&scenario, "result_batch.data_transport_only", "true");
  AddProof(&scenario, "executor_final_row_uuids",
           std::to_string(finalized.final_row_uuids.size()));
  AddProof(&scenario, "source_sql_text_authoritative", "false");
  AddProof(&scenario, "parser_finality_authority", "false");
  AddProof(&scenario, "provider_finality_authority", "false");
  AddProof(&scenario,
           "finality_authority",
           "engine_transaction_inventory");
  FinalizeScenario(&scenario);
  return scenario;
}

void RequireRefusal(exec::OperatorFusionPipelinePlan plan,
                    exec::OperatorFusionProvider provider,
                    std::string_view diagnostic) {
  const auto result =
      exec::ExecuteOperatorFusionPipeline(plan, Authority(), Providers(provider));
  Require(!result.ok() && result.fail_closed,
          "ODF-114 unsafe fusion case was accepted");
  Require(result.diagnostic_code == diagnostic,
          "ODF-114 fusion refusal diagnostic changed");
  RequireEvidenceHygiene(result.evidence, diagnostic);
}

void RequireRuntimeRefusal(opt::RuntimeFilterDescriptor descriptor,
                           std::string_view diagnostic) {
  auto plan =
      Plan(exec::OperatorFusionPipelineKind::kGraphFrontier,
           "runtime_refusal",
           12);
  plan.runtime_filters.push_back(std::move(descriptor));
  const auto result = exec::ExecuteOperatorFusionPipeline(
      plan,
      Authority(),
      Providers(
          [](const exec::OperatorFusionProviderRequest& request) {
            return ProviderRows(request, {Row(0x70, "runtime.refusal", 1.0)});
          },
          {[](const exec::RuntimeFilterProviderRequest& request) {
             return RuntimeRows(request, {Row(0x70, "runtime.scan", 1.0)});
           },
           [](const exec::RuntimeFilterProviderRequest& request) {
             return RuntimeRows(request, {Row(0x70, "runtime.provider", 1.0)});
           },
           [](const exec::RuntimeFilterProviderRequest& request) {
             return RuntimeRows(request, {Row(0x70, "runtime.fallback", 1.0)});
           }}));
  Require(!result.ok() && result.fail_closed,
          "ODF-114 unsafe runtime-filter case was accepted");
  Require(result.diagnostic_code == diagnostic,
          "ODF-114 runtime-filter diagnostic changed");
  RequireEvidenceHygiene(result.evidence, diagnostic);
}

ScenarioEvidence FailClosedMatrixScenario() {
  const auto safe_provider = [](const exec::OperatorFusionProviderRequest& request) {
    return ProviderRows(request, {Row(0x80, "fail.closed.safe", 1.0)});
  };

  auto plan =
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "refusal");
  plan.descriptor_scan_selected = true;
  RequireRefusal(plan,
                 safe_provider,
                 "SB_OPERATOR_FUSION.PHYSICAL_PROVIDER_REQUIRED");

  plan = Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "refusal");
  plan.behavior_store_scan_selected = true;
  RequireRefusal(plan,
                 safe_provider,
                 "SB_OPERATOR_FUSION.PHYSICAL_PROVIDER_REQUIRED");

  plan = Plan(exec::OperatorFusionPipelineKind::kGraphFrontier, "refusal");
  plan.stale = true;
  RequireRefusal(plan, safe_provider, "SB_OPERATOR_FUSION.STALE_DESCRIPTOR");

  RequireRefusal(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "refusal"),
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x81, "missing.exact", 1.0)});
        result.exact_recheck_evidence_present = false;
        return result;
      },
      "SB_OPERATOR_FUSION.EXACT_RECHECK_EVIDENCE_REQUIRED");

  RequireRefusal(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "refusal"),
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x82, "missing.mga", 1.0)});
        result.mga_recheck_evidence_present = false;
        return result;
      },
      "SB_OPERATOR_FUSION.MGA_RECHECK_EVIDENCE_REQUIRED");

  RequireRefusal(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "refusal"),
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result =
            ProviderRows(request, {Row(0x83, "missing.security", 1.0)});
        result.security_recheck_evidence_present = false;
        return result;
      },
      "SB_OPERATOR_FUSION.SECURITY_RECHECK_EVIDENCE_REQUIRED");

  RequireRefusal(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "refusal"),
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x84, "final.rows", 1.0)});
        result.returns_final_rows = true;
        result.mga_recheck_evidence_present = false;
        return result;
      },
      "SB_OPERATOR_FUSION.PROVIDER_FINAL_ROWS_RECHECK_REQUIRED");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
              "refusal");
  plan.parser_or_donor_finality_or_visibility_authority = true;
  RequireRefusal(plan, safe_provider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
              "refusal");
  plan.client_finality_or_visibility_authority = true;
  RequireRefusal(plan, safe_provider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
              "refusal");
  plan.provider_finality_or_visibility_authority = true;
  RequireRefusal(plan, safe_provider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
              "refusal");
  plan.write_ahead_log_finality_or_visibility_authority = true;
  RequireRefusal(plan, safe_provider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  RequireRefusal(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject, "refusal"),
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x85, "provider.authority", 1.0)});
        result.provider_finality_or_visibility_authority = true;
        return result;
      },
      "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  auto runtime_descriptor = RuntimeDescriptor(
      opt::RuntimeFilterFamily::kSearch,
      opt::RuntimeFilterRoute::kProvider,
      "unsafe_authority");
  runtime_descriptor.provider_finality_or_visibility_authority = true;
  RequireRuntimeRefusal(runtime_descriptor,
                        "SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY");

  runtime_descriptor = RuntimeDescriptor(
      opt::RuntimeFilterFamily::kVector,
      opt::RuntimeFilterRoute::kProvider,
      "missing_candidate_set");
  runtime_descriptor.candidate_set_available = false;
  RequireRuntimeRefusal(runtime_descriptor,
                        "SB_RUNTIME_FILTER_EXECUTOR.CANDIDATE_SET_REQUIRED");

  runtime_descriptor = RuntimeDescriptor(
      opt::RuntimeFilterFamily::kTimeSeries,
      opt::RuntimeFilterRoute::kProvider,
      "no_exact_fallback");
  runtime_descriptor.lossy_or_false_negative_possible = true;
  runtime_descriptor.exact_fallback_available = false;
  RequireRuntimeRefusal(runtime_descriptor,
                        "SB_RUNTIME_FILTER_EXECUTOR.EXACT_FALLBACK_REQUIRED");

  runtime_descriptor = RuntimeDescriptor(
      opt::RuntimeFilterFamily::kCandidateSet,
      opt::RuntimeFilterRoute::kScan,
      "descriptor_route");
  runtime_descriptor.descriptor_scan_selected = true;
  RequireRuntimeRefusal(runtime_descriptor,
                        "SB_RUNTIME_FILTER_EXECUTOR.PHYSICAL_ROUTE_REQUIRED");

  ScenarioEvidence scenario;
  scenario.name = "mixed_family_fusion_fail_closed_matrix";
  scenario.families = {"sql_scan", "document", "search", "vector", "graph",
                       "time_series", "runtime_filter"};
  scenario.fail_closed = true;
  scenario.diagnostic_code = "matrix";
  AddProof(&scenario,
           "fusion_descriptor_scan_selected",
           "SB_OPERATOR_FUSION.PHYSICAL_PROVIDER_REQUIRED");
  AddProof(&scenario,
           "fusion_behavior_store_scan_selected",
           "SB_OPERATOR_FUSION.PHYSICAL_PROVIDER_REQUIRED");
  AddProof(&scenario,
           "fusion_stale_descriptor",
           "SB_OPERATOR_FUSION.STALE_DESCRIPTOR");
  AddProof(&scenario,
           "fusion_missing_exact_recheck",
           "SB_OPERATOR_FUSION.EXACT_RECHECK_EVIDENCE_REQUIRED");
  AddProof(&scenario,
           "fusion_missing_mga_recheck",
           "SB_OPERATOR_FUSION.MGA_RECHECK_EVIDENCE_REQUIRED");
  AddProof(&scenario,
           "fusion_missing_security_recheck",
           "SB_OPERATOR_FUSION.SECURITY_RECHECK_EVIDENCE_REQUIRED");
  AddProof(&scenario,
           "fusion_provider_final_rows_without_recheck",
           "SB_OPERATOR_FUSION.PROVIDER_FINAL_ROWS_RECHECK_REQUIRED");
  AddProof(&scenario,
           "fusion_unsafe_authority",
           "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");
  AddProof(&scenario,
           "runtime_filter_unsafe_authority",
           "SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY");
  AddProof(&scenario,
           "runtime_filter_no_candidate_set",
           "SB_RUNTIME_FILTER_EXECUTOR.CANDIDATE_SET_REQUIRED");
  AddProof(&scenario,
           "runtime_filter_no_exact_fallback",
           "SB_RUNTIME_FILTER_EXECUTOR.EXACT_FALLBACK_REQUIRED");
  AddProof(&scenario,
           "runtime_filter_descriptor_route",
           "SB_RUNTIME_FILTER_EXECUTOR.PHYSICAL_ROUTE_REQUIRED");
  AddProof(&scenario, "source_sql_text_authoritative", "false");
  AddProof(&scenario, "parser_finality_authority", "false");
  AddProof(&scenario, "provider_finality_authority", "false");
  AddProof(&scenario,
           "finality_authority",
           "engine_transaction_inventory");
  FinalizeScenario(&scenario);
  return scenario;
}

void WriteJson(const std::vector<ScenarioEvidence>& scenarios) {
  const std::filesystem::path output_path = ODF114_OUTPUT_JSON;
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream json(output_path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(json), "ODF-114 could not open JSON output file");
  json << "{\n";
  json << "  \"gate\": \"optimizer_deficiency_odf_114_gate\",\n";
  json << "  \"odf\": \"ODF-114\",\n";
  json << "  \"sblr_plan_id\": " << Quote(kSblrPlanId) << ",\n";
  json << "  \"scenario_count\": " << scenarios.size() << ",\n";
  json << "  \"benchmark_clean\": true,\n";
  json << "  \"live_speed_numbers\": false,\n";
  json << "  \"source_sql_text_authoritative\": false,\n";
  json << "  \"parser_finality_authority\": false,\n";
  json << "  \"provider_finality_authority\": false,\n";
  json << "  \"finality_authority\": \"engine_transaction_inventory\",\n";
  json << "  \"families\": [\"sql_scan\", \"document\", \"search\", \"vector\", \"graph\", \"time_series\", \"runtime_filter\", \"candidate_set\"],\n";
  json << "  \"scenarios\": [\n";
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const auto& scenario = scenarios[index];
    json << "    {\n";
    json << "      \"name\": " << Quote(scenario.name) << ",\n";
    json << "      \"families_included\": [";
    for (std::size_t family_index = 0; family_index < scenario.families.size();
         ++family_index) {
      if (family_index != 0) {
        json << ", ";
      }
      json << Quote(scenario.families[family_index]);
    }
    json << "],\n";
    json << "      \"sblr_plan_id\": " << Quote(scenario.sblr_plan_id) << ",\n";
    json << "      \"fused_stage_count\": " << scenario.fused_stage_count << ",\n";
    json << "      \"materialization_count\": "
         << scenario.materialization_count << ",\n";
    json << "      \"materialization_barriers_avoided\": "
         << scenario.materialization_barriers_avoided << ",\n";
    json << "      \"materializes_between_stages\": false,\n";
    json << "      \"candidate_rows\": " << scenario.candidate_rows << ",\n";
    json << "      \"output_rows\": " << scenario.output_rows << ",\n";
    json << "      \"exact_recheck_count\": "
         << scenario.exact_recheck_count << ",\n";
    json << "      \"mga_recheck_count\": " << scenario.mga_recheck_count
         << ",\n";
    json << "      \"security_recheck_count\": "
         << scenario.security_recheck_count << ",\n";
    json << "      \"runtime_filter_count\": "
         << scenario.runtime_filter_count << ",\n";
    json << "      \"benchmark_clean\": "
         << (scenario.benchmark_clean ? "true" : "false") << ",\n";
    json << "      \"live_speed_numbers\": "
         << (scenario.live_speed_numbers ? "true" : "false") << ",\n";
    json << "      \"fail_closed\": "
         << (scenario.fail_closed ? "true" : "false") << ",\n";
    json << "      \"diagnostic_code\": " << Quote(scenario.diagnostic_code)
         << ",\n";
    json << "      \"result_hash\": " << Quote(scenario.result_hash) << ",\n";
    json << "      \"proofs\": [\n";
    for (std::size_t proof_index = 0; proof_index < scenario.proofs.size();
         ++proof_index) {
      const auto& [key, value] = scenario.proofs[proof_index];
      json << "        {\"key\": " << Quote(key) << ", \"value\": "
           << Quote(value) << "}";
      if (proof_index + 1 != scenario.proofs.size()) {
        json << ',';
      }
      json << '\n';
    }
    json << "      ]\n";
    json << "    }";
    if (index + 1 != scenarios.size()) {
      json << ',';
    }
    json << '\n';
  }
  json << "  ]\n";
  json << "}\n";
  json.flush();
  Require(static_cast<bool>(json), "ODF-114 JSON output write failed");

  std::ifstream verify(output_path, std::ios::binary);
  std::ostringstream buffer;
  buffer << verify.rdbuf();
  const std::string text = buffer.str();
  for (const auto token : {"docs" "/execution-plans",
                           "docs" "/findings",
                           "public_release_evidence",
                           "docs/reference",
                           "execution_plan",
                           "findings",
                           "contracts",
                           "references"}) {
    Require(text.find(token) == std::string::npos,
            "ODF-114 JSON leaked forbidden documentation token");
  }
}

}  // namespace

int main() {
  std::vector<ScenarioEvidence> scenarios = PipelineScenarios();
  const auto fusion_results = BuildFusionResults();
  scenarios.push_back(RuntimeFilterMixedScenario());
  scenarios.push_back(MixedSblrCandidateAlgebraScenario(fusion_results));
  scenarios.push_back(FailClosedMatrixScenario());
  WriteJson(scenarios);
  return EXIT_SUCCESS;
}
