// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_VECTOR_GENERATION_PUBLICATION
#include "vector_index_generation_publication.hpp"

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

Status WarnStatus() {
  return {StatusCode::ok, Severity::warning, Subsystem::engine};
}

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated =
      scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
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

bool ExternalAuthorityRequested(const VectorGenerationRequest& request) {
  return request.parser_finality_authority ||
         request.client_state_authority ||
         request.timestamp_ordering_authority ||
         request.uuid_ordering_authority ||
         request.event_stream_authority ||
         request.reference_authority ||
         request.write_ahead_authority;
}

bool HasMgaEvidence(const VectorGenerationDescriptor& generation) {
  return !generation.engine_mga_inventory_evidence_ref.empty() &&
         !generation.engine_mga_horizon_evidence_ref.empty();
}

bool SealedPublicationEvidenceComplete(
    const VectorGenerationDescriptor& generation) {
  return generation.validation_evidence_present &&
         !generation.validation_evidence_ref.empty() &&
         generation.sealed_generation_evidence_present &&
         !generation.sealed_generation_evidence_ref.empty() &&
         generation.recall_contract_evidence_present &&
         !generation.recall_contract_evidence_ref.empty() &&
         VectorGenerationRecallContractSatisfied(generation.recall_contract) &&
         generation.publish_barrier_evidence_present &&
         !generation.publish_barrier_evidence_ref.empty() &&
         generation.publish_barrier_engine_owned_mga &&
         HasMgaEvidence(generation) &&
         VectorGenerationResourceEnvelopeBounded(
             generation.resource_envelope);
}

void ApplyVisibility(VectorGenerationDescriptor* generation) {
  generation->visible =
      generation->state == VectorGenerationState::published &&
      generation->persisted_record_present &&
      generation->checksum_valid &&
      generation->complete &&
      !generation->stale &&
      VectorGenerationDescriptorIdentityValid(*generation) &&
      VectorGenerationDescriptorAuthorityClean(*generation) &&
      SealedPublicationEvidenceComplete(*generation);
}

void UpsertGeneration(VectorGenerationLedger* ledger,
                      const VectorGenerationDescriptor& generation) {
  if (ledger == nullptr) {
    return;
  }
  for (auto& existing : ledger->generations) {
    if (SameUuid(existing.generation_uuid, generation.generation_uuid)) {
      existing = generation;
      return;
    }
  }
  ledger->generations.push_back(generation);
}

VectorGenerationEvidenceRow BuildEvidence(
    VectorGenerationLedger* ledger,
    const VectorGenerationDescriptor& generation,
    std::string diagnostic_code,
    std::string detail) {
  VectorGenerationEvidenceRow evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.evidence_id = GeneratedId(UuidKind::object,
                                     4400000 + evidence.sequence);
  evidence.generation_uuid = generation.generation_uuid;
  evidence.index_uuid = generation.index_uuid;
  evidence.table_uuid = generation.table_uuid;
  evidence.state = generation.state;
  evidence.generation = generation.generation;
  evidence.visible = generation.visible;
  evidence.validation_evidence_present =
      generation.validation_evidence_present;
  evidence.sealed_generation_evidence_present =
      generation.sealed_generation_evidence_present;
  evidence.recall_contract_evidence_present =
      generation.recall_contract_evidence_present;
  evidence.publish_barrier_evidence_present =
      generation.publish_barrier_evidence_present;
  evidence.publish_barrier_engine_owned_mga =
      generation.publish_barrier_engine_owned_mga;
  evidence.memory_limit_bytes =
      generation.resource_envelope.memory_limit_bytes;
  evidence.memory_observed_bytes =
      generation.resource_envelope.memory_observed_bytes;
  evidence.temp_space_limit_bytes =
      generation.resource_envelope.temp_space_limit_bytes;
  evidence.temp_space_observed_bytes =
      generation.resource_envelope.temp_space_observed_bytes;
  evidence.worker_limit = generation.resource_envelope.worker_limit;
  evidence.workers_used = generation.resource_envelope.workers_used;
  evidence.resource_governor_evidence_present =
      generation.resource_envelope.resource_governor_evidence_present;
  evidence.authority_source = generation.authority_source;
  evidence.parser_finality_authority = false;
  evidence.client_state_authority = false;
  evidence.timestamp_ordering_authority = false;
  evidence.uuid_ordering_authority = false;
  evidence.event_stream_authority = false;
  evidence.reference_authority = false;
  evidence.write_ahead_authority = false;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.diagnostic_detail = std::move(detail);
  return evidence;
}

