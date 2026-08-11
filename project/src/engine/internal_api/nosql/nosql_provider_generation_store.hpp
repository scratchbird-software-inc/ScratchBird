// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_diagnostics.hpp"
#include "api_types.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_PROVIDER_GENERATION_STORE
// Engine-side metadata for local NoSQL physical provider generations. This is
// evidence and route-admission state only; MGA visibility/finality remains owned
// by the engine transaction inventory.

struct EngineNoSqlProviderGenerationMetadata {
  EngineNoSqlProviderFamily family = EngineNoSqlProviderFamily::kUnknown;
  std::string provider_id;
  std::string database_identity;
  std::string database_uuid;
  std::string collection_uuid;
  std::string generation_uuid;
  std::uint64_t generation_id = 0;
  std::uint64_t descriptor_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::string publish_state = "published";
  std::string validation_state = "validated";
  std::string backup_metadata_ref;
  std::string restore_metadata_ref;
  std::string repair_metadata_ref;
  std::string support_bundle_evidence_id;
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
  // QOW-RCP-076-TIME-SERIES-ROLLUP-CARRIER-V1.  These fields are persisted
  // candidate-admission evidence only.  MGA visibility/finality remains
  // owned by the engine transaction inventory.
  bool time_series_rollup_candidate_present = false;
  std::string time_series_rollup_capability_uuid;
  std::uint64_t time_series_rollup_generation = 0;
  std::uint64_t time_series_visible_late_arrival_generation = 0;
  std::int64_t time_series_rollup_interval_ns = 0;
  std::string time_series_rollup_exactness_attestation_state;
  std::string time_series_rollup_statement_snapshot_uuid;
  std::string time_series_rollup_statement_metadata_snapshot_uuid;
  std::string time_series_rollup_owning_transaction_uuid;
  std::uint64_t time_series_rollup_local_transaction_id = 0;
  std::uint64_t
      time_series_rollup_snapshot_visible_through_local_transaction_id = 0;
  std::string time_series_rollup_security_context_uuid;
  std::string time_series_rollup_catalog_epoch_uuid;
  bool time_series_rollup_exact_residual_recheck_required = false;
  bool time_series_rollup_base_row_mga_recheck_required = false;
  bool time_series_rollup_security_recheck_required = false;
  // QOW-RCP-077-VECTOR-ANN-CARRIER-V1. These fields are persisted
  // candidate-admission evidence only. ANN never owns final rows, ordering,
  // visibility, transaction finality, or recovery authority.
  bool vector_ann_candidate_present = false;
  std::string vector_ann_capability_uuid;
  std::string vector_ann_index_uuid;
  std::string vector_ann_base_relation_uuid;
  std::uint64_t vector_ann_base_relation_generation = 0;
  std::string vector_ann_relation_descriptor_uuid;
  std::uint64_t vector_ann_relation_descriptor_generation = 0;
  std::string vector_ann_embedding_column_uuid;
  std::string vector_ann_embedding_descriptor_uuid;
  std::string vector_ann_embedding_type_uuid;
  std::uint64_t vector_ann_dimension = 0;
  std::string vector_ann_element_profile;
  std::string vector_ann_metric_id;
  std::string vector_ann_algorithm_id;
  std::string vector_ann_publish_attestation_state;
  bool vector_ann_checksum_valid = false;
  bool vector_ann_sealed_generation = false;
  bool vector_ann_recall_attestation_present = false;
  std::uint64_t vector_ann_recall_contract_top_k = 0;
  std::uint64_t vector_ann_recall_sample_rows = 0;
  std::uint64_t vector_ann_required_recall_ppm = 0;
  std::uint64_t vector_ann_observed_recall_ppm = 0;
  bool vector_ann_recall_sample_deterministic = false;
  std::string vector_ann_recall_evidence_uuid;
  std::string vector_ann_statement_uuid;
  std::string vector_ann_statement_snapshot_uuid;
  std::string vector_ann_statement_metadata_snapshot_uuid;
  std::string vector_ann_owning_transaction_uuid;
  std::uint64_t vector_ann_local_transaction_id = 0;
  std::uint64_t vector_ann_snapshot_visible_through_local_transaction_id = 0;
  std::string vector_ann_security_context_uuid;
  std::string vector_ann_catalog_epoch_uuid;
  bool vector_ann_exact_fallback_available = false;
  bool vector_ann_full_base_exact_recheck_required = false;
  bool vector_ann_base_row_mga_recheck_required = false;
  bool vector_ann_security_recheck_required = false;
  bool vector_ann_index_claims_visibility_authority = false;
  bool vector_ann_index_claims_transaction_finality_authority = false;
  bool vector_ann_parser_claims_visibility_authority = false;
  bool vector_ann_parser_claims_transaction_finality_authority = false;
  bool vector_ann_client_claims_visibility_authority = false;
  bool vector_ann_client_claims_transaction_finality_authority = false;
  bool vector_ann_reference_claims_visibility_authority = false;
  bool vector_ann_reference_claims_transaction_finality_authority = false;
  bool vector_ann_wal_claims_visibility_authority = false;
  bool vector_ann_wal_claims_transaction_finality_authority = false;
  // QOW-RCP-078-SEARCH-SEGMENT-CARRIER-V1. These fields are persisted
  // candidate-admission evidence only. A segment never owns final rows,
  // token identity, score/rank, visibility, transaction finality, or recovery.
  bool search_segment_candidate_present = false;
  std::string search_segment_capability_uuid;
  std::string search_segment_index_uuid;
  std::string search_segment_uuid;
  std::string search_segment_base_relation_uuid;
  std::uint64_t search_segment_base_relation_generation = 0;
  std::string search_segment_relation_descriptor_uuid;
  std::uint64_t search_segment_relation_descriptor_generation = 0;
  std::string search_segment_body_column_uuid;
  std::string search_segment_body_descriptor_uuid;
  std::string search_segment_body_type_uuid;
  std::string search_segment_category_column_uuid;
  std::string search_segment_category_descriptor_uuid;
  std::string search_segment_category_type_uuid;
  std::string search_segment_search_type_descriptor_uuid;
  std::uint64_t search_segment_search_type_descriptor_generation = 0;
  std::string search_segment_analyzer_uuid;
  std::uint64_t search_segment_analyzer_generation = 0;
  std::string search_segment_analyzer_pipeline_sha256;
  std::string search_segment_tokenizer_uuid;
  std::uint64_t search_segment_tokenizer_generation = 0;
  std::string search_segment_language_profile_uuid;
  std::uint64_t search_segment_language_profile_generation = 0;
  std::string search_segment_ranking_model_uuid;
  std::uint64_t search_segment_ranking_model_generation = 0;
  std::string search_segment_phrase_profile_uuid;
  std::uint64_t search_segment_phrase_profile_generation = 0;
  std::string search_segment_query_syntax_profile_uuid;
  std::uint64_t search_segment_query_syntax_profile_generation = 0;
  std::string search_segment_index_profile_id;
  std::uint64_t search_segment_generation = 0;
  bool search_segment_position_payload_present = false;
  bool search_segment_checksum_valid = false;
  bool search_segment_sealed_generation = false;
  std::string search_segment_publish_attestation_state;
  std::string search_segment_statement_uuid;
  std::string search_segment_statement_snapshot_uuid;
  std::string search_segment_statement_metadata_snapshot_uuid;
  std::string search_segment_owning_transaction_uuid;
  std::uint64_t search_segment_local_transaction_id = 0;
  std::uint64_t
      search_segment_snapshot_visible_through_local_transaction_id = 0;
  std::string search_segment_security_context_uuid;
  std::string search_segment_catalog_epoch_uuid;
  bool search_segment_exact_fallback_available = false;
  bool search_segment_full_corpus_exact_recheck_required = false;
  bool search_segment_residual_recheck_required = false;
  bool search_segment_base_row_mga_recheck_required = false;
  bool search_segment_security_recheck_required = false;
  bool search_segment_index_claims_visibility_authority = false;
  bool search_segment_index_claims_transaction_finality_authority = false;
  bool search_segment_parser_claims_visibility_authority = false;
  bool search_segment_parser_claims_transaction_finality_authority = false;
  bool search_segment_client_claims_visibility_authority = false;
  bool search_segment_client_claims_transaction_finality_authority = false;
  bool search_segment_reference_claims_visibility_authority = false;
  bool search_segment_reference_claims_transaction_finality_authority = false;
  bool search_segment_wal_claims_visibility_authority = false;
  bool search_segment_wal_claims_transaction_finality_authority = false;
};

struct EngineNoSqlProviderGenerationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineNoSqlProviderGenerationMetadata metadata;
  std::vector<std::string> evidence;
};

struct EngineNoSqlProviderGenerationRepairRequest {
  EngineNoSqlProviderFamily family = EngineNoSqlProviderFamily::kUnknown;
  std::string provider_id;
  std::string collection_uuid;
  bool repair_admitted = false;
  std::vector<EngineNoSqlProviderGenerationMetadata>
      authoritative_source_generations;
};

// QOW-RCP-076-TIME-SERIES-ROLLUP-CAPABILITY-BINDING-V1. The existing
// capability field is an integrity binding over the authoritative persisted
// carrier. It remains candidate-admission evidence only; MGA
// visibility/finality stays with the engine transaction inventory.
std::string DeriveTimeSeriesRollupCapabilityUuidV1(
    const EngineNoSqlProviderGenerationMetadata& metadata);

bool ValidateTimeSeriesRollupCapabilityBindingV1(
    const EngineNoSqlProviderGenerationMetadata& metadata);

// QOW-RCP-077-VECTOR-ANN-CAPABILITY-BINDING-V1. The capability UUID is an
// integrity binding over the complete active persisted carrier. It confers no
// candidate correctness, visibility, finality, security, or recovery authority.
std::string DeriveVectorAnnCapabilityUuidV1(
    const EngineNoSqlProviderGenerationMetadata& metadata);

