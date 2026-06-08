// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_CATALOG_CRYPTO_EVIDENCE_GATE

#include "cluster_catalog_crypto_evidence.hpp"
#include "security/security_crypto_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace catalog = scratchbird::core::catalog;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view expected) {
  return std::find(evidence.begin(), evidence.end(), expected) !=
         evidence.end();
}

std::vector<catalog::ClusterCatalogEvidenceKind> EvidenceKinds() {
  return {catalog::ClusterCatalogEvidenceKind::decision,
          catalog::ClusterCatalogEvidenceKind::fence,
          catalog::ClusterCatalogEvidenceKind::route,
          catalog::ClusterCatalogEvidenceKind::topology,
          catalog::ClusterCatalogEvidenceKind::projection,
          catalog::ClusterCatalogEvidenceKind::descriptor};
}

catalog::ClusterCatalogEvidenceSubject Subject(
    catalog::ClusterCatalogEvidenceKind kind) {
  catalog::ClusterCatalogEvidenceSubject subject;
  subject.kind = kind;
  subject.subject_id =
      "cluster-evidence:" +
      std::string(catalog::ClusterCatalogEvidenceKindName(kind));
  subject.table_path =
      "cluster.sys.catalog." +
      std::string(catalog::ClusterCatalogEvidenceKindName(kind));
  subject.catalog_epoch = 108;
  subject.catalog_generation = 10801;
  subject.fields.push_back({"zeta_generation", "10801"});
  subject.fields.push_back({"alpha_subject", subject.subject_id});
  subject.fields.push_back({"provider_record_digest",
                            "sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"});
  return subject;
}

void TestCanonicalSerializationIsStable() {
  auto first = Subject(catalog::ClusterCatalogEvidenceKind::route);
  auto second = first;
  std::reverse(second.fields.begin(), second.fields.end());

  const auto first_payload =
      catalog::CanonicalSerializeClusterCatalogEvidenceSubject(first);
  const auto second_payload =
      catalog::CanonicalSerializeClusterCatalogEvidenceSubject(second);
  Require(first_payload == second_payload,
          "cluster evidence canonical serialization changed with field order");
  Require(first_payload.find("field.alpha_subject=") <
              first_payload.find("field.zeta_generation="),
          "cluster evidence canonical fields were not sorted");
}

void TestSha256IntegrityForRequiredEvidenceKinds() {
  for (const auto kind : EvidenceKinds()) {
    api::ClusterEvidenceIntegrityRequest request;
    request.subject = Subject(kind);
    request.protection = api::ClusterEvidenceIntegrityProtection::sha256;
    request.provider_authority_claim = true;

    const auto result = api::EvaluateClusterEvidenceIntegrity(request);
    Require(result.ok, "SHA-256 cluster evidence integrity was refused");
    Require(!result.fail_closed, "SHA-256 cluster evidence failed closed");
    Require(result.provider_authority_claim_allowed,
            "SHA-256 provider authority claim was not allowed");
    Require(result.metadata.algorithm == "sha256",
            "SHA-256 metadata algorithm changed");
    Require(StartsWith(result.metadata.digest, "sha256:") &&
                result.metadata.digest.size() == 71,
            "SHA-256 metadata digest shape changed");
    Require(HasEvidence(result.evidence, "CLUSTER_CATALOG_CRYPTO_EVIDENCE"),
            "catalog crypto evidence key missing");
    Require(HasEvidence(result.evidence, "CLUSTER_EVIDENCE_INTEGRITY"),
            "security integrity evidence key missing");

    const auto validation =
        catalog::ValidateClusterCatalogCryptoEvidenceMetadata(result.metadata);
    Require(validation.ok(), "catalog rejected generated SHA-256 metadata");
  }
}

