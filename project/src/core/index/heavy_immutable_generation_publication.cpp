// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-045_HEAVY_IMMUTABLE_GENERATION_PUBLICATION
#include "heavy_immutable_generation_publication.hpp"

#include "uuid.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

TypedUuid GeneratedEvidenceId(u64 seed) {
  const auto generated =
      scratchbird::core::uuid::GenerateEngineIdentityV7(UuidKind::object, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool GeneratedDurableUuid(const TypedUuid& value, UuidKind expected_kind) {
  return value.kind == expected_kind && value.valid() &&
         scratchbird::core::uuid::IsDurableEngineIdentityKind(value.kind) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(value.value);
}

bool ExternalAuthorityRequested(
    const HeavyImmutableGenerationValidationRequest& request) {
  return request.parser_finality_authority ||
         request.client_state_authority ||
         request.timestamp_ordering_authority ||
         request.uuid_ordering_authority ||
         request.event_stream_authority ||
         request.reference_authority ||
         request.write_ahead_authority;
}

void UpsertGeneration(HeavyImmutableGenerationLedger* ledger,
                      const HeavyImmutableGenerationDescriptor& generation) {
  if (ledger == nullptr) {
    return;
  }
  for (auto& existing : ledger->generations) {
    if (SameUuid(existing.identity.generation_uuid,
                 generation.identity.generation_uuid)) {
      existing = generation;
      return;
    }
  }
  ledger->generations.push_back(generation);
}

HeavyImmutableGenerationEvidenceRow BuildEvidence(
    HeavyImmutableGenerationLedger* ledger,
    const HeavyImmutableGenerationDescriptor& generation,
    std::string diagnostic_code,
    std::string detail) {
  HeavyImmutableGenerationEvidenceRow evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.evidence_id = GeneratedEvidenceId(4500000 + evidence.sequence);
  evidence.identity = generation.identity;
  evidence.state = generation.state;
  evidence.visible = generation.visible;
  evidence.source_row_count = generation.source_row_count;
  evidence.source_payload_count = generation.source_payload_count;
  evidence.validation_proof_present = generation.validation_proof_present;
  evidence.publication_fence_present = generation.publication_fence_present;
  evidence.engine_owned_mga_publication_fence =
      generation.engine_owned_mga_publication_fence;
  evidence.engine_mga_finality_authority_present =
      generation.engine_mga_finality_authority_present;
  evidence.generation_finality_authority =
      generation.generation_finality_authority;
  evidence.generation_visibility_authority =
      generation.generation_visibility_authority;
  evidence.cluster_provider_routed = generation.cluster_provider_routed;
  evidence.validation_proof_ref = generation.validation_proof_ref;
  evidence.publication_fence_ref = generation.publication_fence_ref;
  evidence.engine_mga_authority_evidence_ref =
      generation.engine_mga_authority_evidence_ref;
  evidence.authority_source = generation.authority_source;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.diagnostic_detail = std::move(detail);
  evidence.exact_diagnostics = generation.exact_diagnostics;
  return evidence;
}

HeavyImmutableGenerationResult FinishLifecycle(
    HeavyImmutableGenerationLedger* ledger,
    const HeavyImmutableGenerationDescriptor& generation,
    Status status,
    bool accepted,
    bool fail_closed,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  HeavyImmutableGenerationResult result;
  result.status = status;
  result.accepted = accepted;
  result.fail_closed = fail_closed;
  result.generation = generation;
  result.evidence =
      BuildEvidence(ledger, generation, diagnostic_code, detail);
  result.diagnostic = MakeHeavyImmutableGenerationDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
    if (!fail_closed) {
      UpsertGeneration(ledger, generation);
      ++ledger->ledger_generation;
    }
  }
  return result;
}

HeavyImmutableGenerationResult RefuseLifecycle(
    HeavyImmutableGenerationLedger* ledger,
    HeavyImmutableGenerationDescriptor generation,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  generation.state = HeavyImmutableGenerationState::refused;
  generation.visible = false;
  return FinishLifecycle(ledger,
                         generation,
                         RefuseStatus(),
                         false,
                         true,
                         std::move(diagnostic_code),
                         std::move(message_key),
                         std::move(detail));
}

bool ValidationProofComplete(
    const HeavyImmutableGenerationDescriptor& generation) {
  return generation.validation_proof_present &&
         generation.validation_succeeded &&
         !generation.validation_proof_ref.empty() &&
         generation.immutable_payload_complete &&
         generation.source_counts_verified &&
         generation.checksum_verified &&
         generation.source_row_count != 0 &&
         generation.source_payload_count != 0;
}

bool PublicationFenceComplete(
    const HeavyImmutableGenerationDescriptor& generation) {
  return generation.publication_fence_present &&
         !generation.publication_fence_ref.empty() &&
         generation.engine_owned_mga_publication_fence &&
         generation.engine_mga_finality_authority_present &&
         !generation.engine_mga_authority_evidence_ref.empty() &&
         !generation.engine_mga_inventory_evidence_ref.empty() &&
         generation.authority_source == kHeavyImmutableGenerationAuthoritySource &&
         !generation.cluster_provider_routed;
}

void ApplyVisibility(HeavyImmutableGenerationDescriptor* generation) {
  generation->visible =
      generation->state == HeavyImmutableGenerationState::published &&
      generation->persisted_descriptor_present &&
      HeavyImmutableGenerationIdentityValid(generation->identity) &&
      ValidationProofComplete(*generation) &&
      PublicationFenceComplete(*generation) &&
      !generation->generation_finality_authority &&
      !generation->generation_visibility_authority;
}

}  // namespace

