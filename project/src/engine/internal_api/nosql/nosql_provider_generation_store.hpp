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
