// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "compression_policy.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kNoSqlProviderLocalProviderMissing =
    "SB_NOSQL_PROVIDER_LOCAL_PROVIDER_MISSING";
inline constexpr const char* kNoSqlProviderFamilyUnsupported =
    "SB_NOSQL_PROVIDER_FAMILY_UNSUPPORTED";
inline constexpr const char* kNoSqlProviderClusterScopeRefusedLocalOnly =
    "SB_NOSQL_PROVIDER_CLUSTER_SCOPE_REFUSED_LOCAL_ONLY";
inline constexpr const char* kNoSqlProviderDistributedScopeRefusedLocalOnly =
    "SB_NOSQL_PROVIDER_DISTRIBUTED_SCOPE_REFUSED_LOCAL_ONLY";
inline constexpr const char* kNoSqlProviderDescriptorVisibilityProofMissing =
    "SB_NOSQL_PROVIDER_DESCRIPTOR_VISIBILITY_PROOF_MISSING";
inline constexpr const char* kNoSqlProviderDescriptorNotVisibleToSnapshot =
    "SB_NOSQL_PROVIDER_DESCRIPTOR_NOT_VISIBLE_TO_SNAPSHOT";
inline constexpr const char* kNoSqlProviderDescriptorCompatibilityMissing =
    "SB_NOSQL_PROVIDER_DESCRIPTOR_COMPATIBILITY_MISSING";
inline constexpr const char* kNoSqlProviderSecurityProofMissing =
    "SB_NOSQL_PROVIDER_SECURITY_PROOF_MISSING";
inline constexpr const char* kNoSqlProviderSecuritySnapshotProofMissing =
    "SB_NOSQL_PROVIDER_SECURITY_SNAPSHOT_PROOF_MISSING";
inline constexpr const char* kNoSqlProviderIndexGenerationProofMissing =
    "SB_NOSQL_PROVIDER_INDEX_GENERATION_PROOF_MISSING";
inline constexpr const char* kNoSqlProviderIndexGenerationStale =
    "SB_NOSQL_PROVIDER_INDEX_GENERATION_STALE";
inline constexpr const char* kNoSqlProviderIndexGenerationNotVisible =
    "SB_NOSQL_PROVIDER_INDEX_GENERATION_NOT_VISIBLE";
inline constexpr const char* kNoSqlProviderIndexPredicateCoverageMissing =
    "SB_NOSQL_PROVIDER_INDEX_PREDICATE_COVERAGE_MISSING";
inline constexpr const char* kNoSqlProviderDeltaOverlayProofMissing =
    "SB_NOSQL_PROVIDER_DELTA_OVERLAY_PROOF_MISSING";
inline constexpr const char* kNoSqlProviderDeltaOverlaySnapshotCoverageMissing =
    "SB_NOSQL_PROVIDER_DELTA_OVERLAY_SNAPSHOT_COVERAGE_MISSING";
inline constexpr const char* kNoSqlProviderPolicyProofMissing =
    "SB_NOSQL_PROVIDER_POLICY_PROOF_MISSING";
inline constexpr const char* kNoSqlProviderPolicyRefused =
    "SB_NOSQL_PROVIDER_POLICY_REFUSED";
inline constexpr const char* kNoSqlProviderMgaRecheckProofMissing =
    "SB_NOSQL_PROVIDER_MGA_RECHECK_PROOF_MISSING";
inline constexpr const char* kNoSqlProviderRowMgaRecheckRequired =
    "SB_NOSQL_PROVIDER_ROW_MGA_RECHECK_REQUIRED";
inline constexpr const char* kNoSqlProviderSecurityRecheckRequired =
    "SB_NOSQL_PROVIDER_SECURITY_RECHECK_REQUIRED";
inline constexpr const char* kNoSqlProviderFinalityAuthorityRefused =
    "SB_NOSQL_PROVIDER_FINALITY_AUTHORITY_REFUSED";