VectorGenerationLifecycleResult FinishLifecycle(
    VectorGenerationLedger* ledger,
    const VectorGenerationDescriptor& generation,
    Status status,
    bool accepted,
    bool fail_closed,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  VectorGenerationLifecycleResult result;
  result.status = status;
  result.accepted = accepted;
  result.fail_closed = fail_closed;
  result.generation = generation;
  result.evidence = BuildEvidence(ledger,
                                  generation,
                                  diagnostic_code,
                                  detail);
  result.diagnostic = MakeVectorGenerationDiagnostic(
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

VectorGenerationLifecycleResult RefuseLifecycle(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor generation,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  generation.state = VectorGenerationState::refused;
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

VectorGenerationAccessPlan Fallback(VectorGenerationFallbackReason reason,
                                    bool fallback_available,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {}) {
  VectorGenerationAccessPlan plan;
  plan.status = fallback_available ? WarnStatus() : RefuseStatus();
  plan.selected_category = IndexPlanCategory::fallback_full_scan;
  plan.selected_access =
      fallback_available ? "exact_vector_scan" : "refused";
  plan.fallback_reason = VectorGenerationFallbackReasonName(reason);
  plan.ann_generation_selected = false;
  plan.exact_vector_scan_fallback_selected = fallback_available;
  plan.exact_vector_scan_fallback_available = fallback_available;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;
  plan.generation_metadata_visibility_authority = false;
  plan.generation_metadata_finality_authority = false;
  plan.diagnostic = MakeVectorGenerationDiagnostic(
      plan.status,
      fallback_available ? std::move(diagnostic_code)
                         : "INDEX.VECTOR_GENERATION.UNSAFE_NO_EXACT_FALLBACK",
      fallback_available ? std::move(message_key)
                         : "index.vector_generation.unsafe_no_exact_fallback",
      fallback_available ? std::move(detail) : std::move(diagnostic_code));
  plan.actions.push_back(fallback_available
                             ? "select_exact_vector_scan_fallback"
                             : "refuse_unsafe_vector_generation_without_exact_fallback");
  plan.actions.push_back(
      "do_not_use_vector_generation_as_visibility_or_finality_authority");
  return plan;
}

}  // namespace

const char* VectorGenerationStateName(VectorGenerationState state) {
  switch (state) {
    case VectorGenerationState::requested: return "requested";
    case VectorGenerationState::building: return "building";
    case VectorGenerationState::trained: return "trained";
    case VectorGenerationState::validated: return "validated";
    case VectorGenerationState::sealed: return "sealed";
    case VectorGenerationState::published: return "published";
    case VectorGenerationState::retired: return "retired";
    case VectorGenerationState::refused: return "refused";
  }
  return "refused";
}

const char* VectorGenerationFallbackReasonName(
    VectorGenerationFallbackReason reason) {
  switch (reason) {
    case VectorGenerationFallbackReason::none: return "none";
    case VectorGenerationFallbackReason::disabled_generation_exact_fallback:
      return "disabled_generation_exact_fallback";
    case VectorGenerationFallbackReason::missing_generation_exact_fallback:
      return "missing_generation_exact_fallback";
    case VectorGenerationFallbackReason::stale_generation_exact_fallback:
      return "stale_generation_exact_fallback";
    case VectorGenerationFallbackReason::corrupt_generation_exact_fallback:
      return "corrupt_generation_exact_fallback";
    case VectorGenerationFallbackReason::incomplete_generation_exact_fallback:
      return "incomplete_generation_exact_fallback";
    case VectorGenerationFallbackReason::unsealed_generation_exact_fallback:
      return "unsealed_generation_exact_fallback";
    case VectorGenerationFallbackReason::
        non_authoritative_generation_exact_fallback:
      return "non_authoritative_generation_exact_fallback";
    case VectorGenerationFallbackReason::invalid_identity_exact_fallback:
      return "invalid_identity_exact_fallback";
    case VectorGenerationFallbackReason::recall_contract_exact_fallback:
      return "recall_contract_exact_fallback";
    case VectorGenerationFallbackReason::unsafe_state_exact_fallback:
      return "unsafe_state_exact_fallback";
  }
  return "none";
}

const char* VectorGenerationRecoveryClassName(
    VectorGenerationRecoveryClass recovery_class) {
  switch (recovery_class) {
    case VectorGenerationRecoveryClass::clean_sealed_published_generation:
      return "clean_sealed_published_generation";
    case VectorGenerationRecoveryClass::incomplete_pending_exact_fallback:
      return "incomplete_pending_exact_fallback";
    case VectorGenerationRecoveryClass::unsafe_visible_generation_refused:
      return "unsafe_visible_generation_refused";
  }
  return "unsafe_visible_generation_refused";
}

const char* VectorGenerationRecoveryActionName(
    VectorGenerationRecoveryAction action) {
  switch (action) {
    case VectorGenerationRecoveryAction::keep_published_generation:
      return "keep_published_generation";
    case VectorGenerationRecoveryAction::use_exact_vector_scan_fallback:
      return "use_exact_vector_scan_fallback";
    case VectorGenerationRecoveryAction::refuse_unsafe_generation_use:
      return "refuse_unsafe_generation_use";
  }
  return "refuse_unsafe_generation_use";
}

bool VectorGenerationResourceEnvelopeBounded(
    const VectorGenerationResourceEnvelope& envelope) {
  return envelope.resource_governor_evidence_present &&
         !envelope.resource_governor_evidence_ref.empty() &&
         envelope.memory_limit_bytes != 0 &&
         envelope.temp_space_limit_bytes != 0 &&
         envelope.worker_limit != 0 &&
         envelope.memory_observed_bytes <= envelope.memory_limit_bytes &&
         envelope.temp_space_observed_bytes <=
             envelope.temp_space_limit_bytes &&
         envelope.workers_used != 0 &&
         envelope.workers_used <= envelope.worker_limit;
}

bool VectorGenerationRecallContractSatisfied(
    const VectorGenerationRecallContract& contract) {
  return contract.evidence_present &&
         !contract.evidence_ref.empty() &&
         contract.top_k != 0 &&
         contract.exact_sample_rows != 0 &&
         contract.deterministic_sample &&
         contract.required_recall > 0.0 &&
         contract.required_recall <= 1.0 &&
         contract.observed_recall >= contract.required_recall &&
         contract.observed_recall <= 1.0;
}

bool VectorGenerationDescriptorIdentityValid(
    const VectorGenerationDescriptor& generation) {
  return GeneratedDurableUuid(generation.generation_uuid, UuidKind::object) &&
         GeneratedDurableUuid(generation.index_uuid, UuidKind::object) &&
         GeneratedDurableUuid(generation.table_uuid, UuidKind::object);
}

bool VectorGenerationDescriptorAuthorityClean(
    const VectorGenerationDescriptor& generation) {
  return generation.authority_source == kVectorGenerationAuthoritySource &&
         !generation.parser_finality_authority_claimed &&
         !generation.client_finality_authority_claimed &&
         !generation.timestamp_finality_authority_claimed &&
         !generation.uuid_ordering_finality_authority_claimed &&
         !generation.event_stream_finality_authority_claimed &&
         !generation.reference_finality_authority_claimed &&
         !generation.write_ahead_log_finality_authority_claimed;
}

bool VectorGenerationDescriptorUsable(
    const VectorGenerationDescriptor& generation) {
  return generation.visible &&
         generation.state == VectorGenerationState::published &&
         generation.persisted_record_present &&
         generation.checksum_valid &&
         generation.complete &&
         !generation.stale &&
         generation.generation != 0 &&
         VectorGenerationDescriptorIdentityValid(generation) &&
         VectorGenerationDescriptorAuthorityClean(generation) &&
         SealedPublicationEvidenceComplete(generation);
}

VectorGenerationLifecycleResult RequestVectorGeneration(
    VectorGenerationLedger* ledger,
    const VectorGenerationRequest& request) {
  VectorGenerationDescriptor generation;
  generation.generation_uuid = request.generation_uuid.valid()
                                   ? request.generation_uuid
                                   : GeneratedId(
                                         UuidKind::object,
                                         4410000 +
                                             (ledger == nullptr
                                                  ? 1
                                                  : ledger->next_evidence_sequence));
  generation.index_uuid = request.index_uuid;
  generation.table_uuid = request.table_uuid;
  generation.generation = request.generation;
  generation.algorithm = request.algorithm;
  generation.engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  generation.engine_mga_horizon_evidence_ref =
      request.engine_mga_horizon_evidence_ref;
  generation.resource_envelope = request.resource_envelope;
  generation.state = VectorGenerationState::requested;
  generation.visible = false;

  if (!VectorGenerationDescriptorIdentityValid(generation)) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.VECTOR_GENERATION.INVALID_IDENTITY",
        "index.vector_generation.invalid_identity",
        "generation index and table UUIDs must be generated durable object UUIDs");
  }
  if (generation.generation == 0) {
    return RefuseLifecycle(ledger,
                           generation,
                           "INDEX.VECTOR_GENERATION.INVALID_GENERATION",
                           "index.vector_generation.invalid_generation",
                           "generation must be nonzero");
  }
  if (!HasMgaEvidence(generation)) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.VECTOR_GENERATION.MGA_EVIDENCE_MISSING",
        "index.vector_generation.mga_evidence_missing",
        "vector generation requires engine MGA inventory and horizon evidence");
  }
  if (!VectorGenerationResourceEnvelopeBounded(generation.resource_envelope)) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.VECTOR_GENERATION.RESOURCE_ENVELOPE_REFUSED",
        "index.vector_generation.resource_envelope_refused",
        "vector generation requires bounded memory temp space and worker evidence");
  }
  if (ExternalAuthorityRequested(request)) {
    return RefuseLifecycle(
        ledger,
        generation,
        "INDEX.VECTOR_GENERATION.EXTERNAL_AUTHORITY_REJECTED",
        "index.vector_generation.external_authority_rejected",
        "parser client timestamp UUID ordering event stream reference and write-ahead authority claims are forbidden");
  }

  return FinishLifecycle(ledger,
                         generation,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.VECTOR_GENERATION.REQUESTED_UNPUBLISHED",
                         "index.vector_generation.requested_unpublished",
                         {});
}