bool ValidateVectorAnnCapabilityBindingV1(
    const EngineNoSqlProviderGenerationMetadata& metadata);

// QOW-RCP-078-SEARCH-SEGMENT-CAPABILITY-BINDING-V1. The capability UUID is
// an integrity binding over the complete active persisted carrier. It grants
// no candidate correctness, visibility, finality, security, or recovery
// authority.
std::string DeriveSearchSegmentCapabilityUuidV1(
    const EngineNoSqlProviderGenerationMetadata& metadata);

bool ValidateSearchSegmentCapabilityBindingV1(
    const EngineNoSqlProviderGenerationMetadata& metadata);

inline constexpr const char* kNoSqlProviderGenerationIdentityMismatch =
    "SB_NOSQL_PROVIDER_GENERATION.IDENTITY_MISMATCH";
inline constexpr const char* kNoSqlProviderGenerationRepairAdmissionRequired =
    "SB_NOSQL_PROVIDER_GENERATION.REPAIR_ADMISSION_REQUIRED";
inline constexpr const char* kNoSqlProviderGenerationRepairSourceMissing =
    "SB_NOSQL_PROVIDER_GENERATION.REPAIR_SOURCE_MISSING";

std::string EngineNoSqlProviderDatabaseIdentity(
    const EngineRequestContext& context);

EngineNoSqlProviderGenerationMetadata MakeDocumentProviderGenerationMetadata(
    const EngineRequestContext& context,
    const std::string& provider_id,
    const std::string& collection_uuid,
    std::uint64_t generation_id);

EngineNoSqlProviderGenerationResult PublishNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlProviderGenerationMetadata& metadata);

EngineNoSqlProviderGenerationResult LoadNoSqlProviderGeneration(
    const EngineRequestContext& context,
    EngineNoSqlProviderFamily family,
    const std::string& provider_id,
    const std::string& collection_uuid);

EngineNoSqlProviderGenerationResult ValidateNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlPhysicalProviderContract& contract);

EngineNoSqlProviderGenerationResult RepairNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlProviderGenerationRepairRequest& request);

EngineNoSqlProviderGenerationResult DropNoSqlProviderGeneration(
    const EngineRequestContext& context,
    EngineNoSqlProviderFamily family,
    const std::string& provider_id,
    const std::string& collection_uuid);

std::vector<EngineNoSqlProviderGenerationMetadata> ListNoSqlProviderGenerations(
    const EngineRequestContext& context);

EngineNoSqlProviderGenerationResult CleanupNoSqlProviderGenerations(
    const EngineRequestContext& context,
    bool drop_persistent_state);

void AddNoSqlProviderGenerationEvidence(
    EngineApiResult* result,
    const EngineNoSqlProviderGenerationResult& generation);

}  // namespace scratchbird::engine::internal_api
