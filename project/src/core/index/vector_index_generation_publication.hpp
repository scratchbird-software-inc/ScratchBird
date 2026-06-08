// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_VECTOR_GENERATION_PUBLICATION
#include "index_optimizer_integration.hpp"
#include "index_spatial_vector_graph_access.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kVectorGenerationPublicationSearchKey =
    "DPC_VECTOR_GENERATION_PUBLICATION";
inline constexpr const char* kVectorGenerationAuthoritySource =
    "engine_mga_vector_generation_publish_barrier";

enum class VectorGenerationState : u32 {
  requested = 1,
  building = 2,
  trained = 3,
  validated = 4,
  sealed = 5,
  published = 6,
  retired = 7,
  refused = 8
};

enum class VectorGenerationFallbackReason : u32 {
  none = 1,
  disabled_generation_exact_fallback = 2,
  missing_generation_exact_fallback = 3,
  stale_generation_exact_fallback = 4,
  corrupt_generation_exact_fallback = 5,
  incomplete_generation_exact_fallback = 6,
  unsealed_generation_exact_fallback = 7,
  non_authoritative_generation_exact_fallback = 8,
  invalid_identity_exact_fallback = 9,
  recall_contract_exact_fallback = 10,
  unsafe_state_exact_fallback = 11
};

enum class VectorGenerationRecoveryClass : u32 {
  clean_sealed_published_generation = 1,
  incomplete_pending_exact_fallback = 2,
  unsafe_visible_generation_refused = 3
};

enum class VectorGenerationRecoveryAction : u32 {
  keep_published_generation = 1,
  use_exact_vector_scan_fallback = 2,
  refuse_unsafe_generation_use = 3
};

struct VectorGenerationResourceEnvelope {
  u64 memory_limit_bytes = 0;
  u64 memory_observed_bytes = 0;
  u64 temp_space_limit_bytes = 0;
  u64 temp_space_observed_bytes = 0;
  u32 worker_limit = 0;
  u32 workers_used = 0;
  bool resource_governor_evidence_present = false;
  std::string resource_governor_evidence_ref;
};

struct VectorGenerationRecallContract {
  u32 top_k = 0;
  u32 exact_sample_rows = 0;
  double required_recall = 1.0;
  double observed_recall = 0.0;
  bool deterministic_sample = false;
  bool evidence_present = false;
  std::string evidence_ref;
};

struct VectorGenerationDescriptor {
  TypedUuid generation_uuid;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  u64 generation = 0;
  IndexVectorAlgorithm algorithm = IndexVectorAlgorithm::hnsw;
  VectorGenerationState state = VectorGenerationState::requested;
  bool visible = false;
  bool persisted_record_present = false;
  bool checksum_valid = true;
  bool complete = false;
  bool stale = false;
  bool training_evidence_present = false;
  bool validation_evidence_present = false;
  bool sealed_generation_evidence_present = false;
  bool recall_contract_evidence_present = false;
  bool publish_barrier_evidence_present = false;
  bool publish_barrier_engine_owned_mga = false;
  std::string training_evidence_ref;
  std::string validation_evidence_ref;
  std::string sealed_generation_evidence_ref;
  std::string recall_contract_evidence_ref;
  std::string publish_barrier_evidence_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  std::string authority_source = kVectorGenerationAuthoritySource;
  VectorGenerationResourceEnvelope resource_envelope;
  VectorGenerationRecallContract recall_contract;
  bool parser_finality_authority_claimed = false;
  bool client_finality_authority_claimed = false;
  bool timestamp_finality_authority_claimed = false;
  bool uuid_ordering_finality_authority_claimed = false;
  bool event_stream_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct VectorGenerationEvidenceRow {
  u64 sequence = 0;
  TypedUuid evidence_id;
  TypedUuid generation_uuid;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  VectorGenerationState state = VectorGenerationState::requested;
  u64 generation = 0;
  bool visible = false;
  bool validation_evidence_present = false;
  bool sealed_generation_evidence_present = false;
  bool recall_contract_evidence_present = false;
  bool publish_barrier_evidence_present = false;
  bool publish_barrier_engine_owned_mga = false;
  u64 memory_limit_bytes = 0;
  u64 memory_observed_bytes = 0;
  u64 temp_space_limit_bytes = 0;
  u64 temp_space_observed_bytes = 0;
  u32 worker_limit = 0;
  u32 workers_used = 0;
  bool resource_governor_evidence_present = false;
  std::string authority_source = kVectorGenerationAuthoritySource;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
  bool donor_authority = false;
  bool write_ahead_authority = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct VectorGenerationLedger {
  std::vector<VectorGenerationDescriptor> generations;
  std::vector<VectorGenerationEvidenceRow> evidence;
  u64 next_evidence_sequence = 1;
  u64 ledger_generation = 1;
};

struct VectorGenerationRequest {
  TypedUuid generation_uuid;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  u64 generation = 1;
  IndexVectorAlgorithm algorithm = IndexVectorAlgorithm::hnsw;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  VectorGenerationResourceEnvelope resource_envelope;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
  bool donor_authority = false;
  bool write_ahead_authority = false;
};

struct VectorGenerationTrainingRequest {
  std::string training_evidence_ref;
  bool training_succeeded = false;
  bool complete_training_set = false;
};

struct VectorGenerationValidationRequest {
  std::string validation_evidence_ref;
  bool validation_succeeded = false;
  bool complete_generation = false;
};

struct VectorGenerationSealRequest {
  std::string sealed_generation_evidence_ref;
  VectorGenerationRecallContract recall_contract;
  bool sealed_bytes_complete = false;
};

struct VectorGenerationPublishRequest {
  std::string publish_barrier_evidence_ref;
  bool engine_owned_mga_publish_barrier = false;
  std::string authority_source = kVectorGenerationAuthoritySource;
};

struct VectorGenerationLifecycleResult {
  Status status;
  bool accepted = false;
  bool fail_closed = true;
  VectorGenerationDescriptor generation;
  VectorGenerationEvidenceRow evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted && !fail_closed; }
};

struct VectorGenerationAccessRequest {
  std::vector<VectorGenerationDescriptor> generations;
  bool vector_generation_enabled = true;
  bool exact_vector_scan_fallback_available = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
};

struct VectorGenerationAccessPlan {
  Status status;
  DiagnosticRecord diagnostic;
  IndexPlanCategory selected_category = IndexPlanCategory::fallback_full_scan;
  std::string selected_access = "exact_vector_scan";
  std::string fallback_reason = "none";
  std::string authority_source = kVectorGenerationAuthoritySource;
  bool ann_generation_selected = false;
  bool exact_vector_scan_fallback_selected = true;
  bool exact_vector_scan_fallback_available = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool generation_metadata_visibility_authority = false;
  bool generation_metadata_finality_authority = false;
  std::vector<TypedUuid> published_generation_uuids;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && ann_generation_selected; }
};