VectorGenerationLifecycleResult StartVectorGenerationBuild(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation) {
  if (generation == nullptr) {
    return RefuseLifecycle(ledger,
                           VectorGenerationDescriptor{},
                           "INDEX.VECTOR_GENERATION.MISSING_RECORD",
                           "index.vector_generation.missing_record",
                           "generation record is required");
  }
  if (generation->state != VectorGenerationState::requested) {
    return RefuseLifecycle(ledger,
                           *generation,
                           "INDEX.VECTOR_GENERATION.INVALID_TRANSITION",
                           "index.vector_generation.invalid_transition",
                           VectorGenerationStateName(generation->state));
  }
  generation->state = VectorGenerationState::building;
  ApplyVisibility(generation);
  return FinishLifecycle(ledger,
                         *generation,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.VECTOR_GENERATION.BUILDING_UNPUBLISHED",
                         "index.vector_generation.building_unpublished",
                         {});
}

VectorGenerationLifecycleResult MarkVectorGenerationTrained(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationTrainingRequest& request) {
  if (generation == nullptr) {
    return RefuseLifecycle(ledger,
                           VectorGenerationDescriptor{},
                           "INDEX.VECTOR_GENERATION.MISSING_RECORD",
                           "index.vector_generation.missing_record",
                           "generation record is required");
  }
  if (generation->state != VectorGenerationState::building) {
    return RefuseLifecycle(ledger,
                           *generation,
                           "INDEX.VECTOR_GENERATION.INVALID_TRANSITION",
                           "index.vector_generation.invalid_transition",
                           VectorGenerationStateName(generation->state));
  }
  if (!request.training_succeeded ||
      request.training_evidence_ref.empty() ||
      !request.complete_training_set) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.VECTOR_GENERATION.TRAINING_REFUSED",
        "index.vector_generation.training_refused",
        "training requires success evidence and complete training-set evidence");
  }
  generation->state = VectorGenerationState::trained;
  generation->training_evidence_present = true;
  generation->training_evidence_ref = request.training_evidence_ref;
  ApplyVisibility(generation);
  return FinishLifecycle(ledger,
                         *generation,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.VECTOR_GENERATION.TRAINED_UNPUBLISHED",
                         "index.vector_generation.trained_unpublished",
                         {});
}

