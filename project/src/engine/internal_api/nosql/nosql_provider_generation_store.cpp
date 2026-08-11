// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_provider_generation_store.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kGenerationMagic = "SBNOSQLPG1";

std::mutex& StoreMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::vector<EngineNoSqlProviderGenerationMetadata>>&
GenerationCache() {
  static std::map<std::string, std::vector<EngineNoSqlProviderGenerationMetadata>>
      cache;
  return cache;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) {
    parts.push_back(current);
  }
  return parts;
}

std::uint64_t ParseU64(const std::string& value) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

std::int64_t ParseI64(const std::string& value) {
  try {
    return static_cast<std::int64_t>(std::stoll(value));
  } catch (...) {
    return 0;
  }
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE";
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

bool CanonicalPersistedBool(const std::string_view value) {
  return value == "true" || value == "false";
}

bool CanonicalPersistedU64(const std::string_view value) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
  std::uint64_t parsed = 0;
  const auto converted = std::from_chars(value.data(),
                                         value.data() + value.size(), parsed);
  return converted.ec == std::errc{} &&
         converted.ptr == value.data() + value.size() &&
         value == std::to_string(parsed);
}

bool RawVectorCarrierPairsValid(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  static constexpr std::array<std::string_view, 46> kVectorKeys = {
      "vector_ann_candidate_present",
      "vector_ann_capability_uuid",
      "vector_ann_index_uuid",
      "vector_ann_base_relation_uuid",
      "vector_ann_base_relation_generation",
      "vector_ann_relation_descriptor_uuid",
      "vector_ann_relation_descriptor_generation",
      "vector_ann_embedding_column_uuid",
      "vector_ann_embedding_descriptor_uuid",
      "vector_ann_embedding_type_uuid",
      "vector_ann_dimension",
      "vector_ann_element_profile",
      "vector_ann_metric_id",
      "vector_ann_algorithm_id",
      "vector_ann_publish_attestation_state",
      "vector_ann_checksum_valid",
      "vector_ann_sealed_generation",
      "vector_ann_recall_attestation_present",
      "vector_ann_recall_contract_top_k",
      "vector_ann_recall_sample_rows",
      "vector_ann_required_recall_ppm",
      "vector_ann_observed_recall_ppm",
      "vector_ann_recall_sample_deterministic",
      "vector_ann_recall_evidence_uuid",
      "vector_ann_statement_uuid",
      "vector_ann_statement_snapshot_uuid",
      "vector_ann_statement_metadata_snapshot_uuid",
      "vector_ann_owning_transaction_uuid",
      "vector_ann_local_transaction_id",
      "vector_ann_snapshot_visible_through_local_transaction_id",
      "vector_ann_security_context_uuid",
      "vector_ann_catalog_epoch_uuid",
      "vector_ann_exact_fallback_available",
      "vector_ann_full_base_exact_recheck_required",
      "vector_ann_base_row_mga_recheck_required",
      "vector_ann_security_recheck_required",
      "vector_ann_index_claims_visibility_authority",
      "vector_ann_index_claims_transaction_finality_authority",
      "vector_ann_parser_claims_visibility_authority",
      "vector_ann_parser_claims_transaction_finality_authority",
      "vector_ann_client_claims_visibility_authority",
      "vector_ann_client_claims_transaction_finality_authority",
      "vector_ann_reference_claims_visibility_authority",
      "vector_ann_reference_claims_transaction_finality_authority",
      "vector_ann_wal_claims_visibility_authority",
      "vector_ann_wal_claims_transaction_finality_authority",
  };
  static constexpr std::array<std::string_view, 15> kGenericSeedKeys = {
      "family", "provider_id", "database_identity", "database_uuid",
      "collection_uuid", "generation_uuid", "generation_id",
      "descriptor_epoch", "security_epoch", "redaction_epoch",
      "catalog_epoch", "publish_state", "validation_state",
      "provider_claims_transaction_finality_authority",
      "provider_claims_visibility_authority",
  };
  static constexpr std::array<std::string_view, 14> kBoolKeys = {
      "vector_ann_candidate_present", "vector_ann_checksum_valid",
      "vector_ann_sealed_generation", "vector_ann_recall_attestation_present",
      "vector_ann_recall_sample_deterministic",
      "vector_ann_exact_fallback_available",
      "vector_ann_full_base_exact_recheck_required",
      "vector_ann_base_row_mga_recheck_required",
      "vector_ann_security_recheck_required",
      "vector_ann_index_claims_visibility_authority",
      "vector_ann_index_claims_transaction_finality_authority",
      "vector_ann_parser_claims_visibility_authority",
      "vector_ann_parser_claims_transaction_finality_authority",
      "vector_ann_client_claims_visibility_authority",
  };
  static constexpr std::array<std::string_view, 6> kRemainingBoolKeys = {
      "vector_ann_client_claims_transaction_finality_authority",
      "vector_ann_reference_claims_visibility_authority",
      "vector_ann_reference_claims_transaction_finality_authority",
      "vector_ann_wal_claims_visibility_authority",
      "vector_ann_wal_claims_transaction_finality_authority",
      "provider_claims_transaction_finality_authority",
  };
  static constexpr std::array<std::string_view, 1> kGenericRemainingBoolKeys = {
      "provider_claims_visibility_authority",
  };
  static constexpr std::array<std::string_view, 13> kU64Keys = {
      "generation_id", "descriptor_epoch", "security_epoch",
      "redaction_epoch", "catalog_epoch",
      "vector_ann_base_relation_generation",
      "vector_ann_relation_descriptor_generation", "vector_ann_dimension",
      "vector_ann_recall_contract_top_k", "vector_ann_recall_sample_rows",
      "vector_ann_required_recall_ppm", "vector_ann_observed_recall_ppm",
      "vector_ann_local_transaction_id",
  };
  static constexpr std::array<std::string_view, 1> kRemainingU64Keys = {
      "vector_ann_snapshot_visible_through_local_transaction_id",
  };

  bool has_vector_key = false;
  std::map<std::string_view, std::size_t> counts;
  std::map<std::string_view, std::string_view> values;
  for (const auto& [key, value] : pairs) {
    const bool vector_key = key.starts_with("vector_ann_");
    has_vector_key = has_vector_key || vector_key;
    if (vector_key || std::ranges::find(kGenericSeedKeys, key) !=
                          kGenericSeedKeys.end()) {
      ++counts[key];
      values[key] = value;
    }
  }
  if (!has_vector_key) return true;
  for (const auto key : kVectorKeys) {
    if (counts[key] != 1) return false;
  }
  for (const auto key : kGenericSeedKeys) {
    if (counts[key] != 1) return false;
  }
  for (const auto key : kBoolKeys) {
    if (!CanonicalPersistedBool(values[key])) return false;
  }
  for (const auto key : kRemainingBoolKeys) {
    if (!CanonicalPersistedBool(values[key])) return false;
  }
  for (const auto key : kGenericRemainingBoolKeys) {
    if (!CanonicalPersistedBool(values[key])) return false;
  }
  for (const auto key : kU64Keys) {
    if (!CanonicalPersistedU64(values[key])) return false;
  }
  for (const auto key : kRemainingU64Keys) {
    if (!CanonicalPersistedU64(values[key])) return false;
  }
  // The decoder accepts historical family aliases, but an active RCP-077
  // carrier is signed over the canonical spelling.  Default vector fields are
  // persisted for every family and must remain valid there.
  if (values["vector_ann_candidate_present"] == "true" &&
      values["family"] != "vector") {
    return false;
  }
  return true;
}

bool RawSearchCarrierPairsValid(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  static constexpr std::array<std::string_view, 58> kSearchKeys = {
      "search_segment_candidate_present",
      "search_segment_capability_uuid",
      "search_segment_index_uuid",
      "search_segment_uuid",
      "search_segment_base_relation_uuid",
      "search_segment_base_relation_generation",
      "search_segment_relation_descriptor_uuid",
      "search_segment_relation_descriptor_generation",
      "search_segment_body_column_uuid",
      "search_segment_body_descriptor_uuid",
      "search_segment_body_type_uuid",
      "search_segment_category_column_uuid",
      "search_segment_category_descriptor_uuid",
      "search_segment_category_type_uuid",
      "search_segment_search_type_descriptor_uuid",
      "search_segment_search_type_descriptor_generation",
      "search_segment_analyzer_uuid",
      "search_segment_analyzer_generation",
      "search_segment_analyzer_pipeline_sha256",
      "search_segment_tokenizer_uuid",
      "search_segment_tokenizer_generation",
      "search_segment_language_profile_uuid",
      "search_segment_language_profile_generation",
      "search_segment_ranking_model_uuid",
      "search_segment_ranking_model_generation",
      "search_segment_phrase_profile_uuid",
      "search_segment_phrase_profile_generation",
      "search_segment_query_syntax_profile_uuid",
      "search_segment_query_syntax_profile_generation",
      "search_segment_index_profile_id",
      "search_segment_generation",
      "search_segment_position_payload_present",
      "search_segment_checksum_valid",
      "search_segment_sealed_generation",
      "search_segment_publish_attestation_state",
      "search_segment_statement_uuid",
      "search_segment_statement_snapshot_uuid",
      "search_segment_statement_metadata_snapshot_uuid",
      "search_segment_owning_transaction_uuid",
      "search_segment_local_transaction_id",
      "search_segment_snapshot_visible_through_local_transaction_id",
      "search_segment_security_context_uuid",
      "search_segment_catalog_epoch_uuid",
      "search_segment_exact_fallback_available",
      "search_segment_full_corpus_exact_recheck_required",
      "search_segment_residual_recheck_required",
      "search_segment_base_row_mga_recheck_required",
      "search_segment_security_recheck_required",
      "search_segment_index_claims_visibility_authority",
      "search_segment_index_claims_transaction_finality_authority",
      "search_segment_parser_claims_visibility_authority",
      "search_segment_parser_claims_transaction_finality_authority",
      "search_segment_client_claims_visibility_authority",
      "search_segment_client_claims_transaction_finality_authority",
      "search_segment_reference_claims_visibility_authority",
      "search_segment_reference_claims_transaction_finality_authority",
      "search_segment_wal_claims_visibility_authority",
      "search_segment_wal_claims_transaction_finality_authority",
  };
  static constexpr std::array<std::string_view, 15> kGenericSeedKeys = {
      "family", "provider_id", "database_identity", "database_uuid",
      "collection_uuid", "generation_uuid", "generation_id",
      "descriptor_epoch", "security_epoch", "redaction_epoch",
      "catalog_epoch", "publish_state", "validation_state",
      "provider_claims_transaction_finality_authority",
      "provider_claims_visibility_authority",
  };
  static constexpr std::array<std::string_view, 19> kBoolKeys = {
      "search_segment_candidate_present",
      "search_segment_position_payload_present",
      "search_segment_checksum_valid",
      "search_segment_sealed_generation",
      "search_segment_exact_fallback_available",
      "search_segment_full_corpus_exact_recheck_required",
      "search_segment_residual_recheck_required",
      "search_segment_base_row_mga_recheck_required",
      "search_segment_security_recheck_required",
      "search_segment_index_claims_visibility_authority",
      "search_segment_index_claims_transaction_finality_authority",
      "search_segment_parser_claims_visibility_authority",
      "search_segment_parser_claims_transaction_finality_authority",
      "search_segment_client_claims_visibility_authority",
      "search_segment_client_claims_transaction_finality_authority",
      "search_segment_reference_claims_visibility_authority",
      "search_segment_reference_claims_transaction_finality_authority",
      "search_segment_wal_claims_visibility_authority",
      "search_segment_wal_claims_transaction_finality_authority",
  };
  static constexpr std::array<std::string_view, 12> kU64Keys = {
      "search_segment_base_relation_generation",
      "search_segment_relation_descriptor_generation",
      "search_segment_search_type_descriptor_generation",
      "search_segment_analyzer_generation",
      "search_segment_tokenizer_generation",
      "search_segment_language_profile_generation",
      "search_segment_ranking_model_generation",
      "search_segment_phrase_profile_generation",
      "search_segment_query_syntax_profile_generation",
      "search_segment_generation",
      "search_segment_local_transaction_id",
      "search_segment_snapshot_visible_through_local_transaction_id",
  };
  static constexpr std::array<std::string_view, 5> kGenericU64Keys = {
      "generation_id", "descriptor_epoch", "security_epoch",
      "redaction_epoch", "catalog_epoch",
  };
  static constexpr std::array<std::string_view, 2> kGenericBoolKeys = {
      "provider_claims_transaction_finality_authority",
      "provider_claims_visibility_authority",
  };

  bool has_search_key = false;
  std::map<std::string_view, std::size_t> counts;
  std::map<std::string_view, std::string_view> values;
  for (const auto& [key, value] : pairs) {
    const bool search_key = key.starts_with("search_segment_");
    has_search_key = has_search_key || search_key;
    if (search_key || std::ranges::find(kGenericSeedKeys, key) !=
                          kGenericSeedKeys.end()) {
      ++counts[key];
      values[key] = value;
    }
  }
  if (!has_search_key) return true;
  for (const auto key : kSearchKeys) {
    if (counts[key] != 1) return false;
  }
  for (const auto key : kGenericSeedKeys) {
    if (counts[key] != 1) return false;
  }
  for (const auto key : kBoolKeys) {
    if (!CanonicalPersistedBool(values[key])) return false;
  }
  for (const auto key : kGenericBoolKeys) {
    if (!CanonicalPersistedBool(values[key])) return false;
  }
  for (const auto key : kU64Keys) {
    if (!CanonicalPersistedU64(values[key])) return false;
  }
  for (const auto key : kGenericU64Keys) {
    if (!CanonicalPersistedU64(values[key])) return false;
  }
  if (values["search_segment_candidate_present"] == "true" &&
      values["family"] != "search") {
    return false;
  }
  return true;
}

bool IsCanonicalLowercaseNonzeroUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  bool nonzero = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
    nonzero = nonzero || ch != '0';
  }
  return nonzero;
}

bool HasExactTimeSeriesRollupBindingInput(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return metadata.time_series_rollup_candidate_present &&
         metadata.family == EngineNoSqlProviderFamily::kTimeSeries &&
         IsCanonicalLowercaseNonzeroUuid(metadata.provider_id) &&
         !metadata.database_identity.empty() &&
         IsCanonicalLowercaseNonzeroUuid(metadata.database_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(metadata.collection_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(metadata.generation_uuid) &&
         metadata.generation_id != 0 && metadata.descriptor_epoch != 0 &&
         metadata.security_epoch != 0 && metadata.redaction_epoch != 0 &&
         metadata.catalog_epoch != 0 &&
         metadata.publish_state == "published" &&
         metadata.validation_state == "validated" &&
         !metadata.provider_claims_transaction_finality_authority &&
         !metadata.provider_claims_visibility_authority &&
         metadata.time_series_rollup_generation != 0 &&
         metadata.time_series_visible_late_arrival_generation != 0 &&
         metadata.time_series_rollup_generation <=
             metadata.time_series_visible_late_arrival_generation &&
         metadata.time_series_rollup_interval_ns > 0 &&
         metadata.time_series_rollup_exactness_attestation_state ==
             "TIME_SERIES_ROLLUP_SECTION_8_EXACT_V1" &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.time_series_rollup_statement_snapshot_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.time_series_rollup_statement_metadata_snapshot_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.time_series_rollup_owning_transaction_uuid) &&
         metadata.time_series_rollup_local_transaction_id != 0 &&
         metadata
                 .time_series_rollup_snapshot_visible_through_local_transaction_id !=
             0 &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.time_series_rollup_security_context_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.time_series_rollup_catalog_epoch_uuid) &&
         metadata.time_series_rollup_exact_residual_recheck_required &&
         metadata.time_series_rollup_base_row_mga_recheck_required &&
         metadata.time_series_rollup_security_recheck_required;
}