inline constexpr const char* kNoSqlProviderVisibilityAuthorityRefused =
    "SB_NOSQL_PROVIDER_VISIBILITY_AUTHORITY_REFUSED";
inline constexpr const char* kNoSqlProviderParserFinalityAuthorityRefused =
    "SB_NOSQL_PROVIDER_PARSER_FINALITY_AUTHORITY_REFUSED";
inline constexpr const char* kNoSqlProviderWriteAheadFinalityAuthorityRefused =  // wal-not-authority
    "SB_NOSQL_PROVIDER_WRITE_AHEAD_FINALITY_AUTHORITY_REFUSED";  // wal-not-authority
inline constexpr const char* kNoSqlProviderDescriptorScanNotPhysicalProvider =
    "SB_NOSQL_PROVIDER_DESCRIPTOR_SCAN_NOT_PHYSICAL_PROVIDER";
inline constexpr const char* kNoSqlProviderBehaviorScanNotPhysicalProvider =
    "SB_NOSQL_PROVIDER_BEHAVIOR_SCAN_NOT_PHYSICAL_PROVIDER";
inline constexpr const char* kNoSqlProviderGenerationProofMissing =
    "SB_NOSQL_PROVIDER_GENERATION.MISSING";
inline constexpr const char* kNoSqlProviderGenerationUnavailable =
    "SB_NOSQL_PROVIDER_GENERATION.UNAVAILABLE";
inline constexpr const char* kNoSqlProviderGenerationStale =
    "SB_NOSQL_PROVIDER_GENERATION.STALE";
inline constexpr const char* kNoSqlProviderGenerationEpochMismatch =
    "SB_NOSQL_PROVIDER_GENERATION.EPOCH_MISMATCH";
inline constexpr const char* kNoSqlProviderGenerationStateUnvalidated =
    "SB_NOSQL_PROVIDER_GENERATION.STATE_UNVALIDATED";
inline constexpr const char* kNoSqlProviderGenerationMetadataMissing =
    "SB_NOSQL_PROVIDER_GENERATION.METADATA_MISSING";
inline constexpr const char* kNoSqlProviderGenerationAuthorityRefused =
    "SB_NOSQL_PROVIDER_GENERATION.AUTHORITY_REFUSED";

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_PHYSICAL_PROVIDER_CONTRACT
enum class EngineNoSqlProviderFamily {
  kUnknown,
  kKeyValue,
  kDocument,
  kSearch,
  kVector,
  kGraph,
  kTimeSeries,
  kSpatial,
  kColumnar,
};

enum class EngineNoSqlProviderScope {
  kLocal,
  kClusterOnly,
  kDistributed,
};

struct EngineNoSqlDescriptorVisibilityProof {
  bool proof_present = false;
  bool visible_to_snapshot = false;
  bool descriptor_shape_compatible = false;
  bool descriptor_scan_selected = false;
  bool behavior_store_scan_selected = false;
  std::uint64_t descriptor_generation = 0;
  std::string proof_id;
};

struct EngineNoSqlSecurityRedactionProof {
  bool proof_present = false;
  bool redaction_policy_bound = false;
  bool security_snapshot_bound = false;
  std::string redaction_profile = "unverified";
  std::string proof_id;
};

struct EngineNoSqlIndexGenerationProof {
  bool proof_present = false;
  bool visible_to_snapshot = false;
  bool covers_predicate = false;
  std::uint64_t required_generation = 0;
  std::uint64_t available_generation = 0;
  std::string index_uuid;
  std::string proof_id;
};

struct EngineNoSqlDeltaOverlayProof {
  bool required = false;
  bool proof_present = false;
  bool covers_snapshot = false;
  std::uint64_t overlay_generation = 0;
  std::string proof_id;
};

struct EngineNoSqlPolicyProof {
  bool proof_present = false;
  bool allowed = false;
  std::string policy_snapshot_uuid;
  std::vector<std::string> refusal_reasons;
};

