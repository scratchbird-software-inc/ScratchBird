// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cluster_catalog_crypto_evidence.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_CRYPTO_POLICY
// PUBLIC_CRYPTO_POLICY
// Centralized security cryptographic primitives for authentication and
// security-event integrity. OpenSSL supplies the approved SHA-256/HMAC-SHA-256
// algorithms; equality checks stay constant-time with respect to input length.
bool SecurityConstantTimeEqual(std::string_view lhs, std::string_view rhs);
std::string SecuritySha256Hex(std::string_view payload);
std::string SecurityHmacSha256Hex(std::string_view key, std::string_view payload);

// SEARCH_KEY: CLUSTER_EVIDENCE_INTEGRITY
// Engine security validates cluster catalog evidence integrity using approved
// cryptographic primitives or signature-ready metadata. Weak checksums cannot
// support catalog authority claims.
enum class ClusterEvidenceIntegrityProtection {
  sha256,
  hmac_sha256,
  signature_ready_ed25519,
};

struct ClusterEvidenceIntegrityRequest {
  scratchbird::core::catalog::ClusterCatalogEvidenceSubject subject;
  ClusterEvidenceIntegrityProtection protection =
      ClusterEvidenceIntegrityProtection::sha256;
  std::string hmac_key;
  std::string hmac_key_id;
  std::string signature_key_id;
  std::string signature_envelope_id;
  bool provider_authority_claim = false;
  std::string presented_algorithm;
  std::string presented_digest;
};

struct ClusterEvidenceIntegrityResult {
  bool ok = false;
  bool fail_closed = true;
  bool weak_evidence_rejected = false;
  bool signature_ready_metadata = false;
  bool provider_authority_claim_allowed = false;
  std::string diagnostic_code;
  std::string detail;
  scratchbird::core::catalog::ClusterCatalogCryptoEvidenceMetadata metadata;
  std::vector<std::string> evidence;
};

const char* ClusterEvidenceIntegrityProtectionName(
    ClusterEvidenceIntegrityProtection protection);

ClusterEvidenceIntegrityResult EvaluateClusterEvidenceIntegrity(
    const ClusterEvidenceIntegrityRequest& request);

}  // namespace scratchbird::engine::internal_api
