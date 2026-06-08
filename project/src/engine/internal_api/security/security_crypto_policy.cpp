// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/security_crypto_policy.hpp"

#include "hash_digest.hpp"

#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace core_hash = scratchbird::core::hash;
using scratchbird::core::platform::byte;

std::vector<byte> Bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

}  // namespace

bool SecurityConstantTimeEqual(std::string_view lhs, std::string_view rhs) {
  return core_hash::ConstantTimeEqual(lhs, rhs);
}

std::string SecuritySha256Hex(std::string_view payload) {
  const auto digest = core_hash::ComputeSha256Digest(Bytes(payload));
  if (!digest.ok()) {
    return {};
  }
  return core_hash::HexLower(digest.digest);
}

std::string SecurityHmacSha256Hex(std::string_view key, std::string_view payload) {
  const auto key_bytes = Bytes(key);
  const auto payload_bytes = Bytes(payload);
  const auto digest = core_hash::ComputeHmacSha256Digest(key_bytes,
                                                        payload_bytes);
  if (!digest.ok()) {
    return {};
  }
  return core_hash::HexLower(digest.digest);
}

const char* ClusterEvidenceIntegrityProtectionName(
    ClusterEvidenceIntegrityProtection protection) {
  switch (protection) {
    case ClusterEvidenceIntegrityProtection::sha256:
      return "sha256";
    case ClusterEvidenceIntegrityProtection::hmac_sha256:
      return "hmac-sha256";
    case ClusterEvidenceIntegrityProtection::signature_ready_ed25519:
      return "signature-ready-ed25519";
  }
  return "unknown";
}

namespace {

namespace catalog = scratchbird::core::catalog;

void AddClusterEvidenceIntegrity(ClusterEvidenceIntegrityResult* result,
                                 std::string evidence) {
  if (result != nullptr) { result->evidence.push_back(std::move(evidence)); }
}

ClusterEvidenceIntegrityResult ClusterEvidenceIntegrityFailure(
    const catalog::ClusterCatalogCryptoEvidenceMetadata& metadata,
    std::string code,
    std::string detail,
    bool weak_evidence_rejected = false) {
  ClusterEvidenceIntegrityResult result;
  result.ok = false;
  result.fail_closed = true;
  result.weak_evidence_rejected = weak_evidence_rejected;
  result.signature_ready_metadata = metadata.signature_ready;
  result.provider_authority_claim_allowed = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  result.metadata = metadata;
  AddClusterEvidenceIntegrity(&result, "CLUSTER_EVIDENCE_INTEGRITY");
  AddClusterEvidenceIntegrity(&result,
                              "cluster_evidence_integrity.fail_closed=true");
  AddClusterEvidenceIntegrity(&result,
                              "cluster_evidence_integrity.weak_rejected=" +
                                  std::string(weak_evidence_rejected ? "true"
                                                                     : "false"));
  AddClusterEvidenceIntegrity(&result,
                              "cluster_evidence_integrity.algorithm=" +
                                  metadata.algorithm);
  AddClusterEvidenceIntegrity(&result,
                              "cluster_evidence_integrity.diagnostic_code=" +
                                  result.diagnostic_code);
  return result;
}

}  // namespace

// SEARCH_KEY: CLUSTER_EVIDENCE_INTEGRITY
ClusterEvidenceIntegrityResult EvaluateClusterEvidenceIntegrity(
    const ClusterEvidenceIntegrityRequest& request) {
  const std::string canonical =
      catalog::CanonicalSerializeClusterCatalogEvidenceSubject(
          request.subject);
  const std::string expected_algorithm =
      ClusterEvidenceIntegrityProtectionName(request.protection);
  std::string expected_digest;
  if (request.protection == ClusterEvidenceIntegrityProtection::hmac_sha256) {
    if (request.hmac_key.empty() || request.hmac_key_id.empty()) {
      catalog::ClusterCatalogCryptoEvidenceMetadata metadata;
      metadata.subject = request.subject;
      metadata.canonical_payload = canonical;
      metadata.algorithm = expected_algorithm;
      metadata.hmac_key_id = request.hmac_key_id;
      metadata.provider_authority_claim = request.provider_authority_claim;
      return ClusterEvidenceIntegrityFailure(
          metadata,
          "SB_CLUSTER_EVIDENCE_INTEGRITY.HMAC_KEY_REQUIRED",
          "hmac_key_and_key_id_required");
    }
    expected_digest = "hmac-sha256:" +
                      SecurityHmacSha256Hex(request.hmac_key, canonical);
  } else {
    expected_digest = "sha256:" + SecuritySha256Hex(canonical);
  }

  catalog::ClusterCatalogCryptoEvidenceMetadata metadata;
  metadata.subject = request.subject;
  metadata.canonical_payload = canonical;
  metadata.algorithm = request.presented_algorithm.empty()
                           ? expected_algorithm
                           : request.presented_algorithm;
  metadata.digest = request.presented_digest.empty()
                        ? expected_digest
                        : request.presented_digest;
  metadata.hmac_key_id = request.hmac_key_id;
  metadata.signature_key_id = request.signature_key_id;
  metadata.signature_envelope_id = request.signature_envelope_id;
  metadata.signature_ready =
      request.protection ==
      ClusterEvidenceIntegrityProtection::signature_ready_ed25519;
  metadata.provider_authority_claim = request.provider_authority_claim;

  const auto validation =
      catalog::ValidateClusterCatalogCryptoEvidenceMetadata(metadata);
  if (!validation.ok()) {
    return ClusterEvidenceIntegrityFailure(
        metadata,
        validation.diagnostic.diagnostic_code,
        validation.diagnostic.message_key,
        validation.weak_evidence_rejected);
  }
  if (!request.presented_digest.empty() &&
      !SecurityConstantTimeEqual(expected_digest, metadata.digest)) {
    return ClusterEvidenceIntegrityFailure(
        metadata,
        "SB_CLUSTER_EVIDENCE_INTEGRITY.DIGEST_MISMATCH",
        "presented_digest_does_not_match_canonical_payload");
  }

  ClusterEvidenceIntegrityResult result;
  result.ok = true;
  result.fail_closed = false;
  result.weak_evidence_rejected = false;
  result.signature_ready_metadata = validation.signature_ready_metadata;
  result.provider_authority_claim_allowed =
      validation.provider_authority_claim_allowed;
  result.diagnostic_code = "SB_CLUSTER_EVIDENCE_INTEGRITY.OK";
  result.detail = "cluster evidence integrity accepted";
  result.metadata = metadata;
  result.evidence = validation.evidence;
  AddClusterEvidenceIntegrity(&result, "CLUSTER_EVIDENCE_INTEGRITY");
  AddClusterEvidenceIntegrity(&result,
                              "cluster_evidence_integrity.fail_closed=false");
  AddClusterEvidenceIntegrity(&result,
                              "cluster_evidence_integrity.algorithm=" +
                                  metadata.algorithm);
  AddClusterEvidenceIntegrity(&result,
                              "cluster_evidence_integrity.provider_authority_claim_allowed=" +
                                  std::string(result.provider_authority_claim_allowed
                                                  ? "true"
                                                  : "false"));
  return result;
}

}  // namespace scratchbird::engine::internal_api
