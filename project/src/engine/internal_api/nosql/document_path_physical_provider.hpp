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

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kDocumentPathPhysicalProviderId =
    "nosql.local.document.path_provider";
inline constexpr const char* kDocumentPathPhysicalProviderBadChecksum =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.BAD_CHECKSUM";
inline constexpr const char* kDocumentPathPhysicalProviderStaleFormat =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.STALE_FORMAT";
inline constexpr const char* kDocumentPathPhysicalProviderStaleGeneration =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.STALE_GENERATION";
inline constexpr const char* kDocumentPathPhysicalProviderIdentityMismatch =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.IDENTITY_MISMATCH";
inline constexpr const char* kDocumentPathPhysicalProviderMalformedPathDictionary =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.MALFORMED_PATH_DICTIONARY";
inline constexpr const char* kDocumentPathPhysicalProviderMalformedShapeDictionary =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.MALFORMED_SHAPE_DICTIONARY";
inline constexpr const char* kDocumentPathPhysicalProviderMalformedPostings =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.MALFORMED_POSTINGS";
inline constexpr const char* kDocumentPathPhysicalProviderUnsafePathToken =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.UNSAFE_PATH_TOKEN";
inline constexpr const char* kDocumentPathPhysicalProviderTruncatedPayload =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.TRUNCATED_PAYLOAD";
inline constexpr const char* kDocumentPathPhysicalProviderInvalidUuid =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.INVALID_UUID";
inline constexpr const char* kDocumentPathPhysicalProviderMissingGenerationEpoch =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.MISSING_GENERATION_EPOCH";
inline constexpr const char* kDocumentPathPhysicalProviderAuthorityClaimRefused =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED";
inline constexpr const char* kDocumentPathPhysicalProviderDescriptorScanRefused =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.DESCRIPTOR_OR_BEHAVIOR_SCAN_REFUSED";
inline constexpr const char* kDocumentPathPhysicalProviderRebuildRequired =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.REBUILD_REQUIRED";
inline constexpr const char* kDocumentPathPhysicalProviderRepairAdmissionRequired =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.REPAIR_ADMISSION_REQUIRED";
inline constexpr const char* kDocumentPathPhysicalProviderRepairSourceRequired =
    "SB_DOCUMENT_PATH_PHYSICAL_PROVIDER.REPAIR_SOURCE_REQUIRED";

struct DocumentPathProviderIdentity {
  std::string database_uuid;
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_id = kDocumentPathPhysicalProviderId;
  std::string segment_uuid;
  std::uint64_t provider_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
};

struct DocumentPathScalar {
  std::string scalar_type = "string";
  std::string encoded_value;
  bool is_null = false;
};

struct DocumentPathValueEvidence {
  std::string path;
  DocumentPathScalar value;
};

struct DocumentPathRowEvidence {
  std::string document_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::uint64_t row_ordinal = 0;
  std::vector<DocumentPathValueEvidence> values;
  bool authoritative_document_row_path_evidence = true;
  bool descriptor_scan_claim = false;
  bool behavior_scan_claim = false;
  bool summary_visibility_or_finality_claim = false;
  bool parser_finality_authority_claim = false;
  bool reference_finality_authority_claim = false;
  bool provider_finality_authority_claim = false;
  bool write_ahead_log_finality_authority_claim = false;  // wal-not-authority
};

struct DocumentPathProviderBuildRequest {
  std::string artifact_path;
  DocumentPathProviderIdentity identity;
  std::vector<DocumentPathRowEvidence> rows;
};

struct DocumentPathProviderAppendRequest {
  std::string artifact_path;
  DocumentPathProviderIdentity next_identity;
  DocumentPathRowEvidence row;
};

struct DocumentPathProviderMutationRequest {
  std::string artifact_path;
  bool admitted_authoritative_rebuild = false;
  std::vector<DocumentPathRowEvidence> authoritative_source_rows;
};

struct DocumentPathProviderOpenRequest {
  std::string artifact_path;
  DocumentPathProviderIdentity expected_identity;
  bool require_expected_identity = false;
  bool repair_admitted = false;
  std::vector<DocumentPathRowEvidence> authoritative_source_rows;
};