VectorGenerationLifecycleResult ValidateVectorGeneration(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationValidationRequest& request) {
  if (generation == nullptr) {
    return RefuseLifecycle(ledger,
                           VectorGenerationDescriptor{},
                           "INDEX.VECTOR_GENERATION.MISSING_RECORD",
                           "index.vector_generation.missing_record",
                           "generation record is required");
  }
  if (generation->state != VectorGenerationState::trained) {
    return RefuseLifecycle(ledger,
                           *generation,
                           "INDEX.VECTOR_GENERATION.INVALID_TRANSITION",
                           "index.vector_generation.invalid_transition",
                           VectorGenerationStateName(generation->state));
  }
  if (!request.validation_succeeded ||
      request.validation_evidence_ref.empty() ||
      !request.complete_generation) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.VECTOR_GENERATION.VALIDATION_REFUSED",
        "index.vector_generation.validation_refused",
        "validation requires success evidence and complete generation bytes");
  }
  generation->state = VectorGenerationState::validated;
  generation->validation_evidence_present = true;
  generation->validation_evidence_ref = request.validation_evidence_ref;
  generation->complete = true;
  ApplyVisibility(generation);
  return FinishLifecycle(ledger,
                         *generation,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.VECTOR_GENERATION.VALIDATED_UNPUBLISHED",
                         "index.vector_generation.validated_unpublished",
                         {});
}