bool AppendLengthPrefixed(const std::string_view value, std::string* seed) {
  if (seed == nullptr) return false;
  const auto length = std::to_string(value.size());
  if (seed->size() > std::numeric_limits<std::size_t>::max() -
                         length.size() - 1 ||
      seed->size() + length.size() + 1 >
          std::numeric_limits<std::size_t>::max() - value.size()) {
    return false;
  }
  seed->append(length);
  seed->push_back(':');
  seed->append(value);
  return true;
}

std::string DeriveTimeSeriesRollupCapabilityUuidImpl(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!HasExactTimeSeriesRollupBindingInput(metadata)) return {};
  try {
    std::string seed;
    const auto append_field = [&](const std::string_view name,
                                  const std::string& value) {
      return AppendLengthPrefixed(name, &seed) &&
             AppendLengthPrefixed(value, &seed);
    };
    if (!AppendLengthPrefixed(
            "SCRATCHBIRD.TIME_SERIES_ROLLUP_CAPABILITY_BINDING.V1", &seed) ||
        !append_field("family", EngineNoSqlProviderFamilyName(metadata.family)) ||
        !append_field("provider_id", metadata.provider_id) ||
        !append_field("database_identity", metadata.database_identity) ||
        !append_field("database_uuid", metadata.database_uuid) ||
        !append_field("collection_uuid", metadata.collection_uuid) ||
        !append_field("generation_uuid", metadata.generation_uuid) ||
        !append_field("generation_id", std::to_string(metadata.generation_id)) ||
        !append_field("descriptor_epoch",
                      std::to_string(metadata.descriptor_epoch)) ||
        !append_field("security_epoch", std::to_string(metadata.security_epoch)) ||
        !append_field("redaction_epoch",
                      std::to_string(metadata.redaction_epoch)) ||
        !append_field("catalog_epoch", std::to_string(metadata.catalog_epoch)) ||
        !append_field("publish_state", metadata.publish_state) ||
        !append_field("validation_state", metadata.validation_state) ||
        !append_field("provider_claims_transaction_finality_authority",
                      BoolText(metadata
                                   .provider_claims_transaction_finality_authority)) ||
        !append_field("provider_claims_visibility_authority",
                      BoolText(metadata.provider_claims_visibility_authority)) ||
        !append_field("time_series_rollup_candidate_present",
                      BoolText(metadata.time_series_rollup_candidate_present)) ||
        !append_field("time_series_rollup_generation",
                      std::to_string(metadata.time_series_rollup_generation)) ||
        !append_field(
            "time_series_visible_late_arrival_generation",
            std::to_string(
                metadata.time_series_visible_late_arrival_generation)) ||
        !append_field("time_series_rollup_interval_ns",
                      std::to_string(metadata.time_series_rollup_interval_ns)) ||
        !append_field("time_series_rollup_exactness_attestation_state",
                      metadata.time_series_rollup_exactness_attestation_state) ||
        !append_field("time_series_rollup_statement_snapshot_uuid",
                      metadata.time_series_rollup_statement_snapshot_uuid) ||
        !append_field(
            "time_series_rollup_statement_metadata_snapshot_uuid",
            metadata.time_series_rollup_statement_metadata_snapshot_uuid) ||
        !append_field("time_series_rollup_owning_transaction_uuid",
                      metadata.time_series_rollup_owning_transaction_uuid) ||
        !append_field(
            "time_series_rollup_local_transaction_id",
            std::to_string(metadata.time_series_rollup_local_transaction_id)) ||
        !append_field(
            "time_series_rollup_snapshot_visible_through_local_transaction_id",
            std::to_string(metadata
                               .time_series_rollup_snapshot_visible_through_local_transaction_id)) ||
        !append_field("time_series_rollup_security_context_uuid",
                      metadata.time_series_rollup_security_context_uuid) ||
        !append_field("time_series_rollup_catalog_epoch_uuid",
                      metadata.time_series_rollup_catalog_epoch_uuid) ||
        !append_field(
            "time_series_rollup_exact_residual_recheck_required",
            BoolText(
                metadata.time_series_rollup_exact_residual_recheck_required)) ||
        !append_field(
            "time_series_rollup_base_row_mga_recheck_required",
            BoolText(
                metadata.time_series_rollup_base_row_mga_recheck_required)) ||
        !append_field("time_series_rollup_security_recheck_required",
                      BoolText(
                          metadata.time_series_rollup_security_recheck_required))) {
      return {};
    }
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const scratchbird::core::platform::byte*>(seed.data()),
        seed.size());
    if (!digest.ok() || digest.digest_bytes != 32) return {};
    std::array<std::uint8_t, 16> uuid_bytes{};
    std::copy_n(digest.digest.begin(), uuid_bytes.size(), uuid_bytes.begin());
    uuid_bytes[6] =
        static_cast<std::uint8_t>((uuid_bytes[6] & 0x0fU) | 0x80U);
    uuid_bytes[8] =
        static_cast<std::uint8_t>((uuid_bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < uuid_bytes.size(); ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
      out << std::setw(2) << static_cast<unsigned>(uuid_bytes[index]);
    }
    return out.str();
  } catch (...) {
    return {};
  }
}

bool HasExactVectorAnnBindingInput(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  const bool metric = metadata.vector_ann_metric_id == "L2_SQUARED" ||
                      metadata.vector_ann_metric_id == "COSINE" ||
                      metadata.vector_ann_metric_id == "INNER_PRODUCT";
  const bool algorithm = metadata.vector_ann_algorithm_id == "hnsw" ||
                         metadata.vector_ann_algorithm_id == "ivf" ||
                         metadata.vector_ann_algorithm_id == "pq" ||
                         metadata.vector_ann_algorithm_id == "diskann_like";
  return metadata.vector_ann_candidate_present &&
         metadata.family == EngineNoSqlProviderFamily::kVector &&
         IsCanonicalLowercaseNonzeroUuid(metadata.provider_id) &&
         !metadata.database_identity.empty() &&
         IsCanonicalLowercaseNonzeroUuid(metadata.database_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(metadata.collection_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(metadata.generation_uuid) &&
         metadata.generation_id != 0 && metadata.descriptor_epoch != 0 &&
         metadata.security_epoch != 0 && metadata.redaction_epoch != 0 &&
         metadata.catalog_epoch != 0 && metadata.publish_state == "published" &&
         metadata.validation_state == "validated" &&
         !metadata.provider_claims_transaction_finality_authority &&
         !metadata.provider_claims_visibility_authority &&
         IsCanonicalLowercaseNonzeroUuid(metadata.vector_ann_index_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_base_relation_uuid) &&
         metadata.vector_ann_base_relation_uuid == metadata.collection_uuid &&
         metadata.vector_ann_base_relation_generation != 0 &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_relation_descriptor_uuid) &&
         metadata.vector_ann_relation_descriptor_generation != 0 &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_embedding_column_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_embedding_descriptor_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_embedding_type_uuid) &&
         metadata.vector_ann_dimension == 3 &&
         metadata.vector_ann_element_profile == "real32" && metric &&
         algorithm &&
         metadata.vector_ann_publish_attestation_state ==
             "VECTOR_ANN_SECTION_8_FULL_BASE_EXACT_V1" &&
         metadata.vector_ann_checksum_valid &&
         metadata.vector_ann_sealed_generation &&
         metadata.vector_ann_recall_attestation_present &&
         metadata.vector_ann_recall_contract_top_k != 0 &&
         metadata.vector_ann_recall_sample_rows != 0 &&
         metadata.vector_ann_required_recall_ppm >= 1 &&
         metadata.vector_ann_required_recall_ppm <= 1'000'000 &&
         metadata.vector_ann_observed_recall_ppm >=
             metadata.vector_ann_required_recall_ppm &&
         metadata.vector_ann_observed_recall_ppm <= 1'000'000 &&
         metadata.vector_ann_recall_sample_deterministic &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_recall_evidence_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(metadata.vector_ann_statement_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_statement_snapshot_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_statement_metadata_snapshot_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_owning_transaction_uuid) &&
         metadata.vector_ann_local_transaction_id != 0 &&
         metadata.vector_ann_snapshot_visible_through_local_transaction_id !=
             0 &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_security_context_uuid) &&
         IsCanonicalLowercaseNonzeroUuid(
             metadata.vector_ann_catalog_epoch_uuid) &&
         metadata.vector_ann_exact_fallback_available &&
         metadata.vector_ann_full_base_exact_recheck_required &&
         metadata.vector_ann_base_row_mga_recheck_required &&
         metadata.vector_ann_security_recheck_required &&
         !metadata.vector_ann_index_claims_visibility_authority &&
         !metadata.vector_ann_index_claims_transaction_finality_authority &&
         !metadata.vector_ann_parser_claims_visibility_authority &&
         !metadata.vector_ann_parser_claims_transaction_finality_authority &&
         !metadata.vector_ann_client_claims_visibility_authority &&
         !metadata.vector_ann_client_claims_transaction_finality_authority &&
         !metadata.vector_ann_reference_claims_visibility_authority &&
         !metadata.vector_ann_reference_claims_transaction_finality_authority &&
         !metadata.vector_ann_wal_claims_visibility_authority &&
         !metadata.vector_ann_wal_claims_transaction_finality_authority;
}

std::string DeriveVectorAnnCapabilityUuidImpl(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!HasExactVectorAnnBindingInput(metadata)) return {};
  try {
    std::string seed;
    const auto append_field = [&](const std::string_view name,
                                  const std::string& value) {
      return AppendLengthPrefixed(name, &seed) &&
             AppendLengthPrefixed(value, &seed);
    };
    const auto append_u64 = [&](const std::string_view name,
                                const std::uint64_t value) {
      return append_field(name, std::to_string(value));
    };
    const auto append_bool = [&](const std::string_view name,
                                 const bool value) {
      return append_field(name, BoolText(value));
    };
    if (!AppendLengthPrefixed(
            "SCRATCHBIRD.VECTOR_ANN_CAPABILITY_BINDING.V1", &seed) ||
        !append_field("family", EngineNoSqlProviderFamilyName(metadata.family)) ||
        !append_field("provider_id", metadata.provider_id) ||
        !append_field("database_identity", metadata.database_identity) ||
        !append_field("database_uuid", metadata.database_uuid) ||
        !append_field("collection_uuid", metadata.collection_uuid) ||
        !append_field("generation_uuid", metadata.generation_uuid) ||
        !append_u64("generation_id", metadata.generation_id) ||
        !append_u64("descriptor_epoch", metadata.descriptor_epoch) ||
        !append_u64("security_epoch", metadata.security_epoch) ||
        !append_u64("redaction_epoch", metadata.redaction_epoch) ||
        !append_u64("catalog_epoch", metadata.catalog_epoch) ||
        !append_field("publish_state", metadata.publish_state) ||
        !append_field("validation_state", metadata.validation_state) ||
        !append_bool("provider_claims_transaction_finality_authority",
                     metadata.provider_claims_transaction_finality_authority) ||
        !append_bool("provider_claims_visibility_authority",
                     metadata.provider_claims_visibility_authority) ||
        !append_bool("vector_ann_candidate_present",
                     metadata.vector_ann_candidate_present) ||
        !append_field("vector_ann_index_uuid",
                      metadata.vector_ann_index_uuid) ||
        !append_field("vector_ann_base_relation_uuid",
                      metadata.vector_ann_base_relation_uuid) ||
        !append_u64("vector_ann_base_relation_generation",
                    metadata.vector_ann_base_relation_generation) ||
        !append_field("vector_ann_relation_descriptor_uuid",
                      metadata.vector_ann_relation_descriptor_uuid) ||
        !append_u64("vector_ann_relation_descriptor_generation",
                    metadata.vector_ann_relation_descriptor_generation) ||
        !append_field("vector_ann_embedding_column_uuid",
                      metadata.vector_ann_embedding_column_uuid) ||
        !append_field("vector_ann_embedding_descriptor_uuid",
                      metadata.vector_ann_embedding_descriptor_uuid) ||
        !append_field("vector_ann_embedding_type_uuid",
                      metadata.vector_ann_embedding_type_uuid) ||
        !append_u64("vector_ann_dimension", metadata.vector_ann_dimension) ||
        !append_field("vector_ann_element_profile",
                      metadata.vector_ann_element_profile) ||
        !append_field("vector_ann_metric_id", metadata.vector_ann_metric_id) ||
        !append_field("vector_ann_algorithm_id",
                      metadata.vector_ann_algorithm_id) ||
        !append_field("vector_ann_publish_attestation_state",
                      metadata.vector_ann_publish_attestation_state) ||
        !append_bool("vector_ann_checksum_valid",
                     metadata.vector_ann_checksum_valid) ||
        !append_bool("vector_ann_sealed_generation",
                     metadata.vector_ann_sealed_generation) ||
        !append_bool("vector_ann_recall_attestation_present",
                     metadata.vector_ann_recall_attestation_present) ||
        !append_u64("vector_ann_recall_contract_top_k",
                    metadata.vector_ann_recall_contract_top_k) ||
        !append_u64("vector_ann_recall_sample_rows",
                    metadata.vector_ann_recall_sample_rows) ||
        !append_u64("vector_ann_required_recall_ppm",
                    metadata.vector_ann_required_recall_ppm) ||
        !append_u64("vector_ann_observed_recall_ppm",
                    metadata.vector_ann_observed_recall_ppm) ||
        !append_bool("vector_ann_recall_sample_deterministic",
                     metadata.vector_ann_recall_sample_deterministic) ||
        !append_field("vector_ann_recall_evidence_uuid",
                      metadata.vector_ann_recall_evidence_uuid) ||
        !append_field("vector_ann_statement_uuid",
                      metadata.vector_ann_statement_uuid) ||
        !append_field("vector_ann_statement_snapshot_uuid",
                      metadata.vector_ann_statement_snapshot_uuid) ||
        !append_field("vector_ann_statement_metadata_snapshot_uuid",
                      metadata.vector_ann_statement_metadata_snapshot_uuid) ||
        !append_field("vector_ann_owning_transaction_uuid",
                      metadata.vector_ann_owning_transaction_uuid) ||
        !append_u64("vector_ann_local_transaction_id",
                    metadata.vector_ann_local_transaction_id) ||
        !append_u64("vector_ann_snapshot_visible_through_local_transaction_id",
                    metadata
                        .vector_ann_snapshot_visible_through_local_transaction_id) ||
        !append_field("vector_ann_security_context_uuid",
                      metadata.vector_ann_security_context_uuid) ||
        !append_field("vector_ann_catalog_epoch_uuid",
                      metadata.vector_ann_catalog_epoch_uuid) ||
        !append_bool("vector_ann_exact_fallback_available",
                     metadata.vector_ann_exact_fallback_available) ||
        !append_bool("vector_ann_full_base_exact_recheck_required",
                     metadata.vector_ann_full_base_exact_recheck_required) ||
        !append_bool("vector_ann_base_row_mga_recheck_required",
                     metadata.vector_ann_base_row_mga_recheck_required) ||
        !append_bool("vector_ann_security_recheck_required",
                     metadata.vector_ann_security_recheck_required) ||
        !append_bool("vector_ann_index_claims_visibility_authority",
                     metadata.vector_ann_index_claims_visibility_authority) ||
        !append_bool("vector_ann_index_claims_transaction_finality_authority",
                     metadata
                         .vector_ann_index_claims_transaction_finality_authority) ||
        !append_bool("vector_ann_parser_claims_visibility_authority",
                     metadata.vector_ann_parser_claims_visibility_authority) ||
        !append_bool("vector_ann_parser_claims_transaction_finality_authority",
                     metadata
                         .vector_ann_parser_claims_transaction_finality_authority) ||
        !append_bool("vector_ann_client_claims_visibility_authority",
                     metadata.vector_ann_client_claims_visibility_authority) ||
        !append_bool("vector_ann_client_claims_transaction_finality_authority",
                     metadata
                         .vector_ann_client_claims_transaction_finality_authority) ||
        !append_bool("vector_ann_reference_claims_visibility_authority",
                     metadata.vector_ann_reference_claims_visibility_authority) ||
        !append_bool("vector_ann_reference_claims_transaction_finality_authority",
                     metadata
                         .vector_ann_reference_claims_transaction_finality_authority) ||
        !append_bool("vector_ann_wal_claims_visibility_authority",
                     metadata.vector_ann_wal_claims_visibility_authority) ||
        !append_bool("vector_ann_wal_claims_transaction_finality_authority",
                     metadata.vector_ann_wal_claims_transaction_finality_authority)) {
      return {};
    }
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const scratchbird::core::platform::byte*>(seed.data()),
        seed.size());
    if (!digest.ok() || digest.digest_bytes != 32) return {};
    std::array<std::uint8_t, 16> uuid_bytes{};
    std::copy_n(digest.digest.begin(), uuid_bytes.size(), uuid_bytes.begin());
    uuid_bytes[6] =
        static_cast<std::uint8_t>((uuid_bytes[6] & 0x0fU) | 0x80U);
    uuid_bytes[8] =
        static_cast<std::uint8_t>((uuid_bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < uuid_bytes.size(); ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
      out << std::setw(2) << static_cast<unsigned>(uuid_bytes[index]);
    }
    return out.str();
  } catch (...) {
    return {};
  }
}