struct DocumentPathProviderPathEntry {
  std::uint64_t path_id = 0;
  std::string path_kind;
  std::string normalized_path;
};

struct DocumentPathProviderShapeEntry {
  std::uint64_t shape_id = 0;
  std::vector<std::uint64_t> path_ids;
};

struct DocumentPathProviderPosting {
  std::uint64_t path_id = 0;
  std::string scalar_type;
  std::string encoded_value;
  std::string document_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::uint64_t row_ordinal = 0;
  std::string concrete_path;
  std::int64_t array_position = -1;
};

struct DocumentPathProviderArrayExpansion {
  std::uint64_t wildcard_path_id = 0;
  std::uint64_t concrete_path_id = 0;
  std::string concrete_path;
  std::int64_t array_position = -1;
};

struct DocumentPathProviderStats {
  std::uint64_t row_count = 0;
  std::uint64_t path_count = 0;
  std::uint64_t shape_count = 0;
  std::uint64_t posting_count = 0;
  std::uint64_t wildcard_path_count = 0;
  std::uint64_t array_expansion_count = 0;
};

struct DocumentPathProviderArtifact {
  DocumentPathProviderIdentity identity;
  std::vector<DocumentPathProviderPathEntry> path_dictionary;
  std::vector<DocumentPathProviderShapeEntry> shape_dictionary;
  std::vector<DocumentPathProviderPosting> postings;
  std::vector<DocumentPathProviderArrayExpansion> array_expansions;
  DocumentPathProviderStats stats;
};

struct DocumentPathProviderResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  DocumentPathProviderArtifact artifact;
  std::vector<std::string> evidence;
};

struct DocumentPathProviderProbeRequest {
  std::string artifact_path;
  DocumentPathProviderIdentity expected_identity;
  bool require_expected_identity = true;
  std::string path;
  DocumentPathScalar equals_value;
  bool wildcard_path = false;
  std::vector<std::string> projected_paths;
  bool fetch_full_payload = false;
};

struct DocumentPathProviderProjectedValue {
  std::string path;
  DocumentPathScalar value;
};

struct DocumentPathProviderCandidate {
  std::string document_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::string shape_id;
  std::uint64_t shape_ref_count = 0;
  std::uint64_t row_ordinal = 0;
  std::vector<DocumentPathProviderProjectedValue> projected_values;
};

struct DocumentPathProviderProjectionPlan {
  bool fetch_candidate_rows_only = true;
  bool fetch_projected_paths_only = true;
  bool fetch_full_payload = false;
  std::vector<std::string> projected_paths;
  std::vector<DocumentPathProviderCandidate> candidates;
};

struct DocumentPathProviderProbeResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  DocumentPathProviderProjectionPlan projection_plan;
  DocumentPathProviderStats stats;
  std::uint64_t index_probes = 0;
  std::uint64_t path_keys_examined = 0;
  std::vector<std::string> evidence;
};

std::string DocumentPathPhysicalProviderPath(const EngineRequestContext& context);
DocumentPathProviderIdentity DocumentPathProviderIdentityForContext(
    const EngineRequestContext& context,
    std::uint64_t provider_generation,
    const std::string& index_uuid = {});
DocumentPathScalar DocumentPathScalarFromTypedValue(const EngineTypedValue& value);

DocumentPathProviderResult BuildDocumentPathPhysicalProvider(
    const DocumentPathProviderBuildRequest& request);
DocumentPathProviderResult AppendDocumentPathPhysicalProvider(
    const DocumentPathProviderAppendRequest& request);
DocumentPathProviderResult OpenDocumentPathPhysicalProvider(
    const DocumentPathProviderOpenRequest& request);
DocumentPathProviderResult DeleteOrUpdateDocumentPathPhysicalProvider(
    const DocumentPathProviderMutationRequest& request);
DocumentPathProviderProbeResult ProbeDocumentPathPhysicalProvider(
    const DocumentPathProviderProbeRequest& request);

bool DocumentPathProviderEvidenceContains(const std::vector<std::string>& evidence,
                                          const std::string& value);

}  // namespace scratchbird::engine::internal_api