VectorGenerationLifecycleResult SealVectorGeneration(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationSealRequest& request) {
  if (generation == nullptr) {
    return RefuseLifecycle(ledger,
                           VectorGenerationDescriptor{},
                           "INDEX.VECTOR_GENERATION.MISSING_RECORD",
                           "index.vector_generation.missing_record",
                           "generation record is required");
  }
  if (generation->state != VectorGenerationState::validated) {
    return RefuseLifecycle(ledger,
                           *generation,
                           "INDEX.VECTOR_GENERATION.INVALID_TRANSITION",
                           "index.vector_generation.invalid_transition",
                           VectorGenerationStateName(generation->state));
  }
  if (!request.sealed_bytes_complete ||
      request.sealed_generation_evidence_ref.empty()) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.VECTOR_GENERATION.SEAL_REFUSED",
        "index.vector_generation.seal_refused",
        "seal requires complete sealed generation evidence");
  }
  if (!VectorGenerationRecallContractSatisfied(request.recall_contract)) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.VECTOR_GENERATION.RECALL_CONTRACT_REFUSED",
        "index.vector_generation.recall_contract_refused",
        "sealed ANN publication requires deterministic exact-sample recall evidence");
  }

  generation->state = VectorGenerationState::sealed;
  generation->sealed_generation_evidence_present = true;
  generation->sealed_generation_evidence_ref =
      request.sealed_generation_evidence_ref;
  generation->recall_contract = request.recall_contract;
  generation->recall_contract_evidence_present = true;
  generation->recall_contract_evidence_ref =
      request.recall_contract.evidence_ref;
  ApplyVisibility(generation);
  return FinishLifecycle(ledger,
                         *generation,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.VECTOR_GENERATION.SEALED_UNPUBLISHED",
                         "index.vector_generation.sealed_unpublished",
                         {});
}