void TestHmacAndSignatureReadyIntegrity() {
  api::ClusterEvidenceIntegrityRequest hmac;
  hmac.subject = Subject(catalog::ClusterCatalogEvidenceKind::fence);
  hmac.protection = api::ClusterEvidenceIntegrityProtection::hmac_sha256;
  hmac.hmac_key = "pcr108-cluster-evidence-hmac-key";
  hmac.hmac_key_id = "hmac-key:pcr108";
  hmac.provider_authority_claim = true;
  auto result = api::EvaluateClusterEvidenceIntegrity(hmac);
  Require(result.ok, "HMAC cluster evidence integrity was refused");
  Require(result.metadata.algorithm == "hmac-sha256",
          "HMAC metadata algorithm changed");
  Require(StartsWith(result.metadata.digest, "hmac-sha256:") &&
              result.metadata.digest.size() == 76,
          "HMAC metadata digest shape changed");
  Require(result.metadata.hmac_key_id == hmac.hmac_key_id,
          "HMAC key id was not retained");

  api::ClusterEvidenceIntegrityRequest signature_ready;
  signature_ready.subject =
      Subject(catalog::ClusterCatalogEvidenceKind::descriptor);
  signature_ready.protection =
      api::ClusterEvidenceIntegrityProtection::signature_ready_ed25519;
  signature_ready.signature_key_id = "sig-key:pcr108";
  signature_ready.signature_envelope_id = "sig-envelope:pcr108";
  signature_ready.provider_authority_claim = true;
  result = api::EvaluateClusterEvidenceIntegrity(signature_ready);
  Require(result.ok, "signature-ready cluster evidence was refused");
  Require(result.signature_ready_metadata,
          "signature-ready metadata flag was not set");
  Require(result.metadata.algorithm == "signature-ready-ed25519",
          "signature-ready algorithm changed");
  Require(StartsWith(result.metadata.digest, "sha256:"),
          "signature-ready metadata lost canonical payload digest");
}

void TestWeakAndMismatchedEvidenceRejected() {
  catalog::ClusterCatalogCryptoEvidenceMetadata weak;
  weak.subject = Subject(catalog::ClusterCatalogEvidenceKind::decision);
  weak.canonical_payload =
      catalog::CanonicalSerializeClusterCatalogEvidenceSubject(weak.subject);
  weak.algorithm = "fnv1a64";
  weak.digest = "fnv1a64:deadbeef";
  weak.provider_authority_claim = true;
  auto validation =
      catalog::ValidateClusterCatalogCryptoEvidenceMetadata(weak);
  Require(!validation.ok(), "catalog accepted weak FNV evidence");
  Require(validation.weak_evidence_rejected,
          "catalog did not mark weak evidence rejection");

  api::ClusterEvidenceIntegrityRequest weak_security;
  weak_security.subject =
      Subject(catalog::ClusterCatalogEvidenceKind::projection);
  weak_security.protection = api::ClusterEvidenceIntegrityProtection::sha256;
  weak_security.provider_authority_claim = true;
  weak_security.presented_algorithm = "crc32";
  weak_security.presented_digest = "crc32:00112233";
  auto result = api::EvaluateClusterEvidenceIntegrity(weak_security);
  Require(!result.ok && result.fail_closed,
          "security accepted weak CRC evidence");
  Require(result.weak_evidence_rejected,
          "security did not report weak evidence rejection");

  api::ClusterEvidenceIntegrityRequest mismatch;
  mismatch.subject = Subject(catalog::ClusterCatalogEvidenceKind::topology);
  mismatch.protection = api::ClusterEvidenceIntegrityProtection::sha256;
  mismatch.provider_authority_claim = true;
  mismatch.presented_algorithm = "sha256";
  mismatch.presented_digest =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";
  result = api::EvaluateClusterEvidenceIntegrity(mismatch);
  Require(!result.ok && result.fail_closed,
          "security accepted mismatched SHA-256 evidence");
  Require(result.diagnostic_code ==
              "SB_CLUSTER_EVIDENCE_INTEGRITY.DIGEST_MISMATCH",
          "digest mismatch diagnostic changed");

  api::ClusterEvidenceIntegrityRequest missing_hmac;
  missing_hmac.subject = Subject(catalog::ClusterCatalogEvidenceKind::route);
  missing_hmac.protection =
      api::ClusterEvidenceIntegrityProtection::hmac_sha256;
  result = api::EvaluateClusterEvidenceIntegrity(missing_hmac);
  Require(!result.ok && result.fail_closed,
          "security accepted HMAC evidence without key material");
}

}  // namespace

int main() {
  TestCanonicalSerializationIsStable();
  TestSha256IntegrityForRequiredEvidenceKinds();
  TestHmacAndSignatureReadyIntegrity();
  TestWeakAndMismatchedEvidenceRejected();
  return EXIT_SUCCESS;
}