bool LowercaseHexDigest(const std::string_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](const char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool HasExactSearchSegmentBindingInput(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  const auto uuid = [](const std::string& value) {
    return IsCanonicalLowercaseNonzeroUuid(value);
  };
  return metadata.search_segment_candidate_present &&
         metadata.family == EngineNoSqlProviderFamily::kSearch &&
         uuid(metadata.provider_id) && !metadata.database_identity.empty() &&
         uuid(metadata.database_uuid) && uuid(metadata.collection_uuid) &&
         uuid(metadata.generation_uuid) && metadata.generation_id != 0 &&
         metadata.descriptor_epoch != 0 && metadata.security_epoch != 0 &&
         metadata.redaction_epoch != 0 && metadata.catalog_epoch != 0 &&
         metadata.publish_state == "published" &&
         metadata.validation_state == "validated" &&
         !metadata.provider_claims_transaction_finality_authority &&
         !metadata.provider_claims_visibility_authority &&
         uuid(metadata.search_segment_index_uuid) &&
         uuid(metadata.search_segment_uuid) &&
         uuid(metadata.search_segment_base_relation_uuid) &&
         metadata.search_segment_base_relation_uuid ==
             metadata.collection_uuid &&
         metadata.search_segment_base_relation_generation != 0 &&
         uuid(metadata.search_segment_relation_descriptor_uuid) &&
         metadata.search_segment_relation_descriptor_generation != 0 &&
         uuid(metadata.search_segment_body_column_uuid) &&
         uuid(metadata.search_segment_body_descriptor_uuid) &&
         uuid(metadata.search_segment_body_type_uuid) &&
         uuid(metadata.search_segment_category_column_uuid) &&
         uuid(metadata.search_segment_category_descriptor_uuid) &&
         uuid(metadata.search_segment_category_type_uuid) &&
         uuid(metadata.search_segment_search_type_descriptor_uuid) &&
         metadata.search_segment_search_type_descriptor_generation != 0 &&
         uuid(metadata.search_segment_analyzer_uuid) &&
         metadata.search_segment_analyzer_generation != 0 &&
         LowercaseHexDigest(metadata.search_segment_analyzer_pipeline_sha256) &&
         uuid(metadata.search_segment_tokenizer_uuid) &&
         metadata.search_segment_tokenizer_generation != 0 &&
         uuid(metadata.search_segment_language_profile_uuid) &&
         metadata.search_segment_language_profile_generation != 0 &&
         uuid(metadata.search_segment_ranking_model_uuid) &&
         metadata.search_segment_ranking_model_generation != 0 &&
         uuid(metadata.search_segment_phrase_profile_uuid) &&
         metadata.search_segment_phrase_profile_generation != 0 &&
         uuid(metadata.search_segment_query_syntax_profile_uuid) &&
         metadata.search_segment_query_syntax_profile_generation != 0 &&
         metadata.search_segment_index_profile_id ==
             "sb_full_text_positioned_v1" &&
         metadata.search_segment_generation != 0 &&
         metadata.search_segment_position_payload_present &&
         metadata.search_segment_checksum_valid &&
         metadata.search_segment_sealed_generation &&
         metadata.search_segment_publish_attestation_state ==
             "SEARCH_SEGMENT_SECTION_9_FULL_CORPUS_EXACT_V1" &&
         uuid(metadata.search_segment_statement_uuid) &&
         uuid(metadata.search_segment_statement_snapshot_uuid) &&
         uuid(metadata.search_segment_statement_metadata_snapshot_uuid) &&
         uuid(metadata.search_segment_owning_transaction_uuid) &&
         metadata.search_segment_local_transaction_id != 0 &&
         metadata
                 .search_segment_snapshot_visible_through_local_transaction_id !=
             0 &&
         uuid(metadata.search_segment_security_context_uuid) &&
         uuid(metadata.search_segment_catalog_epoch_uuid) &&
         metadata.search_segment_exact_fallback_available &&
         metadata.search_segment_full_corpus_exact_recheck_required &&
         metadata.search_segment_residual_recheck_required &&
         metadata.search_segment_base_row_mga_recheck_required &&
         metadata.search_segment_security_recheck_required &&
         !metadata.search_segment_index_claims_visibility_authority &&
         !metadata.search_segment_index_claims_transaction_finality_authority &&
         !metadata.search_segment_parser_claims_visibility_authority &&
         !metadata.search_segment_parser_claims_transaction_finality_authority &&
         !metadata.search_segment_client_claims_visibility_authority &&
         !metadata.search_segment_client_claims_transaction_finality_authority &&
         !metadata.search_segment_reference_claims_visibility_authority &&
         !metadata.search_segment_reference_claims_transaction_finality_authority &&
         !metadata.search_segment_wal_claims_visibility_authority &&
         !metadata.search_segment_wal_claims_transaction_finality_authority;
}

std::string DeriveSearchSegmentCapabilityUuidImpl(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!HasExactSearchSegmentBindingInput(metadata)) return {};
  try {
    std::string seed;
    const auto append_field = [&](const std::string_view name,
                                  const std::string& value) {
      return AppendLengthPrefixed(name, &seed) &&
             AppendLengthPrefixed(value, &seed);
    };
    const auto append_u64 = [&](const std::string_view name,
                                const std::uint64_t value) {
      return append_field(name, std::to_string(value));
    };
    const auto append_bool = [&](const std::string_view name,
                                 const bool value) {
      return append_field(name, BoolText(value));
    };
    if (!AppendLengthPrefixed(
            "SCRATCHBIRD.SEARCH_SEGMENT_CAPABILITY_BINDING.V1", &seed) ||
        !append_field("family", EngineNoSqlProviderFamilyName(metadata.family)) ||
        !append_field("provider_id", metadata.provider_id) ||
        !append_field("database_identity", metadata.database_identity) ||
        !append_field("database_uuid", metadata.database_uuid) ||
        !append_field("collection_uuid", metadata.collection_uuid) ||
        !append_field("generation_uuid", metadata.generation_uuid) ||
        !append_u64("generation_id", metadata.generation_id) ||
        !append_u64("descriptor_epoch", metadata.descriptor_epoch) ||
        !append_u64("security_epoch", metadata.security_epoch) ||
        !append_u64("redaction_epoch", metadata.redaction_epoch) ||
        !append_u64("catalog_epoch", metadata.catalog_epoch) ||
        !append_field("publish_state", metadata.publish_state) ||
        !append_field("validation_state", metadata.validation_state) ||
        !append_bool("provider_claims_transaction_finality_authority",
                     metadata.provider_claims_transaction_finality_authority) ||
        !append_bool("provider_claims_visibility_authority",
                     metadata.provider_claims_visibility_authority) ||
        !append_bool("search_segment_candidate_present",
                     metadata.search_segment_candidate_present) ||
        !append_field("search_segment_index_uuid",
                      metadata.search_segment_index_uuid) ||
        !append_field("search_segment_uuid", metadata.search_segment_uuid) ||
        !append_field("search_segment_base_relation_uuid",
                      metadata.search_segment_base_relation_uuid) ||
        !append_u64("search_segment_base_relation_generation",
                    metadata.search_segment_base_relation_generation) ||
        !append_field("search_segment_relation_descriptor_uuid",
                      metadata.search_segment_relation_descriptor_uuid) ||
        !append_u64("search_segment_relation_descriptor_generation",
                    metadata.search_segment_relation_descriptor_generation) ||
        !append_field("search_segment_body_column_uuid",
                      metadata.search_segment_body_column_uuid) ||
        !append_field("search_segment_body_descriptor_uuid",
                      metadata.search_segment_body_descriptor_uuid) ||
        !append_field("search_segment_body_type_uuid",
                      metadata.search_segment_body_type_uuid) ||
        !append_field("search_segment_category_column_uuid",
                      metadata.search_segment_category_column_uuid) ||
        !append_field("search_segment_category_descriptor_uuid",
                      metadata.search_segment_category_descriptor_uuid) ||
        !append_field("search_segment_category_type_uuid",
                      metadata.search_segment_category_type_uuid) ||
        !append_field("search_segment_search_type_descriptor_uuid",
                      metadata.search_segment_search_type_descriptor_uuid) ||
        !append_u64("search_segment_search_type_descriptor_generation",
                    metadata.search_segment_search_type_descriptor_generation) ||
        !append_field("search_segment_analyzer_uuid",
                      metadata.search_segment_analyzer_uuid) ||
        !append_u64("search_segment_analyzer_generation",
                    metadata.search_segment_analyzer_generation) ||
        !append_field("search_segment_analyzer_pipeline_sha256",
                      metadata.search_segment_analyzer_pipeline_sha256) ||
        !append_field("search_segment_tokenizer_uuid",
                      metadata.search_segment_tokenizer_uuid) ||
        !append_u64("search_segment_tokenizer_generation",
                    metadata.search_segment_tokenizer_generation) ||
        !append_field("search_segment_language_profile_uuid",
                      metadata.search_segment_language_profile_uuid) ||
        !append_u64("search_segment_language_profile_generation",
                    metadata.search_segment_language_profile_generation) ||
        !append_field("search_segment_ranking_model_uuid",
                      metadata.search_segment_ranking_model_uuid) ||
        !append_u64("search_segment_ranking_model_generation",
                    metadata.search_segment_ranking_model_generation) ||
        !append_field("search_segment_phrase_profile_uuid",
                      metadata.search_segment_phrase_profile_uuid) ||
        !append_u64("search_segment_phrase_profile_generation",
                    metadata.search_segment_phrase_profile_generation) ||
        !append_field("search_segment_query_syntax_profile_uuid",
                      metadata.search_segment_query_syntax_profile_uuid) ||
        !append_u64("search_segment_query_syntax_profile_generation",
                    metadata.search_segment_query_syntax_profile_generation) ||
        !append_field("search_segment_index_profile_id",
                      metadata.search_segment_index_profile_id) ||
        !append_u64("search_segment_generation",
                    metadata.search_segment_generation) ||
        !append_bool("search_segment_position_payload_present",
                     metadata.search_segment_position_payload_present) ||
        !append_bool("search_segment_checksum_valid",
                     metadata.search_segment_checksum_valid) ||
        !append_bool("search_segment_sealed_generation",
                     metadata.search_segment_sealed_generation) ||
        !append_field("search_segment_publish_attestation_state",
                      metadata.search_segment_publish_attestation_state) ||
        !append_field("search_segment_statement_uuid",
                      metadata.search_segment_statement_uuid) ||
        !append_field("search_segment_statement_snapshot_uuid",
                      metadata.search_segment_statement_snapshot_uuid) ||
        !append_field("search_segment_statement_metadata_snapshot_uuid",
                      metadata.search_segment_statement_metadata_snapshot_uuid) ||
        !append_field("search_segment_owning_transaction_uuid",
                      metadata.search_segment_owning_transaction_uuid) ||
        !append_u64("search_segment_local_transaction_id",
                    metadata.search_segment_local_transaction_id) ||
        !append_u64(
            "search_segment_snapshot_visible_through_local_transaction_id",
            metadata.search_segment_snapshot_visible_through_local_transaction_id) ||
        !append_field("search_segment_security_context_uuid",
                      metadata.search_segment_security_context_uuid) ||
        !append_field("search_segment_catalog_epoch_uuid",
                      metadata.search_segment_catalog_epoch_uuid) ||
        !append_bool("search_segment_exact_fallback_available",
                     metadata.search_segment_exact_fallback_available) ||
        !append_bool("search_segment_full_corpus_exact_recheck_required",
                     metadata.search_segment_full_corpus_exact_recheck_required) ||
        !append_bool("search_segment_residual_recheck_required",
                     metadata.search_segment_residual_recheck_required) ||
        !append_bool("search_segment_base_row_mga_recheck_required",
                     metadata.search_segment_base_row_mga_recheck_required) ||
        !append_bool("search_segment_security_recheck_required",
                     metadata.search_segment_security_recheck_required) ||
        !append_bool("search_segment_index_claims_visibility_authority",
                     metadata.search_segment_index_claims_visibility_authority) ||
        !append_bool(
            "search_segment_index_claims_transaction_finality_authority",
            metadata.search_segment_index_claims_transaction_finality_authority) ||
        !append_bool("search_segment_parser_claims_visibility_authority",
                     metadata.search_segment_parser_claims_visibility_authority) ||
        !append_bool(
            "search_segment_parser_claims_transaction_finality_authority",
            metadata.search_segment_parser_claims_transaction_finality_authority) ||
        !append_bool("search_segment_client_claims_visibility_authority",
                     metadata.search_segment_client_claims_visibility_authority) ||
        !append_bool(
            "search_segment_client_claims_transaction_finality_authority",
            metadata.search_segment_client_claims_transaction_finality_authority) ||
        !append_bool("search_segment_reference_claims_visibility_authority",
                     metadata.search_segment_reference_claims_visibility_authority) ||
        !append_bool(
            "search_segment_reference_claims_transaction_finality_authority",
            metadata.search_segment_reference_claims_transaction_finality_authority) ||
        !append_bool("search_segment_wal_claims_visibility_authority",
                     metadata.search_segment_wal_claims_visibility_authority) ||
        !append_bool("search_segment_wal_claims_transaction_finality_authority",
                     metadata.search_segment_wal_claims_transaction_finality_authority)) {
      return {};
    }
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const scratchbird::core::platform::byte*>(seed.data()),
        seed.size());
    if (!digest.ok() || digest.digest_bytes != 32) return {};
    std::array<std::uint8_t, 16> uuid_bytes{};
    std::copy_n(digest.digest.begin(), uuid_bytes.size(), uuid_bytes.begin());
    uuid_bytes[6] =
        static_cast<std::uint8_t>((uuid_bytes[6] & 0x0fU) | 0x80U);
    uuid_bytes[8] =
        static_cast<std::uint8_t>((uuid_bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < uuid_bytes.size(); ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
      out << std::setw(2) << static_cast<unsigned>(uuid_bytes[index]);
    }
    return out.str();
  } catch (...) {
    return {};
  }
}