VectorGenerationLifecycleResult PublishVectorGeneration(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationPublishRequest& request) {
  if (generation == nullptr) {
    return RefuseLifecycle(ledger,
                           VectorGenerationDescriptor{},
                           "INDEX.VECTOR_GENERATION.MISSING_RECORD",
                           "index.vector_generation.missing_record",
                           "generation record is required");
  }
  if (generation->state != VectorGenerationState::sealed) {
    return RefuseLifecycle(ledger,
                           *generation,
                           "INDEX.VECTOR_GENERATION.PUBLISH_NOT_READY",
                           "index.vector_generation.publish_not_ready",
                           VectorGenerationStateName(generation->state));
  }
  if (request.publish_barrier_evidence_ref.empty() ||
      !request.engine_owned_mga_publish_barrier ||
      request.authority_source != kVectorGenerationAuthoritySource) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.VECTOR_GENERATION.PUBLISH_BARRIER_REFUSED",
        "index.vector_generation.publish_barrier_refused",
        "publish requires engine-owned MGA barrier evidence");
  }
  auto published = *generation;
  published.state = VectorGenerationState::published;
  published.persisted_record_present = true;
  published.publish_barrier_evidence_present = true;
  published.publish_barrier_engine_owned_mga = true;
  published.publish_barrier_evidence_ref =
      request.publish_barrier_evidence_ref;
  published.authority_source = request.authority_source;
  ApplyVisibility(&published);
  if (!published.visible) {
    return RefuseLifecycle(
        ledger,
        *generation,
        "INDEX.VECTOR_GENERATION.PUBLISH_EVIDENCE_MISSING",
        "index.vector_generation.publish_evidence_missing",
        "publish requires validation sealed generation recall contract and engine-owned MGA barrier evidence");
  }
  *generation = published;
  return FinishLifecycle(ledger,
                         *generation,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.VECTOR_GENERATION.PUBLISH_SUCCESS",
                         "index.vector_generation.publish_success",
                         {});
}