struct EngineNoSqlProviderGenerationProof {
  bool required = false;
  bool proof_present = false;
  bool visible_to_snapshot = false;
  bool publish_state_bound = false;
  bool validation_state_bound = false;
  bool backup_restore_repair_metadata_bound = false;
  bool support_bundle_evidence_bound = false;
  std::uint64_t required_generation = 0;
  std::uint64_t available_generation = 0;
  std::uint64_t descriptor_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::string generation_uuid;
  std::string provider_id;
  std::string database_uuid;
  std::string collection_uuid;
  std::string publish_state = "unverified";
  std::string validation_state = "unverified";
  std::string backup_metadata_ref;
  std::string restore_metadata_ref;
  std::string repair_metadata_ref;
  std::string support_bundle_evidence_id;
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
};

struct EngineNoSqlMgaRecheckProof {
  bool proof_present = false;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
  bool index_claims_transaction_finality_authority = false;
  bool delta_overlay_claims_transaction_finality_authority = false;
  bool parser_claims_transaction_finality_authority = false;
  bool write_ahead_log_claims_transaction_finality_authority = false;  // wal-not-authority
  std::string authority_source = "engine_transaction_inventory";
};

struct EngineNoSqlPhysicalProviderContract {
  EngineNoSqlProviderFamily family = EngineNoSqlProviderFamily::kUnknown;
  EngineNoSqlProviderScope scope = EngineNoSqlProviderScope::kLocal;
  std::string provider_id = "nosql.local.provider";
  std::string fallback_provider_id;
  bool local_provider_available = false;
  bool exact_fallback_available = false;
  std::uint64_t estimated_rows = 1000;
  EngineNoSqlDescriptorVisibilityProof descriptor_visibility;
  EngineNoSqlSecurityRedactionProof security_redaction;
  EngineNoSqlIndexGenerationProof index_generation;
  EngineNoSqlDeltaOverlayProof delta_overlay;
  EngineNoSqlPolicyProof policy;
  EngineNoSqlProviderGenerationProof provider_generation;
  EngineNoSqlMgaRecheckProof mga_recheck;
};

struct EngineNoSqlPhysicalProviderSelection {
  bool ok = false;
  bool selected = false;
  bool fail_closed = true;
  EngineNoSqlProviderFamily family = EngineNoSqlProviderFamily::kUnknown;
  EngineNoSqlProviderScope scope = EngineNoSqlProviderScope::kLocal;
  std::string selected_provider_id;
  std::string fallback_provider_id;
  std::uint64_t estimated_rows = 0;
  std::vector<std::string> required_facts;
  std::vector<std::string> missing_diagnostics;
  std::vector<std::string> refusal_diagnostics;
  std::vector<std::string> evidence;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool index_transaction_finality_authority = false;
  bool delta_overlay_transaction_finality_authority = false;
  bool parser_transaction_finality_authority = false;
  bool write_ahead_log_transaction_finality_authority = false;  // wal-not-authority
  bool descriptor_scan_selected = false;
  bool behavior_store_scan_selected = false;
};

const char* EngineNoSqlProviderFamilyName(EngineNoSqlProviderFamily family);
EngineNoSqlProviderFamily EngineNoSqlProviderFamilyFromString(std::string_view family);
const char* EngineNoSqlProviderScopeName(EngineNoSqlProviderScope scope);
scratchbird::core::index::CompressionFamily EngineNoSqlProviderCompressionFamily(
    EngineNoSqlProviderFamily family);
const char* EngineNoSqlProviderCompressionPolicyFamilyName(
    EngineNoSqlProviderFamily family);
std::vector<std::string> EngineNoSqlProviderCompressionPolicyEvidence(
    EngineNoSqlProviderFamily family);

EngineNoSqlPhysicalProviderSelection SelectLocalNoSqlPhysicalProvider(
    const EngineNoSqlPhysicalProviderContract& contract);

bool EngineNoSqlSelectionHasDiagnostic(
    const EngineNoSqlPhysicalProviderSelection& selection,
    std::string_view diagnostic_code);

}  // namespace scratchbird::engine::internal_api
