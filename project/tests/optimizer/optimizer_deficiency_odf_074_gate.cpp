// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/vector_api.hpp"

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

api::EngineRequestContext Context(api::EngineApiU64 tx = 74) {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_074_gate_api.sbdb";
  context.database_uuid.canonical = "019df074-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df074-0000-7000-8000-000000000074";
  context.local_transaction_id = tx;
  context.security_context_present = true;
  return context;
}

api::EngineVectorPhysicalProof VectorProof() {
  api::EngineVectorPhysicalProof proof;
  proof.proof_supplied = true;
  proof.exact_vector_proof = true;
  proof.hnsw_proof = true;
  proof.ivf_proof = true;
  proof.pq_proof = true;
  proof.diskann_like_proof = true;
  proof.generation_visibility_proof = true;
  proof.filtered_planner_proof = true;
  proof.pre_filter_proof = true;
  proof.post_filter_proof = true;
  proof.iterative_filter_proof = true;
  proof.hybrid_dense_sparse_proof = true;
  proof.exact_rerank_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kVector;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = "odf074.local.vector.provider";
  proof.provider_contract.fallback_provider_id = "odf074.local.exact_vector";
  proof.provider_contract.local_provider_available = true;
  proof.provider_contract.exact_fallback_available = true;
  proof.provider_contract.descriptor_visibility.proof_present = true;
  proof.provider_contract.descriptor_visibility.visible_to_snapshot = true;
  proof.provider_contract.descriptor_visibility.descriptor_shape_compatible = true;
  proof.provider_contract.security_redaction.proof_present = true;
  proof.provider_contract.security_redaction.redaction_policy_bound = true;
  proof.provider_contract.security_redaction.security_snapshot_bound = true;
  proof.provider_contract.index_generation.proof_present = true;
  proof.provider_contract.index_generation.visible_to_snapshot = true;
  proof.provider_contract.index_generation.covers_predicate = true;
  proof.provider_contract.index_generation.required_generation = 74;
  proof.provider_contract.index_generation.available_generation = 74;
  proof.provider_contract.index_generation.index_uuid = "odf074-vector-index";
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

std::vector<api::EngineVectorCorpusRow> Corpus() {
  return {
      {"row-alpha",
       {1.0, 0.0},
       {{"alpha", 1.0}},
       {{"tenant", "blue"}, {"kind", "primary"}}},
      {"row-beta",
       {0.9, 0.1},
       {{"beta", 1.0}},
       {{"tenant", "red"}, {"kind", "secondary"}}},
      {"row-gamma",
       {0.0, 1.0},
       {{"alpha", 0.2}, {"boost", 1.0}},
       {{"tenant", "blue"}, {"kind", "secondary"}}},
      {"row-delta",
       {0.4, 0.6},
       {{"boost", 3.0}},
       {{"tenant", "green"}, {"kind", "primary"}}},
  };
}

api::EngineVectorSearchRequest BaseRequest() {
  api::EngineVectorSearchRequest request;
  request.context = Context();
  request.query_vector = {1.0, 0.0};
  request.top_k = 3;
  request.vector_corpus_rows = Corpus();
  request.physical_proof = VectorProof();
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
              "ODF-074 evidence leaked forbidden authority or fallback token");
    }
  }
}

void ExactVectorRankingAndMGAEvidence() {
  auto request = BaseRequest();
  request.requested_access_tier = api::EngineVectorAccessTier::kExact;
  const auto result = api::EngineVectorSearch(request);
  Require(result.ok, "ODF-074 exact vector search failed");
  Require(result.result_shape.rows.size() == 3,
          "ODF-074 exact vector search returned the wrong row count");
  Require(RowField(result, 0, "row_uuid") == "row-alpha",
          "ODF-074 exact vector ranking returned the wrong winner");
  Require(RowField(result, 1, "row_uuid") == "row-beta",
          "ODF-074 exact vector ranking did not sort by exact distance");
  Require(EvidenceContains(result, "vector_physical_access",
                           "selected_tier=exact"),
          "ODF-074 exact tier evidence missing");
  Require(EvidenceContains(result, "vector_generation_visibility",
                           "engine_owned_mga_publish_barrier"),
          "ODF-074 generation visibility evidence missing");
  Require(EvidenceContains(result, "vector_exact_rerank",
                           "exact_dense_tiebreak"),
          "ODF-074 exact rerank evidence missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-074 MGA recheck evidence missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-074 security recheck evidence missing");
  Require(RowField(result, 0, "row_mga_recheck_required") == "true",
          "ODF-074 rows did not carry MGA recheck requirements");
  Require(RowField(result, 0, "row_security_recheck_required") == "true",
          "ODF-074 rows did not carry security recheck requirements");
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-074 physical vector search reported descriptor row scans");
  RequireEvidenceHygiene(result);
}

