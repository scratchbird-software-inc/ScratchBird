// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "index_family_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kDonorEmulatedIndexMappingKey =
    "SB_DONOR_EMULATED_INDEX_MAPPING";

struct DonorEmulatedAuthorityClaims {
  bool parser_finality = false;
  bool donor_finality = false;
  bool provider_finality = false;
  bool index_finality = false;
  bool security = false;
  bool visibility = false;
  bool transaction_finality = false;
  bool recovery = false;
  bool log_finality = false;
  bool physical_truth = false;
};

struct DonorEmulatedIndexDeclaration {
  std::string donor_provider;
  std::string donor_profile;
  std::string donor_family;
  std::string catalog_identity;
  std::string declared_name;
  bool descriptor_scan_fallback_requested = false;
  bool behavior_scan_fallback_requested = false;
  DonorEmulatedAuthorityClaims authority_claims;
};

struct DonorEmulatedIndexAdmissionProof {
  bool exact_source_required = true;
  bool exact_source_available = false;
  bool mga_visibility_recheck_required = true;
  bool mga_visibility_recheck_available = false;
  bool security_recheck_required = true;
  bool security_recheck_available = false;
  std::string exact_source_token;
  std::string mga_visibility_token;
  std::string security_token;
};

struct DonorEmulatedIndexMetadataProjection {
  Status status;
  DiagnosticRecord diagnostic;
  std::string normalized_provider;
  std::string normalized_profile;
  std::string normalized_family;
  std::string semantic_profile;
  std::string catalog_identity;
  std::string declared_name;
  IndexFamily native_family = IndexFamily::unknown;
  std::string native_family_id;
  std::string native_candidate_path;
  bool management_visible = false;
  bool catalog_identity_only = true;
  bool executable = false;
  bool native_admission_required = true;
  bool donor_metadata_authority = false;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok(); }
};

struct DonorEmulatedIndexAdmissionResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool admitted = false;
  bool fail_closed = false;
  std::string normalized_provider;
  std::string normalized_profile;
  std::string normalized_family;
  std::string semantic_profile;
  IndexFamily native_family = IndexFamily::unknown;
  std::string native_family_id;
  std::string native_candidate_path;
  bool candidate_only = true;
  bool exact_source_required = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool donor_metadata_authority = false;
  bool physical_truth_authority = false;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

DonorEmulatedIndexMetadataProjection ProjectDonorEmulatedIndexMetadata(
    const DonorEmulatedIndexDeclaration& declaration);

DonorEmulatedIndexAdmissionResult AdmitDonorEmulatedIndexMapping(
    const DonorEmulatedIndexDeclaration& declaration,
    const DonorEmulatedIndexAdmissionProof& proof);

DiagnosticRecord MakeDonorEmulatedIndexMappingDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