bool ConstantTimeTextEqual(const std::string_view left,
                           const std::string_view right) {
  std::size_t different = left.size() ^ right.size();
  const auto maximum = std::max(left.size(), right.size());
  for (std::size_t index = 0; index < maximum; ++index) {
    const unsigned char l = index < left.size()
                                ? static_cast<unsigned char>(left[index])
                                : 0;
    const unsigned char r = index < right.size()
                                ? static_cast<unsigned char>(right[index])
                                : 0;
    different |= static_cast<std::size_t>(l ^ r);
  }
  return different == 0;
}

std::string GenerationPath(const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return {};
  }
  return context.database_path + ".sb.nosql_provider_generations";
}

std::uint64_t Fnva64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

bool IsHex(char ch) {
  return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

bool IsValidUuid(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') { return false; }
    } else if (!IsHex(value[i])) {
      return false;
    }
  }
  return true;
}

std::string StableGenerationUuid(const EngineRequestContext& context,
                                 const std::string& provider_id,
                                 const std::string& collection_uuid,
                                 std::uint64_t generation_id) {
  const std::string database_seed =
      !context.database_uuid.canonical.empty()
          ? context.database_uuid.canonical
          : EngineNoSqlProviderDatabaseIdentity(context);
  const std::string seed = database_seed + "|" + provider_id + "|" +
                           collection_uuid + "|" +
                           std::to_string(generation_id);
  std::string hex = Hex64(Fnva64(seed + ":left")) +
                    Hex64(Fnva64(seed + ":right"));
  hex[12] = '7';
  hex[16] = '8';
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" +
         hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
         hex.substr(20, 12);
}

std::uint64_t NonZeroEpoch(std::uint64_t epoch) { return epoch == 0 ? 1 : epoch; }

std::vector<std::pair<std::string, std::string>> MetadataPairs(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return {
      {"family", EngineNoSqlProviderFamilyName(metadata.family)},
      {"provider_id", metadata.provider_id},
      {"database_identity", metadata.database_identity},
      {"database_uuid", metadata.database_uuid},
      {"collection_uuid", metadata.collection_uuid},
      {"generation_uuid", metadata.generation_uuid},
      {"generation_id", std::to_string(metadata.generation_id)},
      {"descriptor_epoch", std::to_string(metadata.descriptor_epoch)},
      {"security_epoch", std::to_string(metadata.security_epoch)},
      {"redaction_epoch", std::to_string(metadata.redaction_epoch)},
      {"catalog_epoch", std::to_string(metadata.catalog_epoch)},
      {"publish_state", metadata.publish_state},
      {"validation_state", metadata.validation_state},
      {"backup_metadata_ref", metadata.backup_metadata_ref},
      {"restore_metadata_ref", metadata.restore_metadata_ref},
      {"repair_metadata_ref", metadata.repair_metadata_ref},
      {"support_bundle_evidence_id", metadata.support_bundle_evidence_id},
      {"provider_claims_transaction_finality_authority",
       BoolText(metadata.provider_claims_transaction_finality_authority)},
      {"provider_claims_visibility_authority",
       BoolText(metadata.provider_claims_visibility_authority)},
      {"time_series_rollup_candidate_present",
       BoolText(metadata.time_series_rollup_candidate_present)},
      {"time_series_rollup_capability_uuid",
       metadata.time_series_rollup_capability_uuid},
      {"time_series_rollup_generation",
       std::to_string(metadata.time_series_rollup_generation)},
      {"time_series_visible_late_arrival_generation",
       std::to_string(metadata.time_series_visible_late_arrival_generation)},
      {"time_series_rollup_interval_ns",
       std::to_string(metadata.time_series_rollup_interval_ns)},
      {"time_series_rollup_exactness_attestation_state",
       metadata.time_series_rollup_exactness_attestation_state},
      {"time_series_rollup_statement_snapshot_uuid",
       metadata.time_series_rollup_statement_snapshot_uuid},
      {"time_series_rollup_statement_metadata_snapshot_uuid",
       metadata.time_series_rollup_statement_metadata_snapshot_uuid},
      {"time_series_rollup_owning_transaction_uuid",
       metadata.time_series_rollup_owning_transaction_uuid},
      {"time_series_rollup_local_transaction_id",
       std::to_string(metadata.time_series_rollup_local_transaction_id)},
      {"time_series_rollup_snapshot_visible_through_local_transaction_id",
       std::to_string(metadata
                          .time_series_rollup_snapshot_visible_through_local_transaction_id)},
      {"time_series_rollup_security_context_uuid",
       metadata.time_series_rollup_security_context_uuid},
      {"time_series_rollup_catalog_epoch_uuid",
       metadata.time_series_rollup_catalog_epoch_uuid},
      {"time_series_rollup_exact_residual_recheck_required",
       BoolText(metadata.time_series_rollup_exact_residual_recheck_required)},
      {"time_series_rollup_base_row_mga_recheck_required",
       BoolText(metadata.time_series_rollup_base_row_mga_recheck_required)},
      {"time_series_rollup_security_recheck_required",
       BoolText(metadata.time_series_rollup_security_recheck_required)},
      {"vector_ann_candidate_present",
       BoolText(metadata.vector_ann_candidate_present)},
      {"vector_ann_capability_uuid", metadata.vector_ann_capability_uuid},
      {"vector_ann_index_uuid", metadata.vector_ann_index_uuid},
      {"vector_ann_base_relation_uuid",
       metadata.vector_ann_base_relation_uuid},
      {"vector_ann_base_relation_generation",
       std::to_string(metadata.vector_ann_base_relation_generation)},
      {"vector_ann_relation_descriptor_uuid",
       metadata.vector_ann_relation_descriptor_uuid},
      {"vector_ann_relation_descriptor_generation",
       std::to_string(metadata.vector_ann_relation_descriptor_generation)},
      {"vector_ann_embedding_column_uuid",
       metadata.vector_ann_embedding_column_uuid},
      {"vector_ann_embedding_descriptor_uuid",
       metadata.vector_ann_embedding_descriptor_uuid},
      {"vector_ann_embedding_type_uuid",
       metadata.vector_ann_embedding_type_uuid},
      {"vector_ann_dimension", std::to_string(metadata.vector_ann_dimension)},
      {"vector_ann_element_profile", metadata.vector_ann_element_profile},
      {"vector_ann_metric_id", metadata.vector_ann_metric_id},
      {"vector_ann_algorithm_id", metadata.vector_ann_algorithm_id},
      {"vector_ann_publish_attestation_state",
       metadata.vector_ann_publish_attestation_state},
      {"vector_ann_checksum_valid",
       BoolText(metadata.vector_ann_checksum_valid)},
      {"vector_ann_sealed_generation",
       BoolText(metadata.vector_ann_sealed_generation)},
      {"vector_ann_recall_attestation_present",
       BoolText(metadata.vector_ann_recall_attestation_present)},
      {"vector_ann_recall_contract_top_k",
       std::to_string(metadata.vector_ann_recall_contract_top_k)},
      {"vector_ann_recall_sample_rows",
       std::to_string(metadata.vector_ann_recall_sample_rows)},
      {"vector_ann_required_recall_ppm",
       std::to_string(metadata.vector_ann_required_recall_ppm)},
      {"vector_ann_observed_recall_ppm",
       std::to_string(metadata.vector_ann_observed_recall_ppm)},
      {"vector_ann_recall_sample_deterministic",
       BoolText(metadata.vector_ann_recall_sample_deterministic)},
      {"vector_ann_recall_evidence_uuid",
       metadata.vector_ann_recall_evidence_uuid},
      {"vector_ann_statement_uuid", metadata.vector_ann_statement_uuid},
      {"vector_ann_statement_snapshot_uuid",
       metadata.vector_ann_statement_snapshot_uuid},
      {"vector_ann_statement_metadata_snapshot_uuid",
       metadata.vector_ann_statement_metadata_snapshot_uuid},
      {"vector_ann_owning_transaction_uuid",
       metadata.vector_ann_owning_transaction_uuid},
      {"vector_ann_local_transaction_id",
       std::to_string(metadata.vector_ann_local_transaction_id)},
      {"vector_ann_snapshot_visible_through_local_transaction_id",
       std::to_string(
           metadata.vector_ann_snapshot_visible_through_local_transaction_id)},
      {"vector_ann_security_context_uuid",
       metadata.vector_ann_security_context_uuid},
      {"vector_ann_catalog_epoch_uuid",
       metadata.vector_ann_catalog_epoch_uuid},
      {"vector_ann_exact_fallback_available",
       BoolText(metadata.vector_ann_exact_fallback_available)},
      {"vector_ann_full_base_exact_recheck_required",
       BoolText(metadata.vector_ann_full_base_exact_recheck_required)},
      {"vector_ann_base_row_mga_recheck_required",
       BoolText(metadata.vector_ann_base_row_mga_recheck_required)},
      {"vector_ann_security_recheck_required",
       BoolText(metadata.vector_ann_security_recheck_required)},
      {"vector_ann_index_claims_visibility_authority",
       BoolText(metadata.vector_ann_index_claims_visibility_authority)},
      {"vector_ann_index_claims_transaction_finality_authority",
       BoolText(
           metadata.vector_ann_index_claims_transaction_finality_authority)},
      {"vector_ann_parser_claims_visibility_authority",
       BoolText(metadata.vector_ann_parser_claims_visibility_authority)},
      {"vector_ann_parser_claims_transaction_finality_authority",
       BoolText(
           metadata.vector_ann_parser_claims_transaction_finality_authority)},
      {"vector_ann_client_claims_visibility_authority",
       BoolText(metadata.vector_ann_client_claims_visibility_authority)},
      {"vector_ann_client_claims_transaction_finality_authority",
       BoolText(
           metadata.vector_ann_client_claims_transaction_finality_authority)},
      {"vector_ann_reference_claims_visibility_authority",
       BoolText(metadata.vector_ann_reference_claims_visibility_authority)},
      {"vector_ann_reference_claims_transaction_finality_authority",
       BoolText(metadata
                    .vector_ann_reference_claims_transaction_finality_authority)},
      {"vector_ann_wal_claims_visibility_authority",
       BoolText(metadata.vector_ann_wal_claims_visibility_authority)},
      {"vector_ann_wal_claims_transaction_finality_authority",
       BoolText(metadata.vector_ann_wal_claims_transaction_finality_authority)},
      {"search_segment_candidate_present",
       BoolText(metadata.search_segment_candidate_present)},
      {"search_segment_capability_uuid",
       metadata.search_segment_capability_uuid},
      {"search_segment_index_uuid", metadata.search_segment_index_uuid},
      {"search_segment_uuid", metadata.search_segment_uuid},
      {"search_segment_base_relation_uuid",
       metadata.search_segment_base_relation_uuid},
      {"search_segment_base_relation_generation",
       std::to_string(metadata.search_segment_base_relation_generation)},
      {"search_segment_relation_descriptor_uuid",
       metadata.search_segment_relation_descriptor_uuid},
      {"search_segment_relation_descriptor_generation",
       std::to_string(metadata.search_segment_relation_descriptor_generation)},
      {"search_segment_body_column_uuid",
       metadata.search_segment_body_column_uuid},
      {"search_segment_body_descriptor_uuid",
       metadata.search_segment_body_descriptor_uuid},
      {"search_segment_body_type_uuid",
       metadata.search_segment_body_type_uuid},
      {"search_segment_category_column_uuid",
       metadata.search_segment_category_column_uuid},
      {"search_segment_category_descriptor_uuid",
       metadata.search_segment_category_descriptor_uuid},
      {"search_segment_category_type_uuid",
       metadata.search_segment_category_type_uuid},
      {"search_segment_search_type_descriptor_uuid",
       metadata.search_segment_search_type_descriptor_uuid},
      {"search_segment_search_type_descriptor_generation",
       std::to_string(
           metadata.search_segment_search_type_descriptor_generation)},
      {"search_segment_analyzer_uuid", metadata.search_segment_analyzer_uuid},
      {"search_segment_analyzer_generation",
       std::to_string(metadata.search_segment_analyzer_generation)},
      {"search_segment_analyzer_pipeline_sha256",
       metadata.search_segment_analyzer_pipeline_sha256},
      {"search_segment_tokenizer_uuid",
       metadata.search_segment_tokenizer_uuid},
      {"search_segment_tokenizer_generation",
       std::to_string(metadata.search_segment_tokenizer_generation)},
      {"search_segment_language_profile_uuid",
       metadata.search_segment_language_profile_uuid},
      {"search_segment_language_profile_generation",
       std::to_string(metadata.search_segment_language_profile_generation)},
      {"search_segment_ranking_model_uuid",
       metadata.search_segment_ranking_model_uuid},
      {"search_segment_ranking_model_generation",
       std::to_string(metadata.search_segment_ranking_model_generation)},
      {"search_segment_phrase_profile_uuid",
       metadata.search_segment_phrase_profile_uuid},
      {"search_segment_phrase_profile_generation",
       std::to_string(metadata.search_segment_phrase_profile_generation)},
      {"search_segment_query_syntax_profile_uuid",
       metadata.search_segment_query_syntax_profile_uuid},
      {"search_segment_query_syntax_profile_generation",
       std::to_string(metadata.search_segment_query_syntax_profile_generation)},
      {"search_segment_index_profile_id",
       metadata.search_segment_index_profile_id},
      {"search_segment_generation",
       std::to_string(metadata.search_segment_generation)},
      {"search_segment_position_payload_present",
       BoolText(metadata.search_segment_position_payload_present)},
      {"search_segment_checksum_valid",
       BoolText(metadata.search_segment_checksum_valid)},
      {"search_segment_sealed_generation",
       BoolText(metadata.search_segment_sealed_generation)},
      {"search_segment_publish_attestation_state",
       metadata.search_segment_publish_attestation_state},
      {"search_segment_statement_uuid",
       metadata.search_segment_statement_uuid},
      {"search_segment_statement_snapshot_uuid",
       metadata.search_segment_statement_snapshot_uuid},
      {"search_segment_statement_metadata_snapshot_uuid",
       metadata.search_segment_statement_metadata_snapshot_uuid},
      {"search_segment_owning_transaction_uuid",
       metadata.search_segment_owning_transaction_uuid},
      {"search_segment_local_transaction_id",
       std::to_string(metadata.search_segment_local_transaction_id)},
      {"search_segment_snapshot_visible_through_local_transaction_id",
       std::to_string(
           metadata.search_segment_snapshot_visible_through_local_transaction_id)},
      {"search_segment_security_context_uuid",
       metadata.search_segment_security_context_uuid},
      {"search_segment_catalog_epoch_uuid",
       metadata.search_segment_catalog_epoch_uuid},
      {"search_segment_exact_fallback_available",
       BoolText(metadata.search_segment_exact_fallback_available)},
      {"search_segment_full_corpus_exact_recheck_required",
       BoolText(metadata.search_segment_full_corpus_exact_recheck_required)},
      {"search_segment_residual_recheck_required",
       BoolText(metadata.search_segment_residual_recheck_required)},
      {"search_segment_base_row_mga_recheck_required",
       BoolText(metadata.search_segment_base_row_mga_recheck_required)},
      {"search_segment_security_recheck_required",
       BoolText(metadata.search_segment_security_recheck_required)},
      {"search_segment_index_claims_visibility_authority",
       BoolText(metadata.search_segment_index_claims_visibility_authority)},
      {"search_segment_index_claims_transaction_finality_authority",
       BoolText(
           metadata.search_segment_index_claims_transaction_finality_authority)},
      {"search_segment_parser_claims_visibility_authority",
       BoolText(metadata.search_segment_parser_claims_visibility_authority)},
      {"search_segment_parser_claims_transaction_finality_authority",
       BoolText(
           metadata.search_segment_parser_claims_transaction_finality_authority)},
      {"search_segment_client_claims_visibility_authority",
       BoolText(metadata.search_segment_client_claims_visibility_authority)},
      {"search_segment_client_claims_transaction_finality_authority",
       BoolText(
           metadata.search_segment_client_claims_transaction_finality_authority)},
      {"search_segment_reference_claims_visibility_authority",
       BoolText(metadata.search_segment_reference_claims_visibility_authority)},
      {"search_segment_reference_claims_transaction_finality_authority",
       BoolText(metadata
                    .search_segment_reference_claims_transaction_finality_authority)},
      {"search_segment_wal_claims_visibility_authority",
       BoolText(metadata.search_segment_wal_claims_visibility_authority)},
      {"search_segment_wal_claims_transaction_finality_authority",
       BoolText(metadata.search_segment_wal_claims_transaction_finality_authority)},
  };
}

