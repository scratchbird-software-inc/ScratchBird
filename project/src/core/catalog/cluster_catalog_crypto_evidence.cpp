// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_catalog_crypto_evidence.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CryptoOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CryptoErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::catalog};
}

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsHex(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

bool HasLowerHexSuffix(std::string_view value,
                       std::string_view prefix,
                       std::size_t hex_digits) {
  if (!StartsWith(value, prefix) || value.size() != prefix.size() + hex_digits) {
    return false;
  }
  for (std::size_t i = prefix.size(); i < value.size(); ++i) {
    if (!IsHex(value[i])) { return false; }
  }
  return true;
}

bool WeakAlgorithm(std::string value) {
  value = Lower(std::move(value));
  return value == "fnv1a64" || value == "fnv" || value == "crc32" ||
         value == "crc64" || value == "adler32" || value == "xxhash" ||
         value == "checksum" || value == "none";
}

bool IsSha256Digest(std::string_view value) {
  return HasLowerHexSuffix(value, "sha256:", 64);
}

bool IsHmacSha256Digest(std::string_view value) {
  return HasLowerHexSuffix(value, "hmac-sha256:", 64);
}

ClusterCatalogCryptoEvidenceValidationResult CryptoError(
    const ClusterCatalogCryptoEvidenceMetadata& metadata,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {},
    bool weak_evidence_rejected = false) {
  ClusterCatalogCryptoEvidenceValidationResult result;
  result.status = CryptoErrorStatus();
  result.metadata = metadata;
  result.weak_evidence_rejected = weak_evidence_rejected;
  result.signature_ready_metadata = metadata.signature_ready;
  result.provider_authority_claim_allowed = false;
  result.diagnostic = MakeClusterCatalogCryptoEvidenceDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  result.evidence.push_back("CLUSTER_CATALOG_CRYPTO_EVIDENCE");
  result.evidence.push_back("cluster_catalog.crypto_integrity.fail_closed=true");
  result.evidence.push_back("cluster_catalog.crypto_integrity.weak_rejected=" +
                            std::string(weak_evidence_rejected ? "true" : "false"));
  result.evidence.push_back("cluster_catalog.crypto_integrity.algorithm=" +
                            metadata.algorithm);
  return result;
}

}  // namespace

const char* ClusterCatalogEvidenceKindName(ClusterCatalogEvidenceKind kind) {
  switch (kind) {
    case ClusterCatalogEvidenceKind::decision:
      return "decision";
    case ClusterCatalogEvidenceKind::fence:
      return "fence";
    case ClusterCatalogEvidenceKind::route:
      return "route";
    case ClusterCatalogEvidenceKind::topology:
      return "topology";
    case ClusterCatalogEvidenceKind::projection:
      return "projection";
    case ClusterCatalogEvidenceKind::descriptor:
      return "descriptor";
  }
  return "unknown";
}

std::string CanonicalSerializeClusterCatalogEvidenceSubject(
    const ClusterCatalogEvidenceSubject& subject) {
  std::vector<std::pair<std::string, std::string>> fields = subject.fields;
  std::sort(fields.begin(), fields.end());

  std::ostringstream out;
  out << "cluster_catalog_evidence.v1\n";
  out << "kind=" << ClusterCatalogEvidenceKindName(subject.kind) << '\n';
  out << "subject_id=" << subject.subject_id << '\n';
  out << "table_path=" << subject.table_path << '\n';
  out << "catalog_epoch=" << subject.catalog_epoch << '\n';
  out << "catalog_generation=" << subject.catalog_generation << '\n';
  out << "fields=" << fields.size() << '\n';
  for (const auto& field : fields) {
    out << "field." << field.first << '=' << field.second << '\n';
  }
  return out.str();
}

// SEARCH_KEY: CLUSTER_CATALOG_CRYPTO_EVIDENCE
ClusterCatalogCryptoEvidenceValidationResult
ValidateClusterCatalogCryptoEvidenceMetadata(
    const ClusterCatalogCryptoEvidenceMetadata& metadata) {
  if (metadata.subject.subject_id.empty() ||
      metadata.subject.table_path.empty() ||
      metadata.subject.catalog_epoch == 0 ||
      metadata.subject.catalog_generation == 0) {
    return CryptoError(metadata,
                       "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-SUBJECT-REQUIRED",
                       "catalog.cluster_crypto_evidence.subject_required",
                       metadata.subject.subject_id);
  }

  const std::string canonical =
      CanonicalSerializeClusterCatalogEvidenceSubject(metadata.subject);
  if (metadata.canonical_payload != canonical) {
    return CryptoError(metadata,
                       "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-CANONICAL-MISMATCH",
                       "catalog.cluster_crypto_evidence.canonical_mismatch",
                       metadata.subject.subject_id);
  }

  const std::string algorithm = Lower(metadata.algorithm);
  if (WeakAlgorithm(algorithm)) {
    return CryptoError(metadata,
                       "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-WEAK",
                       "catalog.cluster_crypto_evidence.weak_algorithm",
                       metadata.algorithm,
                       true);
  }

  bool algorithm_valid = false;
  bool signature_ready = false;
  if (algorithm == "sha256") {
    algorithm_valid = IsSha256Digest(metadata.digest);
  } else if (algorithm == "hmac-sha256") {
    algorithm_valid = IsHmacSha256Digest(metadata.digest) &&
                      !metadata.hmac_key_id.empty();
  } else if (algorithm == "signature-ready-ed25519" ||
             algorithm == "signature-ready-rsa-pss-sha256") {
    signature_ready = metadata.signature_ready &&
                      !metadata.signature_key_id.empty() &&
                      !metadata.signature_envelope_id.empty();
    algorithm_valid = signature_ready && IsSha256Digest(metadata.digest);
  }
  if (!algorithm_valid) {
    return CryptoError(metadata,
                       "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-ALGORITHM-REQUIRED",
                       "catalog.cluster_crypto_evidence.algorithm_required",
                       metadata.algorithm);
  }

  ClusterCatalogCryptoEvidenceValidationResult result;
  result.status = CryptoOkStatus();
  result.metadata = metadata;
  result.crypto_integrity_required = true;
  result.weak_evidence_rejected = false;
  result.signature_ready_metadata = signature_ready;
  result.provider_authority_claim_allowed =
      metadata.provider_authority_claim && algorithm_valid;
  result.evidence.push_back("CLUSTER_CATALOG_CRYPTO_EVIDENCE");
  result.evidence.push_back("cluster_catalog.crypto_integrity.fail_closed=false");
  result.evidence.push_back("cluster_catalog.crypto_integrity.algorithm=" +
                            metadata.algorithm);
  result.evidence.push_back("cluster_catalog.crypto_integrity.canonical_payload=true");
  result.evidence.push_back("cluster_catalog.crypto_integrity.provider_authority_claim_allowed=" +
                            std::string(result.provider_authority_claim_allowed ? "true" : "false"));
  return result;
}

DiagnosticRecord MakeClusterCatalogCryptoEvidenceDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.catalog.cluster_catalog_crypto_evidence");
}

}  // namespace scratchbird::core::catalog
