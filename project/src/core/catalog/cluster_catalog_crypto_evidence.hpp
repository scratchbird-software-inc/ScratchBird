// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::catalog {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// SEARCH_KEY: CLUSTER_CATALOG_CRYPTO_EVIDENCE
// Cluster catalog authority evidence must bind canonical serialized payloads to
// cryptographic integrity metadata. Non-cryptographic checksums are rejected for
// decision, fence, route, topology, projection, and descriptor authority claims.
enum class ClusterCatalogEvidenceKind {
  decision,
  fence,
  route,
  topology,
  projection,
  descriptor,
};

struct ClusterCatalogEvidenceSubject {
  ClusterCatalogEvidenceKind kind = ClusterCatalogEvidenceKind::decision;
  std::string subject_id;
  std::string table_path;
  u64 catalog_epoch = 0;
  u64 catalog_generation = 0;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct ClusterCatalogCryptoEvidenceMetadata {
  ClusterCatalogEvidenceSubject subject;
  std::string canonical_payload;
  std::string algorithm;
  std::string digest;
  std::string hmac_key_id;
  std::string signature_key_id;
  std::string signature_envelope_id;
  bool signature_ready = false;
  bool provider_authority_claim = false;
};

struct ClusterCatalogCryptoEvidenceValidationResult {
  Status status;
  ClusterCatalogCryptoEvidenceMetadata metadata;
  DiagnosticRecord diagnostic;
  bool crypto_integrity_required = true;
  bool weak_evidence_rejected = false;
  bool signature_ready_metadata = false;
  bool provider_authority_claim_allowed = false;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

const char* ClusterCatalogEvidenceKindName(ClusterCatalogEvidenceKind kind);
std::string CanonicalSerializeClusterCatalogEvidenceSubject(
    const ClusterCatalogEvidenceSubject& subject);
ClusterCatalogCryptoEvidenceValidationResult
ValidateClusterCatalogCryptoEvidenceMetadata(
    const ClusterCatalogCryptoEvidenceMetadata& metadata);
DiagnosticRecord MakeClusterCatalogCryptoEvidenceDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::catalog