std::map<std::string, std::string> PairMap(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::map<std::string, std::string> out;
  for (const auto& [key, value] : pairs) {
    out[key] = value;
  }
  return out;
}

std::string ValueOr(const std::map<std::string, std::string>& values,
                    const std::string& key,
                    const std::string& fallback = {}) {
  const auto it = values.find(key);
  return it == values.end() ? fallback : it->second;
}

EngineNoSqlProviderGenerationMetadata MetadataFromPairs(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  const auto values = PairMap(pairs);
  EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = EngineNoSqlProviderFamilyFromString(ValueOr(values, "family"));
  metadata.provider_id = ValueOr(values, "provider_id");
  metadata.database_identity = ValueOr(values, "database_identity");
  metadata.database_uuid = ValueOr(values, "database_uuid");
  metadata.collection_uuid = ValueOr(values, "collection_uuid");
  metadata.generation_uuid = ValueOr(values, "generation_uuid");
  metadata.generation_id = ParseU64(ValueOr(values, "generation_id"));
  metadata.descriptor_epoch = ParseU64(ValueOr(values, "descriptor_epoch"));
  metadata.security_epoch = ParseU64(ValueOr(values, "security_epoch"));
  metadata.redaction_epoch = ParseU64(ValueOr(values, "redaction_epoch"));
  metadata.catalog_epoch = ParseU64(ValueOr(values, "catalog_epoch"));
  metadata.publish_state = ValueOr(values, "publish_state", "unverified");
  metadata.validation_state = ValueOr(values, "validation_state", "unverified");
  metadata.backup_metadata_ref = ValueOr(values, "backup_metadata_ref");
  metadata.restore_metadata_ref = ValueOr(values, "restore_metadata_ref");
  metadata.repair_metadata_ref = ValueOr(values, "repair_metadata_ref");
  metadata.support_bundle_evidence_id =
      ValueOr(values, "support_bundle_evidence_id");
  metadata.provider_claims_transaction_finality_authority =
      ParseBool(ValueOr(values, "provider_claims_transaction_finality_authority"));
  metadata.provider_claims_visibility_authority =
      ParseBool(ValueOr(values, "provider_claims_visibility_authority"));
  metadata.time_series_rollup_candidate_present =
      ParseBool(ValueOr(values, "time_series_rollup_candidate_present"));
  metadata.time_series_rollup_capability_uuid =
      ValueOr(values, "time_series_rollup_capability_uuid");
  metadata.time_series_rollup_generation =
      ParseU64(ValueOr(values, "time_series_rollup_generation"));
  metadata.time_series_visible_late_arrival_generation = ParseU64(
      ValueOr(values, "time_series_visible_late_arrival_generation"));
  metadata.time_series_rollup_interval_ns =
      ParseI64(ValueOr(values, "time_series_rollup_interval_ns"));
  metadata.time_series_rollup_exactness_attestation_state =
      ValueOr(values, "time_series_rollup_exactness_attestation_state");
  metadata.time_series_rollup_statement_snapshot_uuid =
      ValueOr(values, "time_series_rollup_statement_snapshot_uuid");
  metadata.time_series_rollup_statement_metadata_snapshot_uuid =
      ValueOr(values,
              "time_series_rollup_statement_metadata_snapshot_uuid");
  metadata.time_series_rollup_owning_transaction_uuid =
      ValueOr(values, "time_series_rollup_owning_transaction_uuid");
  metadata.time_series_rollup_local_transaction_id =
      ParseU64(ValueOr(values, "time_series_rollup_local_transaction_id"));
  metadata.time_series_rollup_snapshot_visible_through_local_transaction_id =
      ParseU64(ValueOr(
          values,
          "time_series_rollup_snapshot_visible_through_local_transaction_id"));
  metadata.time_series_rollup_security_context_uuid =
      ValueOr(values, "time_series_rollup_security_context_uuid");
  metadata.time_series_rollup_catalog_epoch_uuid =
      ValueOr(values, "time_series_rollup_catalog_epoch_uuid");
  metadata.time_series_rollup_exact_residual_recheck_required = ParseBool(
      ValueOr(values, "time_series_rollup_exact_residual_recheck_required"));
  metadata.time_series_rollup_base_row_mga_recheck_required = ParseBool(
      ValueOr(values, "time_series_rollup_base_row_mga_recheck_required"));
  metadata.time_series_rollup_security_recheck_required = ParseBool(
      ValueOr(values, "time_series_rollup_security_recheck_required"));
  metadata.vector_ann_candidate_present =
      ParseBool(ValueOr(values, "vector_ann_candidate_present"));
  metadata.vector_ann_capability_uuid =
      ValueOr(values, "vector_ann_capability_uuid");
  metadata.vector_ann_index_uuid = ValueOr(values, "vector_ann_index_uuid");
  metadata.vector_ann_base_relation_uuid =
      ValueOr(values, "vector_ann_base_relation_uuid");
  metadata.vector_ann_base_relation_generation =
      ParseU64(ValueOr(values, "vector_ann_base_relation_generation"));
  metadata.vector_ann_relation_descriptor_uuid =
      ValueOr(values, "vector_ann_relation_descriptor_uuid");
  metadata.vector_ann_relation_descriptor_generation = ParseU64(
      ValueOr(values, "vector_ann_relation_descriptor_generation"));
  metadata.vector_ann_embedding_column_uuid =
      ValueOr(values, "vector_ann_embedding_column_uuid");
  metadata.vector_ann_embedding_descriptor_uuid =
      ValueOr(values, "vector_ann_embedding_descriptor_uuid");
  metadata.vector_ann_embedding_type_uuid =
      ValueOr(values, "vector_ann_embedding_type_uuid");
  metadata.vector_ann_dimension =
      ParseU64(ValueOr(values, "vector_ann_dimension"));
  metadata.vector_ann_element_profile =
      ValueOr(values, "vector_ann_element_profile");
  metadata.vector_ann_metric_id = ValueOr(values, "vector_ann_metric_id");
  metadata.vector_ann_algorithm_id =
      ValueOr(values, "vector_ann_algorithm_id");
  metadata.vector_ann_publish_attestation_state =
      ValueOr(values, "vector_ann_publish_attestation_state");
  metadata.vector_ann_checksum_valid =
      ParseBool(ValueOr(values, "vector_ann_checksum_valid"));
  metadata.vector_ann_sealed_generation =
      ParseBool(ValueOr(values, "vector_ann_sealed_generation"));
  metadata.vector_ann_recall_attestation_present =
      ParseBool(ValueOr(values, "vector_ann_recall_attestation_present"));
  metadata.vector_ann_recall_contract_top_k =
      ParseU64(ValueOr(values, "vector_ann_recall_contract_top_k"));
  metadata.vector_ann_recall_sample_rows =
      ParseU64(ValueOr(values, "vector_ann_recall_sample_rows"));
  metadata.vector_ann_required_recall_ppm =
      ParseU64(ValueOr(values, "vector_ann_required_recall_ppm"));
  metadata.vector_ann_observed_recall_ppm =
      ParseU64(ValueOr(values, "vector_ann_observed_recall_ppm"));
  metadata.vector_ann_recall_sample_deterministic = ParseBool(
      ValueOr(values, "vector_ann_recall_sample_deterministic"));
  metadata.vector_ann_recall_evidence_uuid =
      ValueOr(values, "vector_ann_recall_evidence_uuid");
  metadata.vector_ann_statement_uuid =
      ValueOr(values, "vector_ann_statement_uuid");
  metadata.vector_ann_statement_snapshot_uuid =
      ValueOr(values, "vector_ann_statement_snapshot_uuid");
  metadata.vector_ann_statement_metadata_snapshot_uuid =
      ValueOr(values, "vector_ann_statement_metadata_snapshot_uuid");
  metadata.vector_ann_owning_transaction_uuid =
      ValueOr(values, "vector_ann_owning_transaction_uuid");
  metadata.vector_ann_local_transaction_id =
      ParseU64(ValueOr(values, "vector_ann_local_transaction_id"));
  metadata.vector_ann_snapshot_visible_through_local_transaction_id =
      ParseU64(ValueOr(
          values,
          "vector_ann_snapshot_visible_through_local_transaction_id"));
  metadata.vector_ann_security_context_uuid =
      ValueOr(values, "vector_ann_security_context_uuid");
  metadata.vector_ann_catalog_epoch_uuid =
      ValueOr(values, "vector_ann_catalog_epoch_uuid");
  metadata.vector_ann_exact_fallback_available =
      ParseBool(ValueOr(values, "vector_ann_exact_fallback_available"));
  metadata.vector_ann_full_base_exact_recheck_required = ParseBool(
      ValueOr(values, "vector_ann_full_base_exact_recheck_required"));
  metadata.vector_ann_base_row_mga_recheck_required = ParseBool(
      ValueOr(values, "vector_ann_base_row_mga_recheck_required"));
  metadata.vector_ann_security_recheck_required =
      ParseBool(ValueOr(values, "vector_ann_security_recheck_required"));
  metadata.vector_ann_index_claims_visibility_authority = ParseBool(
      ValueOr(values, "vector_ann_index_claims_visibility_authority"));
  metadata.vector_ann_index_claims_transaction_finality_authority = ParseBool(
      ValueOr(values,
              "vector_ann_index_claims_transaction_finality_authority"));
  metadata.vector_ann_parser_claims_visibility_authority = ParseBool(
      ValueOr(values, "vector_ann_parser_claims_visibility_authority"));
  metadata.vector_ann_parser_claims_transaction_finality_authority = ParseBool(
      ValueOr(values,
              "vector_ann_parser_claims_transaction_finality_authority"));
  metadata.vector_ann_client_claims_visibility_authority = ParseBool(
      ValueOr(values, "vector_ann_client_claims_visibility_authority"));
  metadata.vector_ann_client_claims_transaction_finality_authority = ParseBool(
      ValueOr(values,
              "vector_ann_client_claims_transaction_finality_authority"));
  metadata.vector_ann_reference_claims_visibility_authority = ParseBool(
      ValueOr(values, "vector_ann_reference_claims_visibility_authority"));
  metadata.vector_ann_reference_claims_transaction_finality_authority =
      ParseBool(ValueOr(
          values,
          "vector_ann_reference_claims_transaction_finality_authority"));
  metadata.vector_ann_wal_claims_visibility_authority = ParseBool(
      ValueOr(values, "vector_ann_wal_claims_visibility_authority"));
  metadata.vector_ann_wal_claims_transaction_finality_authority = ParseBool(
      ValueOr(values, "vector_ann_wal_claims_transaction_finality_authority"));
  metadata.search_segment_candidate_present =
      ParseBool(ValueOr(values, "search_segment_candidate_present"));
  metadata.search_segment_capability_uuid =
      ValueOr(values, "search_segment_capability_uuid");
  metadata.search_segment_index_uuid =
      ValueOr(values, "search_segment_index_uuid");
  metadata.search_segment_uuid = ValueOr(values, "search_segment_uuid");
  metadata.search_segment_base_relation_uuid =
      ValueOr(values, "search_segment_base_relation_uuid");
  metadata.search_segment_base_relation_generation =
      ParseU64(ValueOr(values, "search_segment_base_relation_generation"));
  metadata.search_segment_relation_descriptor_uuid =
      ValueOr(values, "search_segment_relation_descriptor_uuid");
  metadata.search_segment_relation_descriptor_generation = ParseU64(
      ValueOr(values, "search_segment_relation_descriptor_generation"));
  metadata.search_segment_body_column_uuid =
      ValueOr(values, "search_segment_body_column_uuid");
  metadata.search_segment_body_descriptor_uuid =
      ValueOr(values, "search_segment_body_descriptor_uuid");
  metadata.search_segment_body_type_uuid =
      ValueOr(values, "search_segment_body_type_uuid");
  metadata.search_segment_category_column_uuid =
      ValueOr(values, "search_segment_category_column_uuid");
  metadata.search_segment_category_descriptor_uuid =
      ValueOr(values, "search_segment_category_descriptor_uuid");
  metadata.search_segment_category_type_uuid =
      ValueOr(values, "search_segment_category_type_uuid");
  metadata.search_segment_search_type_descriptor_uuid =
      ValueOr(values, "search_segment_search_type_descriptor_uuid");
  metadata.search_segment_search_type_descriptor_generation = ParseU64(
      ValueOr(values, "search_segment_search_type_descriptor_generation"));
  metadata.search_segment_analyzer_uuid =
      ValueOr(values, "search_segment_analyzer_uuid");
  metadata.search_segment_analyzer_generation =
      ParseU64(ValueOr(values, "search_segment_analyzer_generation"));
  metadata.search_segment_analyzer_pipeline_sha256 =
      ValueOr(values, "search_segment_analyzer_pipeline_sha256");
  metadata.search_segment_tokenizer_uuid =
      ValueOr(values, "search_segment_tokenizer_uuid");
  metadata.search_segment_tokenizer_generation =
      ParseU64(ValueOr(values, "search_segment_tokenizer_generation"));
  metadata.search_segment_language_profile_uuid =
      ValueOr(values, "search_segment_language_profile_uuid");
  metadata.search_segment_language_profile_generation = ParseU64(
      ValueOr(values, "search_segment_language_profile_generation"));
  metadata.search_segment_ranking_model_uuid =
      ValueOr(values, "search_segment_ranking_model_uuid");
  metadata.search_segment_ranking_model_generation = ParseU64(
      ValueOr(values, "search_segment_ranking_model_generation"));
  metadata.search_segment_phrase_profile_uuid =
      ValueOr(values, "search_segment_phrase_profile_uuid");
  metadata.search_segment_phrase_profile_generation = ParseU64(
      ValueOr(values, "search_segment_phrase_profile_generation"));
  metadata.search_segment_query_syntax_profile_uuid =
      ValueOr(values, "search_segment_query_syntax_profile_uuid");
  metadata.search_segment_query_syntax_profile_generation = ParseU64(
      ValueOr(values, "search_segment_query_syntax_profile_generation"));
  metadata.search_segment_index_profile_id =
      ValueOr(values, "search_segment_index_profile_id");
  metadata.search_segment_generation =
      ParseU64(ValueOr(values, "search_segment_generation"));
  metadata.search_segment_position_payload_present =
      ParseBool(ValueOr(values, "search_segment_position_payload_present"));
  metadata.search_segment_checksum_valid =
      ParseBool(ValueOr(values, "search_segment_checksum_valid"));
  metadata.search_segment_sealed_generation =
      ParseBool(ValueOr(values, "search_segment_sealed_generation"));
  metadata.search_segment_publish_attestation_state =
      ValueOr(values, "search_segment_publish_attestation_state");
  metadata.search_segment_statement_uuid =
      ValueOr(values, "search_segment_statement_uuid");
  metadata.search_segment_statement_snapshot_uuid =
      ValueOr(values, "search_segment_statement_snapshot_uuid");
  metadata.search_segment_statement_metadata_snapshot_uuid =
      ValueOr(values, "search_segment_statement_metadata_snapshot_uuid");
  metadata.search_segment_owning_transaction_uuid =
      ValueOr(values, "search_segment_owning_transaction_uuid");
  metadata.search_segment_local_transaction_id =
      ParseU64(ValueOr(values, "search_segment_local_transaction_id"));
  metadata.search_segment_snapshot_visible_through_local_transaction_id =
      ParseU64(ValueOr(
          values,
          "search_segment_snapshot_visible_through_local_transaction_id"));
  metadata.search_segment_security_context_uuid =
      ValueOr(values, "search_segment_security_context_uuid");
  metadata.search_segment_catalog_epoch_uuid =
      ValueOr(values, "search_segment_catalog_epoch_uuid");
  metadata.search_segment_exact_fallback_available =
      ParseBool(ValueOr(values, "search_segment_exact_fallback_available"));
  metadata.search_segment_full_corpus_exact_recheck_required = ParseBool(
      ValueOr(values, "search_segment_full_corpus_exact_recheck_required"));
  metadata.search_segment_residual_recheck_required = ParseBool(
      ValueOr(values, "search_segment_residual_recheck_required"));
  metadata.search_segment_base_row_mga_recheck_required = ParseBool(
      ValueOr(values, "search_segment_base_row_mga_recheck_required"));
  metadata.search_segment_security_recheck_required =
      ParseBool(ValueOr(values, "search_segment_security_recheck_required"));
  metadata.search_segment_index_claims_visibility_authority = ParseBool(
      ValueOr(values, "search_segment_index_claims_visibility_authority"));
  metadata.search_segment_index_claims_transaction_finality_authority =
      ParseBool(ValueOr(
          values,
          "search_segment_index_claims_transaction_finality_authority"));
  metadata.search_segment_parser_claims_visibility_authority = ParseBool(
      ValueOr(values, "search_segment_parser_claims_visibility_authority"));
  metadata.search_segment_parser_claims_transaction_finality_authority =
      ParseBool(ValueOr(
          values,
          "search_segment_parser_claims_transaction_finality_authority"));
  metadata.search_segment_client_claims_visibility_authority = ParseBool(
      ValueOr(values, "search_segment_client_claims_visibility_authority"));
  metadata.search_segment_client_claims_transaction_finality_authority =
      ParseBool(ValueOr(
          values,
          "search_segment_client_claims_transaction_finality_authority"));
  metadata.search_segment_reference_claims_visibility_authority = ParseBool(
      ValueOr(values, "search_segment_reference_claims_visibility_authority"));
  metadata.search_segment_reference_claims_transaction_finality_authority =
      ParseBool(ValueOr(
          values,
          "search_segment_reference_claims_transaction_finality_authority"));
  metadata.search_segment_wal_claims_visibility_authority = ParseBool(
      ValueOr(values, "search_segment_wal_claims_visibility_authority"));
  metadata.search_segment_wal_claims_transaction_finality_authority =
      ParseBool(ValueOr(
          values,
          "search_segment_wal_claims_transaction_finality_authority"));
  return metadata;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineNoSqlProviderGenerationResult Failure(const EngineRequestContext& context,
                                            const std::string& operation_id,
                                            const char* detail) {
  (void)context;
  EngineNoSqlProviderGenerationResult result;
  result.ok = false;
  result.diagnostic = MakeInvalidRequestDiagnostic(operation_id, detail);
  result.evidence.push_back(std::string("provider_generation_refusal=") +
                            detail);
  result.evidence.push_back("provider_generation_fail_closed=true");
  result.evidence.push_back("provider_generation_finality_authority=false");
  result.evidence.push_back("provider_generation_visibility_authority=false");
  result.evidence.push_back(
      "provider_generation_mga_authority=engine_transaction_inventory");
  return result;
}

void AddCommonEvidence(EngineNoSqlProviderGenerationResult* result) {
  const auto& metadata = result->metadata;
  result->evidence.push_back("provider_generation_family=" +
                             std::string(EngineNoSqlProviderFamilyName(
                                 metadata.family)));
  result->evidence.push_back("provider_generation_provider_id=" +
                             metadata.provider_id);
  result->evidence.push_back("provider_generation_database_identity=" +
                             metadata.database_identity);
  result->evidence.push_back("provider_generation_collection_uuid=" +
                             metadata.collection_uuid);
  result->evidence.push_back("provider_generation_uuid=" +
                             metadata.generation_uuid);
  result->evidence.push_back("provider_generation_id=" +
                             std::to_string(metadata.generation_id));
  result->evidence.push_back("provider_generation_descriptor_epoch=" +
                             std::to_string(metadata.descriptor_epoch));
  result->evidence.push_back("provider_generation_security_epoch=" +
                             std::to_string(metadata.security_epoch));
  result->evidence.push_back("provider_generation_redaction_epoch=" +
                             std::to_string(metadata.redaction_epoch));
  result->evidence.push_back("provider_generation_catalog_epoch=" +
                             std::to_string(metadata.catalog_epoch));
  result->evidence.push_back("provider_generation_publish_state=" +
                             metadata.publish_state);
  result->evidence.push_back("provider_generation_validation_state=" +
                             metadata.validation_state);
  result->evidence.push_back("provider_generation_backup_metadata_ref=" +
                             metadata.backup_metadata_ref);
  result->evidence.push_back("provider_generation_restore_metadata_ref=" +
                             metadata.restore_metadata_ref);
  result->evidence.push_back("provider_generation_repair_metadata_ref=" +
                             metadata.repair_metadata_ref);
  result->evidence.push_back("provider_generation_support_bundle_evidence_id=" +
                             metadata.support_bundle_evidence_id);
  result->evidence.push_back("provider_generation_finality_authority=false");
  result->evidence.push_back("provider_generation_visibility_authority=false");
  result->evidence.push_back(
      "provider_generation_mga_authority=engine_transaction_inventory");
  result->evidence.push_back(
      "provider_generation_time_series_rollup_candidate_present=" +
      BoolText(metadata.time_series_rollup_candidate_present));
  result->evidence.push_back(
      "provider_generation_time_series_rollup_capability_uuid=" +
      metadata.time_series_rollup_capability_uuid);
  result->evidence.push_back(
      "provider_generation_time_series_rollup_generation=" +
      std::to_string(metadata.time_series_rollup_generation));
  result->evidence.push_back(
      "provider_generation_time_series_visible_late_arrival_generation=" +
      std::to_string(
          metadata.time_series_visible_late_arrival_generation));
  result->evidence.push_back(
      "provider_generation_time_series_rollup_interval_ns=" +
      std::to_string(metadata.time_series_rollup_interval_ns));
  result->evidence.push_back(
      "provider_generation_time_series_rollup_exactness_attestation_state=" +
      metadata.time_series_rollup_exactness_attestation_state);
  result->evidence.push_back(
      "provider_generation_time_series_rollup_statement_snapshot_uuid=" +
      metadata.time_series_rollup_statement_snapshot_uuid);
  result->evidence.push_back(
      "provider_generation_time_series_rollup_statement_metadata_snapshot_uuid=" +
      metadata.time_series_rollup_statement_metadata_snapshot_uuid);
  result->evidence.push_back(
      "provider_generation_time_series_rollup_owning_transaction_uuid=" +
      metadata.time_series_rollup_owning_transaction_uuid);
  result->evidence.push_back(
      "provider_generation_time_series_rollup_local_transaction_id=" +
      std::to_string(metadata.time_series_rollup_local_transaction_id));
  result->evidence.push_back(
      "provider_generation_time_series_rollup_snapshot_visible_through_local_transaction_id=" +
      std::to_string(metadata
                         .time_series_rollup_snapshot_visible_through_local_transaction_id));
  result->evidence.push_back(
      "provider_generation_time_series_rollup_security_context_uuid=" +
      metadata.time_series_rollup_security_context_uuid);
  result->evidence.push_back(
      "provider_generation_time_series_rollup_catalog_epoch_uuid=" +
      metadata.time_series_rollup_catalog_epoch_uuid);
  result->evidence.push_back(
      "provider_generation_time_series_rollup_exact_residual_recheck_required=" +
      BoolText(metadata.time_series_rollup_exact_residual_recheck_required));
  result->evidence.push_back(
      "provider_generation_time_series_rollup_base_row_mga_recheck_required=" +
      BoolText(metadata.time_series_rollup_base_row_mga_recheck_required));
  result->evidence.push_back(
      "provider_generation_time_series_rollup_security_recheck_required=" +
      BoolText(metadata.time_series_rollup_security_recheck_required));
  for (const auto& [key, value] : MetadataPairs(metadata)) {
    if (key.starts_with("vector_ann_")) {
      result->evidence.push_back("provider_generation_" + key + "=" + value);
    }
  }
  result->evidence.push_back(
      "provider_generation_vector_ann_binding_valid=" +
      BoolText(metadata.vector_ann_candidate_present &&
               ValidateVectorAnnCapabilityBindingV1(metadata)));
  for (const auto& [key, value] : MetadataPairs(metadata)) {
    if (key.starts_with("search_segment_")) {
      result->evidence.push_back("provider_generation_" + key + "=" + value);
    }
  }
  result->evidence.push_back(
      "provider_generation_search_segment_binding_valid=" +
      BoolText(metadata.search_segment_candidate_present &&
               ValidateSearchSegmentCapabilityBindingV1(metadata)));
}

bool Matches(const EngineNoSqlProviderGenerationMetadata& metadata,
             EngineNoSqlProviderFamily family,
             const std::string& provider_id,
             const std::string& collection_uuid) {
  return metadata.family == family && metadata.provider_id == provider_id &&
         metadata.collection_uuid == collection_uuid;
}

std::string GenerationKey(const EngineNoSqlProviderGenerationMetadata& metadata) {
  return std::string(EngineNoSqlProviderFamilyName(metadata.family)) + "\x1f" +
         metadata.provider_id + "\x1f" + metadata.collection_uuid;
}

bool BoundToContext(const EngineRequestContext& context,
                    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!context.database_uuid.canonical.empty() &&
      !metadata.database_uuid.empty() &&
      IsValidUuid(context.database_uuid.canonical) &&
      IsValidUuid(metadata.database_uuid)) {
    return context.database_uuid.canonical == metadata.database_uuid;
  }
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  return metadata.database_identity.empty() || metadata.database_identity == identity;
}

