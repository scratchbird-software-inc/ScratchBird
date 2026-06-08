// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/search_api.hpp"

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

api::EngineRequestContext Context(api::EngineApiU64 tx = 73) {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_073_gate_api.sbdb";
  context.database_uuid.canonical = "019df073-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df073-0000-7000-8000-000000000073";
  context.local_transaction_id = tx;
  context.security_context_present = true;
  return context;
}

api::EngineSearchPhysicalProof SearchProof() {
  api::EngineSearchPhysicalProof proof;
  proof.proof_supplied = true;
  proof.mutable_buffer_proof = true;
  proof.sealed_inverted_segment_proof = true;
  proof.bm25_statistics_proof = true;
  proof.sparse_vector_score_proof = true;
  proof.maxscore_wand_topk_proof = true;
  proof.bloom_negative_pruning_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kSearch;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = "odf073.local.search.provider";
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
  proof.provider_contract.index_generation.required_generation = 73;
  proof.provider_contract.index_generation.available_generation = 73;
  proof.provider_contract.index_generation.index_uuid = "odf073-search-index";
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

std::vector<api::EngineSearchDocumentInput> Corpus() {
  return {
      {"doc-alpha-strong", "alpha alpha alpha beta search search", true},
      {"doc-alpha-mutable", "alpha mutable buffer entry", false},
      {"doc-beta-sealed", "beta sealed segment entry", true},
      {"doc-gamma", "gamma delta epsilon", true},
  };
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
          "parser_transaction_finality_authority=true",
          "client_autocommit_authority=true"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "ODF-073 evidence leaked forbidden authority or fallback token");
    }
  }
}

void RankedBm25MutableSealedAndBloomEvidence() {
  api::EngineSearchQueryRequest request;
  request.context = Context();
  request.query_text = "alpha beta absentterm";
  request.top_k = 3;
  request.document_corpus = Corpus();
  request.physical_proof = SearchProof();
  const auto result = api::EngineSearchQuery(request);
  Require(result.ok, "ODF-073 physical search query failed");
  Require(result.result_shape.rows.size() == 3,
          "ODF-073 BM25 search returned the wrong row count");
  Require(RowField(result, 0, "document_uuid") == "doc-alpha-strong",
          "ODF-073 BM25 ranking did not put the best document first");
  Require(std::stod(RowField(result, 0, "score")) >
              std::stod(RowField(result, 1, "score")),
          "ODF-073 BM25 scores were not ranked descending");
  Require(RowField(result, 1, "segment_kind") == "mutable_buffer" ||
              RowField(result, 2, "segment_kind") == "mutable_buffer",
          "ODF-073 mutable-buffer segment was not searched");
  Require(EvidenceContains(result, "search_physical_access",
                           "mutable_buffer_and_sealed_segment_bm25"),
          "ODF-073 search did not use the physical BM25 provider");
  Require(EvidenceContains(result, "search_bm25_statistics",
                           "document_frequency_idf_avgdl"),
          "ODF-073 BM25 statistics evidence was missing");
  Require(EvidenceContains(result, "search_sparse_vector_score",
                           "per_term_contributions_recorded"),
          "ODF-073 sparse-vector contribution evidence was missing");
  Require(EvidenceContains(result, "search_mutable_buffer_documents", "1"),
          "ODF-073 mutable-buffer evidence was missing");
  Require(EvidenceContains(result, "search_sealed_segment_documents", "3"),
          "ODF-073 sealed-segment evidence was missing");
  Require(EvidenceContains(result, "search_bloom_negative_pruning",
                           "absent_terms=1"),
          "ODF-073 Bloom negative-pruning evidence was missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-073 MGA recheck evidence was missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-073 security recheck evidence was missing");
  Require(RowField(result, 0, "row_mga_recheck_required") == "true",
          "ODF-073 rows did not carry MGA recheck requirements");
  Require(RowField(result, 0, "row_security_recheck_required") == "true",
          "ODF-073 rows did not carry security recheck requirements");
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-073 physical search reported descriptor/behavior row scans");
  RequireEvidenceHygiene(result);
}

void TopKWandPrunesCandidates() {
  api::EngineSearchQueryRequest request;
  request.context = Context();
  request.query_text = "alpha beta";
  request.top_k = 1;
  request.document_corpus = Corpus();
  request.physical_proof = SearchProof();
  const auto result = api::EngineSearchQuery(request);
  Require(result.ok, "ODF-073 top-K physical search failed");
  Require(result.result_shape.rows.size() == 1,
          "ODF-073 top-K search returned more than K rows");
  Require(RowField(result, 0, "document_uuid") == "doc-alpha-strong",
          "ODF-073 top-K search returned the wrong winner");
  Require(EvidenceContains(result, "search_wand_topk_pruning",
                           "candidates_pruned="),
          "ODF-073 WAND pruning evidence was missing");
  Require(!EvidenceContains(result, "search_wand_topk_pruning",
                            "candidates_pruned=0"),
          "ODF-073 WAND pruning did not prune any candidate");
  RequireEvidenceHygiene(result);
}