VectorGenerationAccessPlan PlanVectorGenerationAccess(
    const VectorGenerationAccessRequest& request) {
  if (!request.vector_generation_enabled) {
    return Fallback(
        VectorGenerationFallbackReason::disabled_generation_exact_fallback,
        request.exact_vector_scan_fallback_available,
        "INDEX.VECTOR_GENERATION.DISABLED_EXACT_FALLBACK",
        "index.vector_generation.disabled_exact_fallback");
  }
  if (!request.base_row_mga_recheck_required ||
      !request.base_row_security_recheck_required) {
    return Fallback(
        VectorGenerationFallbackReason::
            non_authoritative_generation_exact_fallback,
        request.exact_vector_scan_fallback_available,
        "INDEX.VECTOR_GENERATION.BASE_ROW_RECHECK_REQUIRED",
        "index.vector_generation.base_row_recheck_required");
  }
  if (request.generations.empty()) {
    return Fallback(
        VectorGenerationFallbackReason::missing_generation_exact_fallback,
        request.exact_vector_scan_fallback_available,
        "INDEX.VECTOR_GENERATION.MISSING_EXACT_FALLBACK",
        "index.vector_generation.missing_exact_fallback");
  }

  VectorGenerationAccessPlan plan;
  plan.status = OkStatus();
  plan.selected_category = IndexPlanCategory::vector_search;
  plan.selected_access = "sealed_ann_vector_scan";
  plan.fallback_reason =
      VectorGenerationFallbackReasonName(VectorGenerationFallbackReason::none);
  plan.ann_generation_selected = true;
  plan.exact_vector_scan_fallback_selected = false;
  plan.exact_vector_scan_fallback_available =
      request.exact_vector_scan_fallback_available;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;

  for (const auto& generation : request.generations) {
    if (!VectorGenerationDescriptorIdentityValid(generation)) {
      return Fallback(
          VectorGenerationFallbackReason::invalid_identity_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.INVALID_IDENTITY_EXACT_FALLBACK",
          "index.vector_generation.invalid_identity_exact_fallback");
    }
    if (!VectorGenerationDescriptorAuthorityClean(generation)) {
      return Fallback(
          VectorGenerationFallbackReason::
              non_authoritative_generation_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.NON_AUTHORITATIVE_EXACT_FALLBACK",
          "index.vector_generation.non_authoritative_exact_fallback",
          generation.authority_source);
    }
    if (generation.state == VectorGenerationState::requested ||
        generation.state == VectorGenerationState::building ||
        generation.state == VectorGenerationState::trained ||
        generation.state == VectorGenerationState::validated) {
      return Fallback(
          VectorGenerationFallbackReason::incomplete_generation_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.INCOMPLETE_EXACT_FALLBACK",
          "index.vector_generation.incomplete_exact_fallback",
          VectorGenerationStateName(generation.state));
    }
    if (generation.state == VectorGenerationState::sealed ||
        !generation.persisted_record_present) {
      return Fallback(
          VectorGenerationFallbackReason::unsealed_generation_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.UNSEALED_EXACT_FALLBACK",
          "index.vector_generation.unsealed_exact_fallback",
          VectorGenerationStateName(generation.state));
    }
    if (!generation.checksum_valid ||
        generation.state == VectorGenerationState::refused ||
        generation.generation == 0) {
      return Fallback(
          VectorGenerationFallbackReason::corrupt_generation_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.CORRUPT_EXACT_FALLBACK",
          "index.vector_generation.corrupt_exact_fallback");
    }
    if (generation.stale ||
        generation.state == VectorGenerationState::retired) {
      return Fallback(
          VectorGenerationFallbackReason::stale_generation_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.STALE_EXACT_FALLBACK",
          "index.vector_generation.stale_exact_fallback");
    }
    if (!VectorGenerationRecallContractSatisfied(
            generation.recall_contract)) {
      return Fallback(
          VectorGenerationFallbackReason::recall_contract_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.RECALL_EXACT_FALLBACK",
          "index.vector_generation.recall_exact_fallback");
    }
    if (!VectorGenerationDescriptorUsable(generation)) {
      return Fallback(
          VectorGenerationFallbackReason::unsafe_state_exact_fallback,
          request.exact_vector_scan_fallback_available,
          "INDEX.VECTOR_GENERATION.UNSAFE_STATE_EXACT_FALLBACK",
          "index.vector_generation.unsafe_state_exact_fallback");
    }
    plan.published_generation_uuids.push_back(generation.generation_uuid);
  }

  plan.diagnostic = MakeVectorGenerationDiagnostic(
      plan.status,
      "INDEX.VECTOR_GENERATION.SELECTED",
      "index.vector_generation.selected");
  plan.actions.push_back("use_sealed_published_ann_generation");
  plan.actions.push_back("apply_exact_rerank_and_base_row_mga_recheck");
  plan.actions.push_back("apply_base_row_security_recheck");
  plan.actions.push_back(
      "do_not_use_vector_generation_as_visibility_or_finality_authority");
  return plan;
}