bool HasDefaultTimeSeriesRollupCarrier(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return !metadata.time_series_rollup_candidate_present &&
         metadata.time_series_rollup_capability_uuid.empty() &&
         metadata.time_series_rollup_generation == 0 &&
         metadata.time_series_visible_late_arrival_generation == 0 &&
         metadata.time_series_rollup_interval_ns == 0 &&
         metadata.time_series_rollup_exactness_attestation_state.empty() &&
         metadata.time_series_rollup_statement_snapshot_uuid.empty() &&
         metadata.time_series_rollup_statement_metadata_snapshot_uuid.empty() &&
         metadata.time_series_rollup_owning_transaction_uuid.empty() &&
         metadata.time_series_rollup_local_transaction_id == 0 &&
         metadata
                 .time_series_rollup_snapshot_visible_through_local_transaction_id ==
             0 &&
         metadata.time_series_rollup_security_context_uuid.empty() &&
         metadata.time_series_rollup_catalog_epoch_uuid.empty() &&
         !metadata.time_series_rollup_exact_residual_recheck_required &&
         !metadata.time_series_rollup_base_row_mga_recheck_required &&
         !metadata.time_series_rollup_security_recheck_required;
}

bool HasValidTimeSeriesRollupCarrier(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!metadata.time_series_rollup_candidate_present) {
    return HasDefaultTimeSeriesRollupCarrier(metadata);
  }
  return HasExactTimeSeriesRollupBindingInput(metadata) &&
         metadata.time_series_rollup_capability_uuid !=
             metadata.generation_uuid &&
         metadata.time_series_rollup_capability_uuid ==
             DeriveTimeSeriesRollupCapabilityUuidImpl(metadata);
}