void MissingProofsFailClosedWithExactDiagnostics() {
  api::EngineSearchQueryRequest request;
  request.context = Context();
  request.query_text = "alpha";
  request.top_k = 2;
  request.document_corpus = Corpus();
  auto result = api::EngineSearchQuery(request);
  Require(!result.ok, "ODF-073 missing physical proof did not fail closed");
  Require(DiagnosticContains(result, api::kSearchPhysicalProofMissing),
          "ODF-073 missing physical proof diagnostic changed");

  const struct {
    const char* diagnostic;
    void (*mutate)(api::EngineSearchPhysicalProof*);
  } cases[] = {
      {api::kSearchMutableBufferProofMissing,
       [](api::EngineSearchPhysicalProof* proof) {
         proof->mutable_buffer_proof = false;
       }},
      {api::kSearchSealedInvertedSegmentProofMissing,
       [](api::EngineSearchPhysicalProof* proof) {
         proof->sealed_inverted_segment_proof = false;
       }},
      {api::kSearchBm25StatisticsProofMissing,
       [](api::EngineSearchPhysicalProof* proof) {
         proof->bm25_statistics_proof = false;
       }},
      {api::kSearchSparseVectorScoreProofMissing,
       [](api::EngineSearchPhysicalProof* proof) {
         proof->sparse_vector_score_proof = false;
       }},
      {api::kSearchMaxScoreWandTopKProofMissing,
       [](api::EngineSearchPhysicalProof* proof) {
         proof->maxscore_wand_topk_proof = false;
       }},
      {api::kSearchBloomNegativePruningProofMissing,
       [](api::EngineSearchPhysicalProof* proof) {
         proof->bloom_negative_pruning_proof = false;
       }},
  };
  for (const auto& item : cases) {
    request.physical_proof = SearchProof();
    item.mutate(&request.physical_proof);
    result = api::EngineSearchQuery(request);
    Require(!result.ok, "ODF-073 missing proof flag did not fail closed");
    Require(DiagnosticContains(result, item.diagnostic),
            "ODF-073 missing proof flag diagnostic changed");
  }
}

void ProviderContractRefusalsFailClosed() {
  api::EngineSearchQueryRequest request;
  request.context = Context();
  request.query_text = "alpha";
  request.top_k = 2;
  request.document_corpus = Corpus();
  request.physical_proof = SearchProof();
  request.physical_proof.provider_contract.descriptor_visibility
      .descriptor_scan_selected = true;
  auto result = api::EngineSearchQuery(request);
  Require(!result.ok, "ODF-073 descriptor scan was accepted as physical search");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderDescriptorScanNotPhysicalProvider),
          "ODF-073 descriptor scan refusal diagnostic changed");

  request.physical_proof = SearchProof();
  request.physical_proof.provider_contract.descriptor_visibility
      .behavior_store_scan_selected = true;
  result = api::EngineSearchQuery(request);
  Require(!result.ok,
          "ODF-073 behavior-store scan was accepted as physical search");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderBehaviorScanNotPhysicalProvider),
          "ODF-073 behavior-store scan refusal diagnostic changed");

  request.physical_proof = SearchProof();
  request.physical_proof.provider_contract.mga_recheck
      .parser_claims_transaction_finality_authority = true;
  result = api::EngineSearchQuery(request);
  Require(!result.ok,
          "ODF-073 parser finality authority was accepted by search");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderParserFinalityAuthorityRefused),
          "ODF-073 parser authority refusal diagnostic changed");

  request.physical_proof = SearchProof();
  request.physical_proof.provider_contract.mga_recheck
      .provider_claims_transaction_finality_authority = true;
  result = api::EngineSearchQuery(request);
  Require(!result.ok,
          "ODF-073 provider finality authority was accepted by search");
  Require(DiagnosticContains(result, api::kNoSqlProviderFinalityAuthorityRefused),
          "ODF-073 provider authority refusal diagnostic changed");

  request.physical_proof = SearchProof();
  request.physical_proof.provider_contract.mga_recheck
      .write_ahead_log_claims_transaction_finality_authority = true;
  result = api::EngineSearchQuery(request);
  Require(!result.ok,
          "ODF-073 write-ahead finality authority was accepted by search");
  Require(DiagnosticContains(
              result,
              api::kNoSqlProviderWriteAheadFinalityAuthorityRefused),
          "ODF-073 write-ahead authority refusal diagnostic changed");

  request.physical_proof = SearchProof();
  request.physical_proof.provider_contract.family =
      api::EngineNoSqlProviderFamily::kDocument;
  result = api::EngineSearchQuery(request);
  Require(!result.ok,
          "ODF-073 non-search provider family was accepted by search");
  Require(DiagnosticContains(result, api::kNoSqlProviderFamilyUnsupported),
          "ODF-073 provider family refusal diagnostic changed");
}

void LegacyFallbackStillWorksForOldBasicRequest() {
  api::EngineSearchQueryRequest request;
  request.context = Context();
  request.target_object.uuid.canonical = "legacy-search-collection";
  request.target_object.object_kind = "search_collection";
  const auto result = api::EngineSearchQuery(request);
  Require(result.ok, "ODF-073 legacy search fallback failed");
  Require(EvidenceContains(result, "search_query", "full_text_descriptor_query"),
          "ODF-073 legacy search fallback evidence changed");
  Require(EvidenceContains(result, "nosql_behavior",
                           "specialized_descriptor_fallback"),
          "ODF-073 legacy search behavior fallback evidence missing");
}

}  // namespace

int main() {
  RankedBm25MutableSealedAndBloomEvidence();
  TopKWandPrunesCandidates();
  MissingProofsFailClosedWithExactDiagnostics();
  ProviderContractRefusalsFailClosed();
  LegacyFallbackStillWorksForOldBasicRequest();
  return EXIT_SUCCESS;
}