VectorGenerationRecoveryResult ClassifyVectorGenerationReopen(
    const VectorGenerationLedger& ledger,
    bool exact_vector_scan_fallback_available) {
  VectorGenerationRecoveryResult result;
  bool saw_visible_usable = false;
  bool saw_pending = false;
  bool saw_unsafe_visible = false;
  for (const auto& generation : ledger.generations) {
    if (generation.visible && VectorGenerationDescriptorUsable(generation)) {
      saw_visible_usable = true;
      continue;
    }
    if (generation.visible &&
        !VectorGenerationDescriptorUsable(generation)) {
      saw_unsafe_visible = true;
    }
    if (generation.state == VectorGenerationState::requested ||
        generation.state == VectorGenerationState::building ||
        generation.state == VectorGenerationState::trained ||
        generation.state == VectorGenerationState::validated ||
        generation.state == VectorGenerationState::sealed) {
      saw_pending = true;
    }
  }

  if (saw_unsafe_visible) {
    result.status = RefuseStatus();
    result.fail_closed = true;
    result.recovery_class =
        VectorGenerationRecoveryClass::unsafe_visible_generation_refused;
    result.action =
        VectorGenerationRecoveryAction::refuse_unsafe_generation_use;
    result.stable_reason =
        "visible vector generation failed identity authority recall or MGA evidence validation";
    result.diagnostic = MakeVectorGenerationDiagnostic(
        result.status,
        "INDEX.VECTOR_GENERATION.RECOVERY_UNSAFE_REFUSED",
        "index.vector_generation.recovery_unsafe_refused",
        result.stable_reason);
    return result;
  }
  if (saw_visible_usable && !saw_pending) {
    result.status = OkStatus();
    result.fail_closed = false;
    result.recovery_class =
        VectorGenerationRecoveryClass::clean_sealed_published_generation;
    result.action = VectorGenerationRecoveryAction::keep_published_generation;
    result.stable_reason =
        "sealed published vector generation retains authoritative evidence";
    result.diagnostic = MakeVectorGenerationDiagnostic(
        result.status,
        "INDEX.VECTOR_GENERATION.RECOVERY_KEEP_PUBLISHED",
        "index.vector_generation.recovery_keep_published",
        result.stable_reason);
    return result;
  }

  result.status = exact_vector_scan_fallback_available ? WarnStatus()
                                                       : RefuseStatus();
  result.fail_closed = !exact_vector_scan_fallback_available;
  result.recovery_class =
      exact_vector_scan_fallback_available
          ? VectorGenerationRecoveryClass::incomplete_pending_exact_fallback
          : VectorGenerationRecoveryClass::unsafe_visible_generation_refused;
  result.action =
      exact_vector_scan_fallback_available
          ? VectorGenerationRecoveryAction::use_exact_vector_scan_fallback
          : VectorGenerationRecoveryAction::refuse_unsafe_generation_use;
  result.stable_reason =
      exact_vector_scan_fallback_available
          ? "incomplete or pending vector generation ignored in favor of exact vector scan fallback"
          : "incomplete vector generation cannot be used without exact fallback";
  result.diagnostic = MakeVectorGenerationDiagnostic(
      result.status,
      exact_vector_scan_fallback_available
          ? "INDEX.VECTOR_GENERATION.RECOVERY_EXACT_FALLBACK"
          : "INDEX.VECTOR_GENERATION.RECOVERY_UNSAFE_REFUSED",
      exact_vector_scan_fallback_available
          ? "index.vector_generation.recovery_exact_fallback"
          : "index.vector_generation.recovery_unsafe_refused",
      result.stable_reason);
  return result;
}

std::vector<std::string> VectorGenerationRecallLifecycleEvidenceAdapter(
    const VectorTrainingRecallLifecycleDecision& decision) {
  auto evidence = VectorTrainingRecallLifecycleEvidence(decision);
  evidence.push_back("vector_generation_publication_lifecycle_adapter=true");
  evidence.push_back("vector_generation_metadata_visibility_authority=false");
  evidence.push_back("vector_generation_metadata_finality_authority=false");
  return evidence;
}

DiagnosticRecord MakeVectorGenerationDiagnostic(Status status,
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
                        "core.index.vector_generation_publication");
}

}  // namespace scratchbird::core::index