bool HasDefaultVectorAnnCarrier(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return !metadata.vector_ann_candidate_present &&
         metadata.vector_ann_capability_uuid.empty() &&
         metadata.vector_ann_index_uuid.empty() &&
         metadata.vector_ann_base_relation_uuid.empty() &&
         metadata.vector_ann_base_relation_generation == 0 &&
         metadata.vector_ann_relation_descriptor_uuid.empty() &&
         metadata.vector_ann_relation_descriptor_generation == 0 &&
         metadata.vector_ann_embedding_column_uuid.empty() &&
         metadata.vector_ann_embedding_descriptor_uuid.empty() &&
         metadata.vector_ann_embedding_type_uuid.empty() &&
         metadata.vector_ann_dimension == 0 &&
         metadata.vector_ann_element_profile.empty() &&
         metadata.vector_ann_metric_id.empty() &&
         metadata.vector_ann_algorithm_id.empty() &&
         metadata.vector_ann_publish_attestation_state.empty() &&
         !metadata.vector_ann_checksum_valid &&
         !metadata.vector_ann_sealed_generation &&
         !metadata.vector_ann_recall_attestation_present &&
         metadata.vector_ann_recall_contract_top_k == 0 &&
         metadata.vector_ann_recall_sample_rows == 0 &&
         metadata.vector_ann_required_recall_ppm == 0 &&
         metadata.vector_ann_observed_recall_ppm == 0 &&
         !metadata.vector_ann_recall_sample_deterministic &&
         metadata.vector_ann_recall_evidence_uuid.empty() &&
         metadata.vector_ann_statement_uuid.empty() &&
         metadata.vector_ann_statement_snapshot_uuid.empty() &&
         metadata.vector_ann_statement_metadata_snapshot_uuid.empty() &&
         metadata.vector_ann_owning_transaction_uuid.empty() &&
         metadata.vector_ann_local_transaction_id == 0 &&
         metadata.vector_ann_snapshot_visible_through_local_transaction_id ==
             0 &&
         metadata.vector_ann_security_context_uuid.empty() &&
         metadata.vector_ann_catalog_epoch_uuid.empty() &&
         !metadata.vector_ann_exact_fallback_available &&
         !metadata.vector_ann_full_base_exact_recheck_required &&
         !metadata.vector_ann_base_row_mga_recheck_required &&
         !metadata.vector_ann_security_recheck_required &&
         !metadata.vector_ann_index_claims_visibility_authority &&
         !metadata.vector_ann_index_claims_transaction_finality_authority &&
         !metadata.vector_ann_parser_claims_visibility_authority &&
         !metadata.vector_ann_parser_claims_transaction_finality_authority &&
         !metadata.vector_ann_client_claims_visibility_authority &&
         !metadata.vector_ann_client_claims_transaction_finality_authority &&
         !metadata.vector_ann_reference_claims_visibility_authority &&
         !metadata.vector_ann_reference_claims_transaction_finality_authority &&
         !metadata.vector_ann_wal_claims_visibility_authority &&
         !metadata.vector_ann_wal_claims_transaction_finality_authority;
}

bool HasValidVectorAnnCarrier(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!metadata.vector_ann_candidate_present) {
    return HasDefaultVectorAnnCarrier(metadata);
  }
  const auto derived = DeriveVectorAnnCapabilityUuidImpl(metadata);
  return !derived.empty() &&
         metadata.vector_ann_capability_uuid != metadata.generation_uuid &&
         ConstantTimeTextEqual(metadata.vector_ann_capability_uuid, derived);
}

bool HasDefaultSearchSegmentCarrier(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return !metadata.search_segment_candidate_present &&
         metadata.search_segment_capability_uuid.empty() &&
         metadata.search_segment_index_uuid.empty() &&
         metadata.search_segment_uuid.empty() &&
         metadata.search_segment_base_relation_uuid.empty() &&
         metadata.search_segment_base_relation_generation == 0 &&
         metadata.search_segment_relation_descriptor_uuid.empty() &&
         metadata.search_segment_relation_descriptor_generation == 0 &&
         metadata.search_segment_body_column_uuid.empty() &&
         metadata.search_segment_body_descriptor_uuid.empty() &&
         metadata.search_segment_body_type_uuid.empty() &&
         metadata.search_segment_category_column_uuid.empty() &&
         metadata.search_segment_category_descriptor_uuid.empty() &&
         metadata.search_segment_category_type_uuid.empty() &&
         metadata.search_segment_search_type_descriptor_uuid.empty() &&
         metadata.search_segment_search_type_descriptor_generation == 0 &&
         metadata.search_segment_analyzer_uuid.empty() &&
         metadata.search_segment_analyzer_generation == 0 &&
         metadata.search_segment_analyzer_pipeline_sha256.empty() &&
         metadata.search_segment_tokenizer_uuid.empty() &&
         metadata.search_segment_tokenizer_generation == 0 &&
         metadata.search_segment_language_profile_uuid.empty() &&
         metadata.search_segment_language_profile_generation == 0 &&
         metadata.search_segment_ranking_model_uuid.empty() &&
         metadata.search_segment_ranking_model_generation == 0 &&
         metadata.search_segment_phrase_profile_uuid.empty() &&
         metadata.search_segment_phrase_profile_generation == 0 &&
         metadata.search_segment_query_syntax_profile_uuid.empty() &&
         metadata.search_segment_query_syntax_profile_generation == 0 &&
         metadata.search_segment_index_profile_id.empty() &&
         metadata.search_segment_generation == 0 &&
         !metadata.search_segment_position_payload_present &&
         !metadata.search_segment_checksum_valid &&
         !metadata.search_segment_sealed_generation &&
         metadata.search_segment_publish_attestation_state.empty() &&
         metadata.search_segment_statement_uuid.empty() &&
         metadata.search_segment_statement_snapshot_uuid.empty() &&
         metadata.search_segment_statement_metadata_snapshot_uuid.empty() &&
         metadata.search_segment_owning_transaction_uuid.empty() &&
         metadata.search_segment_local_transaction_id == 0 &&
         metadata
                 .search_segment_snapshot_visible_through_local_transaction_id ==
             0 &&
         metadata.search_segment_security_context_uuid.empty() &&
         metadata.search_segment_catalog_epoch_uuid.empty() &&
         !metadata.search_segment_exact_fallback_available &&
         !metadata.search_segment_full_corpus_exact_recheck_required &&
         !metadata.search_segment_residual_recheck_required &&
         !metadata.search_segment_base_row_mga_recheck_required &&
         !metadata.search_segment_security_recheck_required &&
         !metadata.search_segment_index_claims_visibility_authority &&
         !metadata.search_segment_index_claims_transaction_finality_authority &&
         !metadata.search_segment_parser_claims_visibility_authority &&
         !metadata.search_segment_parser_claims_transaction_finality_authority &&
         !metadata.search_segment_client_claims_visibility_authority &&
         !metadata.search_segment_client_claims_transaction_finality_authority &&
         !metadata.search_segment_reference_claims_visibility_authority &&
         !metadata.search_segment_reference_claims_transaction_finality_authority &&
         !metadata.search_segment_wal_claims_visibility_authority &&
         !metadata.search_segment_wal_claims_transaction_finality_authority;
}

bool HasValidSearchSegmentCarrier(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!metadata.search_segment_candidate_present) {
    return HasDefaultSearchSegmentCarrier(metadata);
  }
  const auto derived = DeriveSearchSegmentCapabilityUuidImpl(metadata);
  return !derived.empty() &&
         metadata.search_segment_capability_uuid != metadata.generation_uuid &&
         ConstantTimeTextEqual(metadata.search_segment_capability_uuid,
                               derived);
}

bool HasLifecycleMetadata(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return metadata.family != EngineNoSqlProviderFamily::kUnknown &&
         (!metadata.database_identity.empty() ||
          !metadata.database_uuid.empty()) &&
         (metadata.database_uuid.empty() ||
          IsValidUuid(metadata.database_uuid)) &&
         !metadata.provider_id.empty() && !metadata.collection_uuid.empty() &&
         metadata.generation_id != 0 &&
         IsValidUuid(metadata.generation_uuid) &&
         !metadata.publish_state.empty() && !metadata.validation_state.empty() &&
         !metadata.backup_metadata_ref.empty() &&
         !metadata.restore_metadata_ref.empty() &&
         !metadata.repair_metadata_ref.empty() &&
         !metadata.support_bundle_evidence_id.empty() &&
         HasValidTimeSeriesRollupCarrier(metadata) &&
         HasValidVectorAnnCarrier(metadata) &&
         HasValidSearchSegmentCarrier(metadata) &&
         (metadata.family == EngineNoSqlProviderFamily::kVector ||
          !metadata.vector_ann_candidate_present) &&
         (metadata.family == EngineNoSqlProviderFamily::kSearch ||
          !metadata.search_segment_candidate_present);
}

bool MetadataRefMismatch(
    const EngineNoSqlProviderGenerationProof& proof,
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return (!proof.backup_metadata_ref.empty() &&
          proof.backup_metadata_ref != metadata.backup_metadata_ref) ||
         (!proof.restore_metadata_ref.empty() &&
          proof.restore_metadata_ref != metadata.restore_metadata_ref) ||
         (!proof.repair_metadata_ref.empty() &&
          proof.repair_metadata_ref != metadata.repair_metadata_ref) ||
         (!proof.support_bundle_evidence_id.empty() &&
          proof.support_bundle_evidence_id != metadata.support_bundle_evidence_id);
}

bool RewriteLocked(
    const EngineRequestContext& context,
    const std::vector<EngineNoSqlProviderGenerationMetadata>& generations) {
  const auto path = GenerationPath(context);
  if (path.empty()) { return true; }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) { return false; }
  for (const auto& metadata : generations) {
    out << kGenerationMagic << "\tGENERATION\t"
        << EncodeCrudPairs(MetadataPairs(metadata)) << '\n';
  }
  out.flush();
  return static_cast<bool>(out);
}

bool ExistingFileNeedsRecordSeparator(const std::string& path) {
  if (path.empty()) return false;
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size == 0) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(static_cast<std::streamoff>(size - 1));
  char last = '\0';
  in.get(last);
  return in && last != '\n';
}

std::vector<EngineNoSqlProviderGenerationMetadata> LoadLocked(
    const EngineRequestContext& context) {
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  auto cache_it = GenerationCache().find(identity);
  if (cache_it != GenerationCache().end()) {
    return cache_it->second;
  }

  std::vector<EngineNoSqlProviderGenerationMetadata> loaded;
  // Pre-existing ordinary families retain latest-wins log behavior. Active
  // vector/search carriers are retained as cohorts so a duplicated raw
  // carrier cannot be normalized away before canonical admission rejects it.
  std::map<std::string,
           std::vector<EngineNoSqlProviderGenerationMetadata>> latest;
  const auto path = GenerationPath(context);
  if (!path.empty()) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind(kGenerationMagic, 0) != 0) {
        continue;
      }
      const auto parts = Split(line, '\t');
      if (parts.size() < 3 || parts[1] != std::string("GENERATION")) {
        if (parts.size() >= 3 && parts[1] == std::string("DROP")) {
          const auto decoded = DecodeCrudPairs(parts[2]);
          auto metadata = MetadataFromPairs(decoded);
          const bool raw_vector_valid = RawVectorCarrierPairsValid(decoded);
          const bool raw_search_valid = RawSearchCarrierPairsValid(decoded);
          if (!raw_vector_valid) {
            metadata.vector_ann_candidate_present = false;
            metadata.vector_ann_capability_uuid = "invalid-raw-carrier";
          }
          if (!raw_search_valid) {
            metadata.search_segment_candidate_present = false;
            metadata.search_segment_capability_uuid = "invalid-raw-carrier";
          }
          if (!raw_vector_valid || !raw_search_valid) {
            // A malformed tombstone is evidence of corrupt persistence, not
            // authority to erase the last valid generation and turn the
            // corruption into benign absence.
            latest[GenerationKey(metadata)] = {std::move(metadata)};
            continue;
          }
          if (BoundToContext(context, metadata) ||
              metadata.time_series_rollup_candidate_present ||
              !HasDefaultVectorAnnCarrier(metadata) ||
              !HasDefaultSearchSegmentCarrier(metadata)) {
            latest.erase(GenerationKey(metadata));
          }
        }
        continue;
      }
      const auto decoded = DecodeCrudPairs(parts[2]);
      auto metadata = MetadataFromPairs(decoded);
      if (!RawVectorCarrierPairsValid(decoded)) {
        // Preserve a nondefault sentinel so the corrupt record cannot be
        // normalized into a benign inactive/absent ANN generation.
        metadata.vector_ann_candidate_present = false;
        metadata.vector_ann_capability_uuid = "invalid-raw-carrier";
      }
      if (!RawSearchCarrierPairsValid(decoded)) {
        metadata.search_segment_candidate_present = false;
        metadata.search_segment_capability_uuid = "invalid-raw-carrier";
      }
      if (!BoundToContext(context, metadata) &&
          !metadata.time_series_rollup_candidate_present &&
          HasDefaultVectorAnnCarrier(metadata) &&
          HasDefaultSearchSegmentCarrier(metadata)) {
        continue;
      }
      // Candidate rows retain their decoded identity so the capability binding
      // remains an integrity check over the persisted bytes.  Normalizing a
      // substituted identity here would silently repair the signed value before
      // canonical admission sees it.  Legacy/default rows keep the historical
      // context normalization behavior.
      if (!metadata.time_series_rollup_candidate_present &&
          HasDefaultVectorAnnCarrier(metadata) &&
          HasDefaultSearchSegmentCarrier(metadata)) {
        metadata.database_identity = identity;
      }
      const auto key = GenerationKey(metadata);
      if ((metadata.family == EngineNoSqlProviderFamily::kVector &&
           !HasDefaultVectorAnnCarrier(metadata)) ||
          (metadata.family == EngineNoSqlProviderFamily::kSearch &&
           !HasDefaultSearchSegmentCarrier(metadata))) {
        latest[key].push_back(std::move(metadata));
      } else {
        latest[key] = {std::move(metadata)};
      }
    }
  }
  for (auto& [key, generations] : latest) {
    (void)key;
    for (auto& metadata : generations) {
      loaded.push_back(std::move(metadata));
    }
  }
  GenerationCache()[identity] = loaded;
  return loaded;
}

}  // namespace

std::string DeriveTimeSeriesRollupCapabilityUuidV1(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return DeriveTimeSeriesRollupCapabilityUuidImpl(metadata);
}

bool ValidateTimeSeriesRollupCapabilityBindingV1(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!metadata.time_series_rollup_candidate_present) return false;
  const auto derived = DeriveTimeSeriesRollupCapabilityUuidImpl(metadata);
  return !derived.empty() &&
         metadata.time_series_rollup_capability_uuid == derived;
}

std::string DeriveVectorAnnCapabilityUuidV1(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return DeriveVectorAnnCapabilityUuidImpl(metadata);
}

bool ValidateVectorAnnCapabilityBindingV1(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!metadata.vector_ann_candidate_present) return false;
  const auto derived = DeriveVectorAnnCapabilityUuidImpl(metadata);
  return !derived.empty() &&
         ConstantTimeTextEqual(metadata.vector_ann_capability_uuid, derived);
}

std::string DeriveSearchSegmentCapabilityUuidV1(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  return DeriveSearchSegmentCapabilityUuidImpl(metadata);
}

bool ValidateSearchSegmentCapabilityBindingV1(
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  if (!metadata.search_segment_candidate_present) return false;
  const auto derived = DeriveSearchSegmentCapabilityUuidImpl(metadata);
  return !derived.empty() &&
         ConstantTimeTextEqual(metadata.search_segment_capability_uuid,
                               derived);
}

std::string EngineNoSqlProviderDatabaseIdentity(
    const EngineRequestContext& context) {
  if (!context.database_path.empty()) {
    return context.database_path;
  }
  if (!context.database_uuid.canonical.empty()) {
    return context.database_uuid.canonical;
  }
  return "embedded_transient_nosql_provider";
}

