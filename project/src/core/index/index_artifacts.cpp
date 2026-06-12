// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_artifacts.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

void AddEvidence(IndexArtifactDecision* decision,
                 std::string key,
                 std::string value) {
  decision->evidence.push_back(std::move(key) + "=" + std::move(value));
}

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

IndexArtifactDecision Refuse(std::string code,
                             std::string key,
                             bool policy_blocked = false,
                             std::string detail = {}) {
  IndexArtifactDecision decision;
  decision.status = RefuseStatus();
  decision.policy_blocked = policy_blocked;
  decision.diagnostic = MakeIndexArtifactDiagnostic(
      decision.status, std::move(code), std::move(key), std::move(detail));
  AddEvidence(&decision, "diagnostic_code", decision.diagnostic.diagnostic_code);
  return decision;
}
}  // namespace

IndexArtifactDecision PlanIndexArtifactOperation(const IndexArtifactRequest& request) {
  if (!request.index_uuid.valid() || request.family == IndexFamily::unknown) {
    return Refuse("SB-INDEX-ARTIFACT-INVALID-REQUEST", "index.artifact.invalid_request");
  }
  if (!request.durable_metadata_present ||
      !request.durable_metadata.durable_metadata_present) {
    return Refuse("SB-INDEX-ARTIFACT-DURABLE-METADATA-REQUIRED",
                  "index.artifact.durable_metadata_required");
  }
  if (request.durable_metadata.index_uuid.kind != request.index_uuid.kind ||
      request.durable_metadata.index_uuid.value != request.index_uuid.value ||
      request.durable_metadata.family != request.family) {
    return Refuse("SB-INDEX-ARTIFACT-DURABLE-METADATA-MISMATCH",
                  "index.artifact.durable_metadata_mismatch");
  }
  const auto metadata_validation =
      ValidateIndexMetapageDurableMetadata(request.durable_metadata);
  if (!metadata_validation.ok()) {
    return Refuse("SB-INDEX-ARTIFACT-DURABLE-METADATA-INVALID",
                  "index.artifact.durable_metadata_invalid",
                  false,
                  metadata_validation.diagnostic.diagnostic_code);
  }
  if ((request.operation == IndexArtifactOperation::import_definition ||
       request.operation == IndexArtifactOperation::reference_import) &&
      !request.policy_allows_import) {
    return Refuse("SB-INDEX-ARTIFACT-IMPORT-POLICY-REFUSED", "index.artifact.import_policy_refused", true);
  }
  if (!request.finality_proven) {
    return Refuse("SB-INDEX-ARTIFACT-FINALITY-REQUIRED", "index.artifact.finality_required");
  }
  IndexArtifactDecision decision;
  decision.status = OkStatus();
  decision.allowed = true;
  decision.canonical_artifact_class = "index.definition:" + request.semantic_profile_id;
  decision.emulated = !request.reference_name.empty();
  decision.durable_metadata_valid = true;
  decision.checksum_profile_bound =
      request.durable_metadata.checksum_profile !=
      PageBodyChecksumProfile::unknown;
  decision.format_compatible = request.durable_metadata.format_compatible;
  decision.identity_bound = request.durable_metadata.identity_bound;
  decision.descriptor_hash_bound =
      request.durable_metadata.descriptor_hash_bound;
  decision.route_capability_hash_bound =
      request.durable_metadata.route_capability_bound;
  decision.provider_evidence_hash_bound =
      request.durable_metadata.provider_evidence_hash_bound;
  decision.family_validator_passed =
      request.durable_metadata.family_validator_passed;
  decision.requires_rebuild = request.operation == IndexArtifactOperation::reference_import ||
                              request.operation == IndexArtifactOperation::seed_bind ||
                              !request.resource_epoch_current;
  decision.requires_verify = true;
  if (!request.preserve_uuid && request.operation == IndexArtifactOperation::import_definition) {
    decision.requires_rebuild = true;
  }
  AddEvidence(&decision, "durable_metadata_valid",
              BoolText(decision.durable_metadata_valid));
  AddEvidence(&decision, "checksum_profile",
              scratchbird::storage::page::PageBodyChecksumProfileName(
                  request.durable_metadata.checksum_profile));
  AddEvidence(&decision, "format_compatible",
              BoolText(decision.format_compatible));
  AddEvidence(&decision, "identity_bound", BoolText(decision.identity_bound));
  AddEvidence(&decision, "descriptor_hash_bound",
              BoolText(decision.descriptor_hash_bound));
  AddEvidence(&decision, "route_capability_hash_bound",
              BoolText(decision.route_capability_hash_bound));
  AddEvidence(&decision, "provider_evidence_hash_bound",
              BoolText(decision.provider_evidence_hash_bound));
  AddEvidence(&decision, "family_validator_passed",
              BoolText(decision.family_validator_passed));
  return decision;
}

DiagnosticRecord MakeIndexArtifactDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.artifacts");
}

}  // namespace scratchbird::core::index