struct VectorGenerationRecoveryResult {
  Status status;
  bool fail_closed = true;
  VectorGenerationRecoveryClass recovery_class =
      VectorGenerationRecoveryClass::unsafe_visible_generation_refused;
  VectorGenerationRecoveryAction action =
      VectorGenerationRecoveryAction::refuse_unsafe_generation_use;
  std::string stable_reason;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* VectorGenerationStateName(VectorGenerationState state);
const char* VectorGenerationFallbackReasonName(
    VectorGenerationFallbackReason reason);
const char* VectorGenerationRecoveryClassName(
    VectorGenerationRecoveryClass recovery_class);
const char* VectorGenerationRecoveryActionName(
    VectorGenerationRecoveryAction action);

bool VectorGenerationResourceEnvelopeBounded(
    const VectorGenerationResourceEnvelope& envelope);
bool VectorGenerationRecallContractSatisfied(
    const VectorGenerationRecallContract& contract);
bool VectorGenerationDescriptorIdentityValid(
    const VectorGenerationDescriptor& generation);
bool VectorGenerationDescriptorAuthorityClean(
    const VectorGenerationDescriptor& generation);
bool VectorGenerationDescriptorUsable(
    const VectorGenerationDescriptor& generation);

VectorGenerationLifecycleResult RequestVectorGeneration(
    VectorGenerationLedger* ledger,
    const VectorGenerationRequest& request);
VectorGenerationLifecycleResult StartVectorGenerationBuild(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation);
VectorGenerationLifecycleResult MarkVectorGenerationTrained(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationTrainingRequest& request);
VectorGenerationLifecycleResult ValidateVectorGeneration(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationValidationRequest& request);
VectorGenerationLifecycleResult SealVectorGeneration(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationSealRequest& request);
VectorGenerationLifecycleResult PublishVectorGeneration(
    VectorGenerationLedger* ledger,
    VectorGenerationDescriptor* generation,
    const VectorGenerationPublishRequest& request);

VectorGenerationAccessPlan PlanVectorGenerationAccess(
    const VectorGenerationAccessRequest& request);
VectorGenerationRecoveryResult ClassifyVectorGenerationReopen(
    const VectorGenerationLedger& ledger,
    bool exact_vector_scan_fallback_available);
std::vector<std::string> VectorGenerationRecallLifecycleEvidenceAdapter(
    const VectorTrainingRecallLifecycleDecision& decision);
DiagnosticRecord MakeVectorGenerationDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::core::index