void TierSelectionEvidence() {
  const struct {
    api::EngineVectorAccessTier tier;
    const char* evidence;
  } cases[] = {{api::EngineVectorAccessTier::kHnsw, "selected_tier=hnsw"},
               {api::EngineVectorAccessTier::kIvf, "selected_tier=ivf"},
               {api::EngineVectorAccessTier::kPq, "selected_tier=pq"},
               {api::EngineVectorAccessTier::kDiskAnnLike,
                "selected_tier=diskann_like"}};
  for (const auto& item : cases) {
    auto request = BaseRequest();
    request.requested_access_tier = item.tier;
    const auto result = api::EngineVectorSearch(request);
    Require(result.ok, "ODF-074 vector tier selection failed");
    Require(EvidenceContains(result, "vector_physical_access", item.evidence),
            "ODF-074 selected tier evidence missing");
    Require(RowField(result, 0, "row_uuid") == "row-alpha",
            "ODF-074 selected tier changed deterministic exact rerank winner");
    RequireEvidenceHygiene(result);
  }

  auto request = BaseRequest();
  request.option_envelopes.push_back("vector.access_tier=hnsw");
  const auto result = api::EngineVectorSearch(request);
  Require(result.ok, "ODF-074 option-selected HNSW tier failed");
  Require(EvidenceContains(result, "vector_physical_access",
                           "selected_tier=hnsw"),
          "ODF-074 option-selected tier evidence missing");
}

void FilterStrategies() {
  const struct {
    api::EngineVectorFilteredStrategy strategy;
    const char* evidence_kind;
    const char* evidence_id;
  } cases[] = {{api::EngineVectorFilteredStrategy::kPreFilter,
                "vector_pre_filter",
                "applied=true"},
               {api::EngineVectorFilteredStrategy::kPostFilter,
                "vector_post_filter",
                "applied=true"},
               {api::EngineVectorFilteredStrategy::kIterativeFilter,
                "vector_iterative_filter",
                "applied=true"}};
  for (const auto& item : cases) {
    auto request = BaseRequest();
    request.top_k = 2;
    request.filtered_strategy = item.strategy;
    request.metadata_filters.push_back({"tenant", "blue"});
    const auto result = api::EngineVectorSearch(request);
    Require(result.ok, "ODF-074 filtered vector search failed");
    Require(result.result_shape.rows.size() == 2,
            "ODF-074 filtered vector search returned wrong row count");
    Require(RowField(result, 0, "row_uuid") == "row-alpha",
            "ODF-074 filtered vector search returned wrong winner");
    Require(RowField(result, 1, "row_uuid") == "row-gamma",
            "ODF-074 filtered vector search returned an unfiltered row");
    Require(EvidenceContains(result, item.evidence_kind, item.evidence_id),
            "ODF-074 filtered strategy evidence missing");
    RequireEvidenceHygiene(result);
  }
}

void HybridDenseSparseScoring() {
  auto request = BaseRequest();
  request.top_k = 1;
  request.sparse_terms.push_back({"boost", 2.0});
  const auto result = api::EngineVectorSearch(request);
  Require(result.ok, "ODF-074 hybrid vector search failed");
  Require(RowField(result, 0, "row_uuid") == "row-delta",
          "ODF-074 hybrid dense+sparse score did not affect ranking");
  Require(EvidenceContains(result, "vector_hybrid_dense_sparse",
                           "dense_plus_sparse_score"),
          "ODF-074 hybrid score evidence missing");
  Require(std::stod(RowField(result, 0, "sparse_score")) > 0.0,
          "ODF-074 hybrid winner did not report sparse contribution");
  RequireEvidenceHygiene(result);
}