EngineNoSqlProviderGenerationMetadata MakeDocumentProviderGenerationMetadata(
    const EngineRequestContext& context,
    const std::string& provider_id,
    const std::string& collection_uuid,
    std::uint64_t generation_id) {
  EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = EngineNoSqlProviderFamily::kDocument;
  metadata.provider_id = provider_id;
  metadata.database_identity = EngineNoSqlProviderDatabaseIdentity(context);
  metadata.database_uuid = IsValidUuid(context.database_uuid.canonical)
                               ? context.database_uuid.canonical
                               : GenerateCrudEngineUuid("database");
  metadata.collection_uuid = collection_uuid;
  metadata.generation_id = generation_id;
  metadata.generation_uuid = StableGenerationUuid(context,
                                                  provider_id,
                                                  collection_uuid,
                                                  generation_id);
  metadata.descriptor_epoch = NonZeroEpoch(context.resource_epoch);
  metadata.security_epoch = NonZeroEpoch(context.security_epoch);
  metadata.redaction_epoch = NonZeroEpoch(context.security_epoch);
  metadata.catalog_epoch = NonZeroEpoch(context.catalog_generation_id);
  metadata.publish_state = "published";
  metadata.validation_state = "validated";
  metadata.backup_metadata_ref =
      "backup.provider_generation:" + metadata.generation_uuid;
  metadata.restore_metadata_ref =
      "restore.provider_generation:" + metadata.generation_uuid;
  metadata.repair_metadata_ref =
      "repair.provider_generation:" + metadata.generation_uuid;
  metadata.support_bundle_evidence_id =
      "support.nosql_provider_generation:" + metadata.generation_uuid;
  return metadata;
}

EngineNoSqlProviderGenerationResult PublishNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlProviderGenerationMetadata& metadata) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  auto writable = metadata;
  writable.database_identity = EngineNoSqlProviderDatabaseIdentity(context);
  if (writable.database_uuid.empty() || !IsValidUuid(writable.database_uuid)) {
    writable.database_uuid = IsValidUuid(context.database_uuid.canonical)
                                 ? context.database_uuid.canonical
                                 : GenerateCrudEngineUuid("database");
  }
  if (writable.descriptor_epoch == 0) {
    writable.descriptor_epoch = NonZeroEpoch(context.resource_epoch);
  }
  if (writable.security_epoch == 0) {
    writable.security_epoch = NonZeroEpoch(context.security_epoch);
  }
  if (writable.redaction_epoch == 0) {
    writable.redaction_epoch = NonZeroEpoch(context.security_epoch);
  }
  if (writable.catalog_epoch == 0) {
    writable.catalog_epoch = NonZeroEpoch(context.catalog_generation_id);
  }
  if (!HasLifecycleMetadata(writable)) {
    return Failure(context,
                   "nosql.provider_generation.publish",
                   kNoSqlProviderGenerationMetadataMissing);
  }
  if (!BoundToContext(context, writable)) {
    return Failure(context,
                   "nosql.provider_generation.publish",
                   kNoSqlProviderGenerationIdentityMismatch);
  }
  if (writable.provider_claims_transaction_finality_authority ||
      writable.provider_claims_visibility_authority) {
    return Failure(context,
                   "nosql.provider_generation.publish",
                   kNoSqlProviderGenerationAuthorityRefused);
  }

  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  auto generations = LoadLocked(context);
  if (writable.family == EngineNoSqlProviderFamily::kVector ||
      writable.family == EngineNoSqlProviderFamily::kSearch) {
    std::size_t matching = 0;
    for (const auto& existing : generations) {
      if (!Matches(existing, writable.family, writable.provider_id,
                   writable.collection_uuid)) {
        continue;
      }
      ++matching;
      if ((writable.family == EngineNoSqlProviderFamily::kVector &&
           !HasValidVectorAnnCarrier(existing)) ||
          (writable.family == EngineNoSqlProviderFamily::kSearch &&
           !HasValidSearchSegmentCarrier(existing))) {
        return Failure(context,
                       "nosql.provider_generation.publish",
                       kNoSqlProviderGenerationMetadataMissing);
      }
    }
    if (matching > 1) {
      return Failure(context,
                     "nosql.provider_generation.publish",
                     kNoSqlProviderGenerationMetadataMissing);
    }
  }
  generations.erase(
      std::remove_if(generations.begin(),
                     generations.end(),
                     [&](const EngineNoSqlProviderGenerationMetadata& existing) {
                       return Matches(existing,
                                      writable.family,
                                      writable.provider_id,
                                      writable.collection_uuid);
                     }),
      generations.end());
  generations.push_back(writable);
  if (writable.family == EngineNoSqlProviderFamily::kVector ||
      writable.family == EngineNoSqlProviderFamily::kSearch) {
    // Vector/search publication is a single-current-generation replacement.
    // It never converts a matching corrupt/duplicate carrier into a repair.
    if (!RewriteLocked(context, generations)) {
      return Failure(context,
                     "nosql.provider_generation.publish",
                     kNoSqlProviderGenerationUnavailable);
    }
  } else {
    // Preserve the established append-log semantics, including unrelated and
    // forward-compatible records, for every pre-existing family.
    const auto path = GenerationPath(context);
    if (!path.empty()) {
      std::ofstream out(path, std::ios::binary | std::ios::app);
      if (!out) {
        return Failure(context,
                       "nosql.provider_generation.publish",
                       kNoSqlProviderGenerationUnavailable);
      }
      if (ExistingFileNeedsRecordSeparator(path)) out << '\n';
      out << kGenerationMagic << "\tGENERATION\t"
          << EncodeCrudPairs(MetadataPairs(writable)) << '\n';
      out.flush();
      if (!out) {
        return Failure(context,
                       "nosql.provider_generation.publish",
                       kNoSqlProviderGenerationUnavailable);
      }
    }
  }
  GenerationCache()[identity] = generations;

  EngineNoSqlProviderGenerationResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.metadata = std::move(writable);
  AddCommonEvidence(&result);
  result.evidence.push_back("provider_generation_persisted=true");
  result.evidence.push_back("provider_generation_concurrency_guard=mutex");
  result.evidence.push_back("provider_generation_support_bundle_ready=true");
  return result;
}

EngineNoSqlProviderGenerationResult LoadNoSqlProviderGeneration(
    const EngineRequestContext& context,
    EngineNoSqlProviderFamily family,
    const std::string& provider_id,
    const std::string& collection_uuid) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  const auto loaded = LoadLocked(context);
  EngineNoSqlProviderGenerationResult result;
  std::size_t matching_vector_carriers = 0;
  std::size_t matching_search_carriers = 0;
  for (const auto& metadata : loaded) {
    if (Matches(metadata, family, provider_id, collection_uuid) &&
        family == EngineNoSqlProviderFamily::kVector &&
        !HasDefaultVectorAnnCarrier(metadata)) {
      ++matching_vector_carriers;
    }
    if (Matches(metadata, family, provider_id, collection_uuid) &&
        family == EngineNoSqlProviderFamily::kSearch &&
        !HasDefaultSearchSegmentCarrier(metadata)) {
      ++matching_search_carriers;
    }
  }
  if (matching_vector_carriers > 1 || matching_search_carriers > 1) {
    return Failure(context,
                   "nosql.provider_generation.load",
                   kNoSqlProviderGenerationMetadataMissing);
  }
  for (const auto& metadata : loaded) {
    if (Matches(metadata, family, provider_id, collection_uuid)) {
      if (metadata.time_series_rollup_candidate_present &&
          !ValidateTimeSeriesRollupCapabilityBindingV1(metadata)) {
        return Failure(context,
                       "nosql.provider_generation.load",
                       kNoSqlProviderGenerationMetadataMissing);
      }
      if (!HasValidVectorAnnCarrier(metadata)) {
        return Failure(context,
                       "nosql.provider_generation.load",
                       kNoSqlProviderGenerationMetadataMissing);
      }
      if (!HasValidSearchSegmentCarrier(metadata)) {
        return Failure(context,
                       "nosql.provider_generation.load",
                       kNoSqlProviderGenerationMetadataMissing);
      }
      result.ok = true;
      result.diagnostic = OkDiagnostic();
      result.metadata = metadata;
      AddCommonEvidence(&result);
      result.evidence.push_back("provider_generation_loaded=true");
      return result;
    }
  }
  return Failure(context,
                 "nosql.provider_generation.load",
                 kNoSqlProviderGenerationUnavailable);
}

EngineNoSqlProviderGenerationResult ValidateNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlPhysicalProviderContract& contract) {
  if (!contract.provider_generation.required) {
    EngineNoSqlProviderGenerationResult result;
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back("provider_generation_required=false");
    return result;
  }
  if (!contract.provider_generation.proof_present) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationProofMissing);
  }
  auto loaded = LoadNoSqlProviderGeneration(context,
                                            contract.family,
                                            contract.provider_id,
                                            contract.provider_generation.collection_uuid);
  if (!loaded.ok) {
    return loaded;
  }

  const auto& metadata = loaded.metadata;
  const auto& proof = contract.provider_generation;
  const bool generation_mismatch =
      metadata.generation_id < proof.required_generation ||
      proof.available_generation < proof.required_generation ||
      (!proof.generation_uuid.empty() &&
       metadata.generation_uuid != proof.generation_uuid) ||
      (!proof.provider_id.empty() && metadata.provider_id != proof.provider_id) ||
      (!proof.database_uuid.empty() && IsValidUuid(proof.database_uuid) &&
       metadata.database_uuid != proof.database_uuid) ||
      (!proof.collection_uuid.empty() &&
       metadata.collection_uuid != proof.collection_uuid);
  if (generation_mismatch || !proof.visible_to_snapshot) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationStale);
  }

  const bool epoch_mismatch =
      (proof.descriptor_epoch != 0 &&
       metadata.descriptor_epoch != proof.descriptor_epoch) ||
      (proof.security_epoch != 0 &&
       metadata.security_epoch != proof.security_epoch) ||
      (proof.redaction_epoch != 0 &&
       metadata.redaction_epoch != proof.redaction_epoch) ||
      (proof.catalog_epoch != 0 &&
       metadata.catalog_epoch != proof.catalog_epoch);
  if (epoch_mismatch) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationEpochMismatch);
  }

  if (!proof.publish_state_bound || !proof.validation_state_bound ||
      metadata.publish_state != "published" ||
      metadata.validation_state != "validated") {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationStateUnvalidated);
  }

  if (!proof.backup_restore_repair_metadata_bound ||
      !proof.support_bundle_evidence_bound ||
      metadata.backup_metadata_ref.empty() ||
      metadata.restore_metadata_ref.empty() ||
      metadata.repair_metadata_ref.empty() ||
      metadata.support_bundle_evidence_id.empty() ||
      MetadataRefMismatch(proof, metadata)) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationMetadataMissing);
  }

  if (metadata.provider_claims_transaction_finality_authority ||
      metadata.provider_claims_visibility_authority ||
      proof.provider_claims_transaction_finality_authority ||
      proof.provider_claims_visibility_authority) {
    return Failure(context,
                   "nosql.provider_generation.validate",
                   kNoSqlProviderGenerationAuthorityRefused);
  }

  loaded.evidence.push_back("provider_generation_validated=true");
  loaded.evidence.push_back("provider_generation_epoch_bound=true");
  loaded.evidence.push_back("provider_generation_backup_restore_repair_bound=true");
  loaded.evidence.push_back("provider_generation_support_bundle_ready=true");
  return loaded;
}

EngineNoSqlProviderGenerationResult RepairNoSqlProviderGeneration(
    const EngineRequestContext& context,
    const EngineNoSqlProviderGenerationRepairRequest& request) {
  if (!request.repair_admitted) {
    return Failure(context,
                   "nosql.provider_generation.repair",
                   kNoSqlProviderGenerationRepairAdmissionRequired);
  }
  if (request.authoritative_source_generations.empty()) {
    return Failure(context,
                   "nosql.provider_generation.repair",
                   kNoSqlProviderGenerationRepairSourceMissing);
  }

  for (auto metadata : request.authoritative_source_generations) {
    if (!Matches(metadata,
                 request.family,
                 request.provider_id,
                 request.collection_uuid)) {
      continue;
    }
    if (!BoundToContext(context, metadata)) {
      return Failure(context,
                     "nosql.provider_generation.repair",
                     kNoSqlProviderGenerationIdentityMismatch);
    }
    if (!HasLifecycleMetadata(metadata)) {
      return Failure(context,
                     "nosql.provider_generation.repair",
                     kNoSqlProviderGenerationMetadataMissing);
    }
    metadata.publish_state = "published";
    metadata.validation_state = "validated";
    auto repaired = PublishNoSqlProviderGeneration(context, metadata);
    if (repaired.ok) {
      repaired.evidence.push_back("provider_generation_repair_admitted=true");
      repaired.evidence.push_back("provider_generation_repair_source=authoritative");
      repaired.evidence.push_back("provider_generation_repair_published=true");
      repaired.evidence.push_back("provider_generation_descriptor_scan_fallback=false");
    }
    return repaired;
  }

  return Failure(context,
                 "nosql.provider_generation.repair",
                 kNoSqlProviderGenerationRepairSourceMissing);
}

EngineNoSqlProviderGenerationResult DropNoSqlProviderGeneration(
    const EngineRequestContext& context,
    EngineNoSqlProviderFamily family,
    const std::string& provider_id,
    const std::string& collection_uuid) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  auto generations = LoadLocked(context);
  const auto before = generations.size();
  generations.erase(
      std::remove_if(generations.begin(),
                     generations.end(),
                     [&](const EngineNoSqlProviderGenerationMetadata& existing) {
                       return Matches(existing, family, provider_id, collection_uuid);
                     }),
      generations.end());
  if (generations.size() == before) {
    return Failure(context,
                   "nosql.provider_generation.drop",
                   kNoSqlProviderGenerationUnavailable);
  }
  if (!RewriteLocked(context, generations)) {
    return Failure(context,
                   "nosql.provider_generation.drop",
                   kNoSqlProviderGenerationUnavailable);
  }
  GenerationCache()[identity] = generations;

  EngineNoSqlProviderGenerationResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.evidence.push_back("provider_generation_drop=complete");
  result.evidence.push_back("provider_generation_persistent_state_removed=true");
  result.evidence.push_back("provider_generation_cache_stale=false");
  result.evidence.push_back("provider_generation_concurrency_guard=mutex");
  result.evidence.push_back("provider_generation_finality_authority=false");
  result.evidence.push_back("provider_generation_visibility_authority=false");
  return result;
}

std::vector<EngineNoSqlProviderGenerationMetadata> ListNoSqlProviderGenerations(
    const EngineRequestContext& context) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  return LoadLocked(context);
}

EngineNoSqlProviderGenerationResult CleanupNoSqlProviderGenerations(
    const EngineRequestContext& context,
    bool drop_persistent_state) {
  std::lock_guard<std::mutex> guard(StoreMutex());
  const auto identity = EngineNoSqlProviderDatabaseIdentity(context);
  GenerationCache().erase(identity);
  if (drop_persistent_state) {
    const auto path = GenerationPath(context);
    if (!path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
  EngineNoSqlProviderGenerationResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.evidence.push_back("provider_generation_cleanup=complete");
  result.evidence.push_back(std::string("provider_generation_drop_persistent_state=") +
                            BoolText(drop_persistent_state));
  result.evidence.push_back("provider_generation_concurrency_guard=mutex");
  return result;
}

void AddNoSqlProviderGenerationEvidence(
    EngineApiResult* result,
    const EngineNoSqlProviderGenerationResult& generation) {
  for (const auto& item : generation.evidence) {
    AddApiBehaviorEvidence(result, "nosql_provider_generation", item);
  }
}

}  // namespace scratchbird::engine::internal_api
