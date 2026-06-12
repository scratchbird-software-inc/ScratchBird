// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-096 operator fusion executor gate.

#include "operator_fusion_executor.hpp"
#include "runtime_filter_pushdown.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid V7(platform::UuidKind kind,
                       platform::u64 unix_epoch_millis,
                       platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(unix_epoch_millis);
  Require(generated.ok(), "ODF-096 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0x96;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "ODF-096 typed UUIDv7 creation failed");
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

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "provider_finality_or_visibility_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "parser_executes_sql=true",
          "client_finality_or_visibility_authority=true",
          "write_ahead_log_finality_or_visibility_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-096 evidence leaked forbidden document or authority token");
    }
  }
}

idx::CandidateSetRow Row(platform::byte suffix,
                         bool exact = true,
                         bool visible = true,
                         bool authorized = true) {
  idx::CandidateSetRow row;
  row.row_uuid = V7(platform::UuidKind::row, 1710000096000ull, suffix);
  row.score = static_cast<double>(suffix);
  row.exact_predicate_match = exact;
  row.mga_visible = visible;
  row.security_authorized = authorized;
  row.exact_payload_available = true;
  row.source = "odf096_operator_fusion";
  return row;
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

exec::OperatorFusionPipelinePlan Plan(exec::OperatorFusionPipelineKind kind) {
  exec::OperatorFusionPipelinePlan plan;
  plan.kind = kind;
  plan.pipeline_id =
      std::string("odf096.") + exec::OperatorFusionPipelineKindName(kind);
  plan.plan_node_id = "plan_node." + plan.pipeline_id;
  plan.provider_id = "provider." + plan.pipeline_id;
  plan.descriptor_id = "descriptor." + plan.pipeline_id;
  plan.expected_row_descriptor_id = "row_descriptor.odf096";
  plan.output_descriptor_id = "output_descriptor.odf096";
  plan.stages = Stages(kind);
  plan.projected_column_names = {"row_uuid", "score"};
  plan.descriptor_generation = 11;
  plan.required_descriptor_generation = 10;
  plan.input_rows = 100;
  plan.limit_k = 2;
  if (kind == exec::OperatorFusionPipelineKind::kSearchScoreTopK ||
      kind == exec::OperatorFusionPipelineKind::kVectorRerank) {
    plan.lossy_or_approximate = true;
  }
  return plan;
}

exec::OperatorFusionProviderResult ProviderRows(
    const exec::OperatorFusionProviderRequest& request,
    std::vector<idx::CandidateSetRow> rows) {
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
  result.evidence.push_back("operator_fusion.provider=odf096_memory_provider");
  result.evidence.push_back("operator_fusion.provider_authority=physical_only");
  result.evidence.push_back("operator_fusion.provider_returns_final_rows=false");
  result.evidence.push_back(
      "operator_fusion.provider_finality_or_visibility_authority=false");
  return result;
}

exec::OperatorFusionProviderResult PrimaryProvider(
    const exec::OperatorFusionProviderRequest& request) {
  return ProviderRows(request, {Row(0x10), Row(0x11), Row(0x12, false)});
}

exec::OperatorFusionProviderResult RuntimePrimaryProvider(
    const exec::OperatorFusionProviderRequest& request) {
  return ProviderRows(request, {Row(0x21), Row(0x22)});
}

exec::RuntimeFilterProviderResult RuntimeFilterProvider(
    const exec::RuntimeFilterProviderRequest&) {
  exec::RuntimeFilterProviderResult result;
  result.status = {platform::StatusCode::ok, platform::Severity::info,
                   platform::Subsystem::engine};
  result.exact_recheck_evidence_present = true;
  result.mga_recheck_evidence_present = true;
  result.security_recheck_evidence_present = true;
  result.candidate_rows = {Row(0x21), Row(0x23)};
  result.evidence.push_back("runtime_filter.provider=odf096_runtime_provider");
  result.evidence.push_back("runtime_filter.provider_returns_final_rows=false");
  return result;
}

exec::OperatorFusionProviderSet Providers(
    exec::OperatorFusionProvider primary = PrimaryProvider) {
  exec::OperatorFusionProviderSet providers;
  providers.primary_provider = primary;
  providers.exact_fallback_provider = PrimaryProvider;
  providers.runtime_filter_providers = {RuntimeFilterProvider,
                                        RuntimeFilterProvider,
                                        RuntimeFilterProvider};
  providers.rerank_scorer = [](const idx::CandidateSetRow& row) {
    return row.score + 1000.0;
  };
  return providers;
}

opt::RuntimeFilterDescriptor RuntimeDescriptor() {
  opt::RuntimeFilterDescriptor descriptor;
  descriptor.family = opt::RuntimeFilterFamily::kSearch;
  descriptor.route = opt::RuntimeFilterRoute::kProvider;
  descriptor.filter_id = "odf096.runtime_filter";
  descriptor.plan_node_id = "odf096.runtime_filter.node";
  descriptor.provider_id = "odf096.runtime_filter.provider";
  descriptor.predicate_digest = "odf096.runtime_filter.digest";
  descriptor.descriptor_generation = 12;
  descriptor.required_descriptor_generation = 10;
  descriptor.input_rows = 100;
  descriptor.estimated_candidate_rows = 2;
  descriptor.estimated_pruned_rows = 98;
  descriptor.baseline_cost_units = 1000;
  descriptor.filter_cost_units = 10;
  descriptor.exact_recheck_cost_units = 2;
  descriptor.plan_shape_supported = true;
  descriptor.provider_supports_runtime_filters = true;
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

void SupportedPipelinesAvoidMaterialization() {
  const std::vector<exec::OperatorFusionPipelineKind> kinds = {
      exec::OperatorFusionPipelineKind::kScanFilterProject,
      exec::OperatorFusionPipelineKind::kIndexVisibilityProject,
      exec::OperatorFusionPipelineKind::kSearchScoreTopK,
      exec::OperatorFusionPipelineKind::kVectorRerank,
      exec::OperatorFusionPipelineKind::kGraphFrontier,
      exec::OperatorFusionPipelineKind::kTimeAggregate};

  for (const auto kind : kinds) {
    const auto result =
        exec::ExecuteOperatorFusionPipeline(Plan(kind), Authority(), Providers());
    Require(result.ok(), "ODF-096 supported fusion pipeline failed");
    Require(result.counters.input_rows == 100,
            "ODF-096 input-row counter changed");
    Require(result.counters.fused_stages == 3,
            "ODF-096 fused-stage counter changed");
    Require(result.counters.materialization_barriers_avoided == 2,
            "ODF-096 materialization avoidance counter changed");
    Require(result.counters.materialization_count == 0,
            "ODF-096 unnecessary materialization occurred");
    Require(result.counters.provider_authority_evidence_count == 1,
            "ODF-096 provider authority evidence counter missing");
    Require(result.counters.exact_recheck_count == result.counters.candidate_rows,
            "ODF-096 exact recheck counter missing");
    Require(result.counters.mga_recheck_count == result.counters.candidate_rows,
            "ODF-096 MGA recheck counter missing");
    Require(result.counters.security_recheck_count ==
                result.counters.candidate_rows,
            "ODF-096 security recheck counter missing");
    Require(result.output_batch.row_count == result.final_row_uuids.size(),
            "ODF-096 vectorized result batch row count changed");
    Require(EvidenceHas(result.evidence,
                        "operator_fusion.materializes_between_stages=false"),
            "ODF-096 materialization avoidance evidence missing");
    Require(EvidenceHas(result.evidence,
                        "result_batch.data_transport_only=true"),
            "ODF-096 ODF-091 vectorized batch evidence missing");
    Require(EvidenceHas(result.evidence,
                        "mga_finality_authority=engine_transaction_inventory"),
            "ODF-096 MGA authority evidence missing");
    RequireEvidenceHygiene(result.evidence);
  }
}

void RuntimeFilterFusionUsesOdf095AndIntersectsCandidates() {
  auto plan = Plan(exec::OperatorFusionPipelineKind::kScanFilterProject);
  plan.runtime_filters.push_back(RuntimeDescriptor());
  const auto result = exec::ExecuteOperatorFusionPipeline(
      plan, Authority(), Providers(RuntimePrimaryProvider));
  Require(result.ok(), "ODF-096 runtime-filter fusion failed");
  Require(result.counters.runtime_filter_use_count == 1,
          "ODF-096 runtime-filter counter missing");
  Require(result.counters.candidate_rows == 1,
          "ODF-096 runtime-filter candidate intersection changed");
  Require(result.counters.output_rows == 1,
          "ODF-096 runtime-filter output row count changed");
  Require(EvidenceHas(result.evidence, "runtime_filter.executor=pushdown_v1"),
          "ODF-096 ODF-095 runtime filter evidence missing");
  Require(EvidenceHas(result.evidence,
                      "operator_fusion.runtime_filter_applied=true"),
          "ODF-096 runtime-filter fusion evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void ProviderFinalRowsAreDemotedOnlyWithRecheckEvidence() {
  const auto final_provider = [](const exec::OperatorFusionProviderRequest& request) {
    auto result = ProviderRows(request, {Row(0x31), Row(0x32)});
    result.returns_final_rows = true;
    return result;
  };
  const auto accepted = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject),
      Authority(), Providers(final_provider));
  Require(accepted.ok(),
          "ODF-096 provider-final rows with engine recheck evidence failed");
  Require(EvidenceHas(accepted.evidence,
                      "provider_final_rows_demoted_to_candidates=true"),
          "ODF-096 provider-final demotion evidence missing");

  const auto unsafe_final_provider =
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x33)});
        result.returns_final_rows = true;
        result.mga_recheck_evidence_present = false;
        return result;
      };
  const auto refused = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject),
      Authority(), Providers(unsafe_final_provider));
  Require(!refused.ok() && refused.fail_closed,
          "ODF-096 provider-final rows without recheck were accepted");
  Require(refused.diagnostic_code ==
              "SB_OPERATOR_FUSION.PROVIDER_FINAL_ROWS_RECHECK_REQUIRED",
          "ODF-096 provider-final refusal diagnostic changed");
  RequireEvidenceHygiene(refused.evidence);
}