void MissingProofsFailClosedWithExactDiagnostics() {
  auto request = BaseRequest();
  request.physical_proof = {};
  auto result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 missing physical proof did not fail closed");
  Require(DiagnosticContains(result, api::kVectorPhysicalProofMissing),
          "ODF-074 missing physical proof diagnostic changed");

  const struct {
    const char* diagnostic;
    void (*mutate)(api::EngineVectorPhysicalProof*);
  } cases[] = {
      {api::kVectorExactProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->exact_vector_proof = false;
       }},
      {api::kVectorHnswProofMissing,
       [](api::EngineVectorPhysicalProof* proof) { proof->hnsw_proof = false; }},
      {api::kVectorIvfProofMissing,
       [](api::EngineVectorPhysicalProof* proof) { proof->ivf_proof = false; }},
      {api::kVectorPqProofMissing,
       [](api::EngineVectorPhysicalProof* proof) { proof->pq_proof = false; }},
      {api::kVectorDiskAnnProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->diskann_like_proof = false;
       }},
      {api::kVectorGenerationVisibilityProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->generation_visibility_proof = false;
       }},
      {api::kVectorFilteredPlannerProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->filtered_planner_proof = false;
       }},
      {api::kVectorPreFilterProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->pre_filter_proof = false;
       }},
      {api::kVectorPostFilterProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->post_filter_proof = false;
       }},
      {api::kVectorIterativeFilterProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->iterative_filter_proof = false;
       }},
      {api::kVectorHybridProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->hybrid_dense_sparse_proof = false;
       }},
      {api::kVectorExactRerankProofMissing,
       [](api::EngineVectorPhysicalProof* proof) {
         proof->exact_rerank_proof = false;
       }},
  };
  for (const auto& item : cases) {
    request = BaseRequest();
    item.mutate(&request.physical_proof);
    result = api::EngineVectorSearch(request);
    Require(!result.ok, "ODF-074 missing proof flag did not fail closed");
    Require(DiagnosticContains(result, item.diagnostic),
            "ODF-074 missing proof flag diagnostic changed");
  }
}

void ProviderContractRefusalsFailClosed() {
  auto request = BaseRequest();
  request.physical_proof.provider_contract.descriptor_visibility
      .descriptor_scan_selected = true;
  auto result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 descriptor scan was accepted as vector access");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderDescriptorScanNotPhysicalProvider),
          "ODF-074 descriptor scan refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.descriptor_visibility
      .behavior_store_scan_selected = true;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 behavior-store scan was accepted as vector access");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderBehaviorScanNotPhysicalProvider),
          "ODF-074 behavior-store scan refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.security_redaction.proof_present = false;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 missing security proof did not fail closed");
  Require(DiagnosticContains(result, api::kNoSqlProviderSecurityProofMissing),
          "ODF-074 missing security proof diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck.proof_present = false;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 missing MGA proof did not fail closed");
  Require(DiagnosticContains(result, api::kNoSqlProviderMgaRecheckProofMissing),
          "ODF-074 missing MGA proof diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .parser_claims_transaction_finality_authority = true;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 parser finality authority was accepted");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderParserFinalityAuthorityRefused),
          "ODF-074 parser authority refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .provider_claims_transaction_finality_authority = true;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 provider finality authority was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderFinalityAuthorityRefused),
          "ODF-074 provider authority refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .provider_claims_visibility_authority = true;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 provider visibility authority was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderVisibilityAuthorityRefused),
          "ODF-074 provider visibility authority diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .write_ahead_log_claims_transaction_finality_authority = true;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 write-ahead finality authority was accepted");
  Require(DiagnosticContains(
              result,
              api::kNoSqlProviderWriteAheadFinalityAuthorityRefused),
          "ODF-074 write-ahead authority refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.family =
      api::EngineNoSqlProviderFamily::kSearch;
  result = api::EngineVectorSearch(request);
  Require(!result.ok, "ODF-074 non-vector provider family was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderFamilyUnsupported),
          "ODF-074 provider family refusal diagnostic changed");
}

void LegacyFallbackStillWorksForOldBasicRequest() {
  api::EngineVectorSearchRequest request;
  request.context = Context();
  request.target_object.uuid.canonical = "legacy-vector-collection";
  request.target_object.object_kind = "vector_collection";
  const auto result = api::EngineVectorSearch(request);
  Require(result.ok, "ODF-074 legacy vector fallback failed");
  Require(EvidenceContains(result, "vector_search", "exact_fallback_available"),
          "ODF-074 legacy vector fallback evidence changed");
  Require(EvidenceContains(result, "nosql_behavior",
                           "exact_scan_until_vector_index_available"),
          "ODF-074 legacy vector behavior fallback evidence missing");
}

}  // namespace

int main() {
  ExactVectorRankingAndMGAEvidence();
  TierSelectionEvidence();
  FilterStrategies();
  HybridDenseSparseScoring();
  MissingProofsFailClosedWithExactDiagnostics();
  ProviderContractRefusalsFailClosed();
  LegacyFallbackStillWorksForOldBasicRequest();
  return EXIT_SUCCESS;
}