const char* HeavyImmutableGenerationStateName(
    HeavyImmutableGenerationState state) {
  switch (state) {
    case HeavyImmutableGenerationState::requested: return "requested";
    case HeavyImmutableGenerationState::validated: return "validated";
    case HeavyImmutableGenerationState::published: return "published";
    case HeavyImmutableGenerationState::refused: return "refused";
  }
  return "refused";
}

bool HeavyImmutableGenerationIdentityValid(
    const HeavyImmutableGenerationIdentity& identity) {
  return GeneratedDurableUuid(identity.generation_uuid, UuidKind::object) &&
         GeneratedDurableUuid(identity.table_or_collection_uuid,
                              UuidKind::object) &&
         GeneratedDurableUuid(identity.transaction_uuid,
                              UuidKind::transaction) &&
         !identity.family.empty() &&
         !identity.profile.empty();
}

bool HeavyImmutableGenerationValidated(
    const HeavyImmutableGenerationDescriptor& generation) {
  return generation.state == HeavyImmutableGenerationState::validated &&
         HeavyImmutableGenerationIdentityValid(generation.identity) &&
         ValidationProofComplete(generation) &&
         generation.engine_mga_finality_authority_present &&
         !generation.engine_mga_authority_evidence_ref.empty() &&
         !generation.engine_mga_inventory_evidence_ref.empty() &&
         !generation.generation_finality_authority &&
         !generation.generation_visibility_authority &&
         !generation.cluster_provider_routed;
}

bool HeavyImmutableGenerationPublishable(
    const HeavyImmutableGenerationDescriptor& generation) {
  return HeavyImmutableGenerationValidated(generation) &&
         !generation.publication_fence_present &&
         generation.authority_source == kHeavyImmutableGenerationAuthoritySource;
}

bool HeavyImmutableGenerationPublished(
    const HeavyImmutableGenerationDescriptor& generation) {
  return generation.visible &&
         generation.state == HeavyImmutableGenerationState::published &&
         HeavyImmutableGenerationIdentityValid(generation.identity) &&
         ValidationProofComplete(generation) &&
         PublicationFenceComplete(generation) &&
         !generation.generation_finality_authority &&
         !generation.generation_visibility_authority;
}

