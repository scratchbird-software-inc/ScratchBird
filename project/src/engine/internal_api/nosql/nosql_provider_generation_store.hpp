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