void RequireRefusal(exec::OperatorFusionPipelinePlan plan,
                    exec::OperatorFusionProvider provider,
                    std::string_view diagnostic) {
  const auto result =
      exec::ExecuteOperatorFusionPipeline(plan, Authority(), Providers(provider));
  Require(!result.ok() && result.fail_closed,
          "ODF-096 unsafe fusion plan/provider was accepted");
  Require(result.diagnostic_code == diagnostic,
          "ODF-096 fail-closed diagnostic changed");
  RequireEvidenceHygiene(result.evidence);
}

void FailClosedForUnsafePlansAndProviders() {
  auto plan = Plan(exec::OperatorFusionPipelineKind::kScanFilterProject);
  plan.stages = {exec::OperatorFusionStageKind::kScan,
                 exec::OperatorFusionStageKind::kProject};
  RequireRefusal(plan, PrimaryProvider, "SB_OPERATOR_FUSION.UNSUPPORTED_PIPELINE");

  plan = Plan(exec::OperatorFusionPipelineKind::kGraphFrontier);
  plan.stale = true;
  RequireRefusal(plan, PrimaryProvider, "SB_OPERATOR_FUSION.STALE_DESCRIPTOR");

  plan = Plan(exec::OperatorFusionPipelineKind::kSearchScoreTopK);
  plan.lossy_or_approximate = true;
  plan.exact_fallback_available = false;
  RequireRefusal(plan, PrimaryProvider,
                 "SB_OPERATOR_FUSION.EXACT_FALLBACK_REQUIRED");

  plan = Plan(exec::OperatorFusionPipelineKind::kTimeAggregate);
  plan.redaction_barrier_crossed = true;
  plan.redaction_barrier_proven = false;
  RequireRefusal(plan, PrimaryProvider,
                 "SB_OPERATOR_FUSION.REDACTION_BARRIER_REQUIRED");

  plan = Plan(exec::OperatorFusionPipelineKind::kTimeAggregate);
  plan.security_barrier_crossed = true;
  plan.security_barrier_proven = false;
  RequireRefusal(plan, PrimaryProvider,
                 "SB_OPERATOR_FUSION.SECURITY_BARRIER_REQUIRED");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject);
  plan.parser_or_reference_finality_or_visibility_authority = true;
  RequireRefusal(plan, PrimaryProvider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject);
  plan.client_finality_or_visibility_authority = true;
  RequireRefusal(plan, PrimaryProvider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject);
  plan.provider_finality_or_visibility_authority = true;
  RequireRefusal(plan, PrimaryProvider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  plan = Plan(exec::OperatorFusionPipelineKind::kIndexVisibilityProject);
  plan.write_ahead_log_finality_or_visibility_authority = true;
  RequireRefusal(plan, PrimaryProvider, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  plan = Plan(exec::OperatorFusionPipelineKind::kScanFilterProject);
  plan.engine_mga_authoritative = false;
  RequireRefusal(plan, PrimaryProvider,
                 "SB_OPERATOR_FUSION.MGA_RECHECK_REQUIRED");

  plan = Plan(exec::OperatorFusionPipelineKind::kScanFilterProject);
  plan.security_context_bound = false;
  RequireRefusal(plan, PrimaryProvider,
                 "SB_OPERATOR_FUSION.SECURITY_RECHECK_REQUIRED");

  const auto descriptor_mismatch =
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x41)});
        result.row_descriptor_id = "wrong.row.descriptor";
        return result;
      };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 descriptor_mismatch,
                 "SB_OPERATOR_FUSION.ROW_DESCRIPTOR_MISMATCH");

  const auto missing_exact = [](const exec::OperatorFusionProviderRequest& request) {
    auto result = ProviderRows(request, {Row(0x42)});
    result.exact_recheck_evidence_present = false;
    return result;
  };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 missing_exact,
                 "SB_OPERATOR_FUSION.EXACT_RECHECK_EVIDENCE_REQUIRED");

  const auto missing_mga = [](const exec::OperatorFusionProviderRequest& request) {
    auto result = ProviderRows(request, {Row(0x43)});
    result.mga_recheck_evidence_present = false;
    return result;
  };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 missing_mga,
                 "SB_OPERATOR_FUSION.MGA_RECHECK_EVIDENCE_REQUIRED");

  const auto missing_security =
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x44)});
        result.security_recheck_evidence_present = false;
        return result;
      };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 missing_security,
                 "SB_OPERATOR_FUSION.SECURITY_RECHECK_EVIDENCE_REQUIRED");

  const auto missing_authority =
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x45)});
        result.provider_authority_evidence_present = false;
        return result;
      };
  RequireRefusal(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
      missing_authority,
      "SB_OPERATOR_FUSION.PROVIDER_AUTHORITY_EVIDENCE_REQUIRED");

  const auto provider_authority =
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x46)});
        result.provider_finality_or_visibility_authority = true;
        return result;
      };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 provider_authority, "SB_OPERATOR_FUSION.UNSAFE_AUTHORITY");

  const auto corrupt_row = [](const exec::OperatorFusionProviderRequest& request) {
    auto result = ProviderRows(request, {idx::CandidateSetRow{}});
    return result;
  };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 corrupt_row, "SB_CANDIDATE_SET.EXACT_ROW_UUID_REQUIRED");

  const auto redaction_violation =
      [](const exec::OperatorFusionProviderRequest& request) {
        auto result = ProviderRows(request, {Row(0x47)});
        result.redaction_or_security_violation = true;
        return result;
      };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 redaction_violation,
                 "SB_OPERATOR_FUSION.SECURITY_BARRIER_REQUIRED");

  const auto impossible_candidate_count =
      [](const exec::OperatorFusionProviderRequest& request) {
        std::vector<idx::CandidateSetRow> rows;
        for (platform::u64 i = 0; i <= request.plan.input_rows; ++i) {
          rows.push_back(Row(static_cast<platform::byte>(0x50 + (i % 64))));
        }
        return ProviderRows(request, std::move(rows));
      };
  RequireRefusal(Plan(exec::OperatorFusionPipelineKind::kScanFilterProject),
                 impossible_candidate_count,
                 "SB_OPERATOR_FUSION.COUNTERS_INVALID");

  auto unsupported_plan =
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject);
  unsupported_plan.exact_fallback_available = false;
  RequireRefusal(
      unsupported_plan,
      [](const exec::OperatorFusionProviderRequest&) {
        return exec::MakeUnsupportedOperatorFusionProviderResult(
            "operator_fusion.provider=unsupported_fixture");
      },
      "SB_OPERATOR_FUSION.PROVIDER_UNSUPPORTED");

  const auto unsupported_fallback = exec::ExecuteOperatorFusionPipeline(
      Plan(exec::OperatorFusionPipelineKind::kScanFilterProject), Authority(),
      {[](const exec::OperatorFusionProviderRequest&) {
         return exec::MakeUnsupportedOperatorFusionProviderResult(
             "operator_fusion.provider=unsupported_fixture");
       },
       [](const exec::OperatorFusionProviderRequest&) {
         return exec::MakeUnsupportedOperatorFusionProviderResult(
             "operator_fusion.fallback_provider=unsupported_fixture");
       },
       {},
       {}});
  Require(!unsupported_fallback.ok() && unsupported_fallback.fail_closed,
          "ODF-096 unsupported exact fallback provider was accepted");
  Require(unsupported_fallback.diagnostic_code ==
              "SB_OPERATOR_FUSION.PROVIDER_UNSUPPORTED",
          "ODF-096 unsupported fallback diagnostic changed");
  RequireEvidenceHygiene(unsupported_fallback.evidence);
}

}  // namespace

int main() {
  SupportedPipelinesAvoidMaterialization();
  RuntimeFilterFusionUsesOdf095AndIntersectsCandidates();
  ProviderFinalRowsAreDemotedOnlyWithRecheckEvidence();
  FailClosedForUnsafePlansAndProviders();
  return EXIT_SUCCESS;
}