HeavyImmutableGenerationResult ValidateHeavyImmutableGeneration(
    HeavyImmutableGenerationLedger* ledger,
    const HeavyImmutableGenerationValidationRequest& request) {
  HeavyImmutableGenerationDescriptor generation;
  generation.identity = request.identity;
  generation.state = HeavyImmutableGenerationState::requested;
  generation.source_row_count = request.source_row_count;
  generation.source_payload_count = request.source_payload_count;
  generation.validation_proof_present =
      !request.validation_proof_ref.empty();
  generation.validation_succeeded = request.validation_succeeded;
  generation.immutable_payload_complete =
      request.immutable_payload_complete;
  generation.source_counts_verified = request.source_counts_verified;
  generation.checksum_verified = request.checksum_verified;
  generation.validation_proof_ref = request.validation_proof_ref;
  generation.engine_mga_authority_evidence_ref =
      request.engine_mga_authority_evidence_ref;
  generation.engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  generation.engine_mga_finality_authority_present =
      !request.engine_mga_authority_evidence_ref.empty() &&
      !request.engine_mga_inventory_evidence_ref.empty();
  generation.operation_id = request.operation_id;
  generation.exact_diagnostics = request.exact_diagnostics;
  generation.cluster_provider_routed = request.cluster_provider_routed;
  generation.generation_finality_authority = false;
  generation.generation_visibility_authority = false;

  if (!HeavyImmutableGenerationIdentityValid(generation.identity)) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.INVALID_IDENTITY",
        "index.heavy_immutable_generation.invalid_identity",
        "generation table-or-collection transaction family and profile identity are required");
  }
  if (!request.source_row_count_present ||
      !request.source_payload_count_present ||
      generation.source_row_count == 0 ||
      generation.source_payload_count == 0) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.SOURCE_COUNTS_MISSING",
        "index.heavy_immutable_generation.source_counts_missing",
        "source row and payload counts are required");
  }
  if (!generation.engine_mga_finality_authority_present) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.MGA_AUTHORITY_MISSING",
        "index.heavy_immutable_generation.mga_authority_missing",
        "engine MGA finality authority and inventory evidence are required");
  }
  if (ExternalAuthorityRequested(request)) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.EXTERNAL_AUTHORITY_REJECTED",
        "index.heavy_immutable_generation.external_authority_rejected",
        "parser client timestamp UUID ordering event stream reference and write-ahead authority claims are forbidden");
  }
  if (request.cluster_provider_routed) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.CLUSTER_PROVIDER_REFUSED",
        "index.heavy_immutable_generation.cluster_provider_refused",
        "local heavy-family immutable generation publication must not route to a cluster provider");
  }
  if (!ValidationProofComplete(generation)) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.VALIDATION_PROOF_MISSING",
        "index.heavy_immutable_generation.validation_proof_missing",
        "validation proof immutable payload source-count and checksum evidence are required");
  }

  generation.state = HeavyImmutableGenerationState::validated;
  return FinishLifecycle(
      ledger,
      generation,
      OkStatus(),
      true,
      false,
      "INDEX.HEAVY_IMMUTABLE_GENERATION.VALIDATED_UNPUBLISHED",
      "index.heavy_immutable_generation.validated_unpublished",
      {});
}

HeavyImmutableGenerationResult PublishHeavyImmutableGeneration(
    HeavyImmutableGenerationLedger* ledger,
    HeavyImmutableGenerationDescriptor* generation,
    const HeavyImmutableGenerationPublicationRequest& request) {
  if (generation == nullptr) {
    return RefuseLifecycle(
        ledger,
        HeavyImmutableGenerationDescriptor{},
        "INDEX.HEAVY_IMMUTABLE_GENERATION.MISSING_RECORD",
        "index.heavy_immutable_generation.missing_record",
        "validated generation descriptor is required");
  }
  if (!HeavyImmutableGenerationPublishable(*generation)) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.PUBLISH_NOT_READY",
        "index.heavy_immutable_generation.publish_not_ready",
        HeavyImmutableGenerationStateName(generation->state));
  }
  if (request.publication_fence_ref.empty() ||
      !request.engine_owned_mga_publication_fence ||
      request.authority_source != kHeavyImmutableGenerationAuthoritySource) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.PUBLICATION_FENCE_MISSING",
        "index.heavy_immutable_generation.publication_fence_missing",
        "engine-owned MGA publication fence evidence is required");
  }
  if (request.cluster_provider_routed) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.CLUSTER_PROVIDER_REFUSED",
        "index.heavy_immutable_generation.cluster_provider_refused",
        "local heavy-family immutable generation publication must not route to a cluster provider");
  }

  auto published = *generation;
  published.state = HeavyImmutableGenerationState::published;
  published.persisted_descriptor_present = true;
  published.publication_fence_present = true;
  published.publication_fence_ref = request.publication_fence_ref;
  published.engine_owned_mga_publication_fence = true;
  published.authority_source = request.authority_source;
  published.cluster_provider_routed = request.cluster_provider_routed;
  published.generation_finality_authority = false;
  published.generation_visibility_authority = false;
  ApplyVisibility(&published);
  if (!published.visible) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.HEAVY_IMMUTABLE_GENERATION.PUBLISH_EVIDENCE_INCOMPLETE",
        "index.heavy_immutable_generation.publish_evidence_incomplete",
        "publish requires validated proof MGA authority and publication fence evidence");
  }
  *generation = published;
  return FinishLifecycle(
      ledger,
      *generation,
      OkStatus(),
      true,
      false,
      "INDEX.HEAVY_IMMUTABLE_GENERATION.PUBLISH_SUCCESS",
      "index.heavy_immutable_generation.publish_success",
      {});
}

DiagnosticRecord MakeHeavyImmutableGenerationDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.heavy_immutable_generation_publication");
}

}  // namespace scratchbird::core::index
