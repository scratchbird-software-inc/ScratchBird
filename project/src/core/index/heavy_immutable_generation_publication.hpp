// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ODF-045_HEAVY_IMMUTABLE_GENERATION_PUBLICATION
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u64;

inline constexpr const char* kHeavyImmutableGenerationPublicationSearchKey =
    "ODF-045_HEAVY_IMMUTABLE_GENERATION_PUBLICATION";
inline constexpr const char* kHeavyImmutableGenerationAuthoritySource =
    "engine_mga_heavy_immutable_generation_publication";

enum class HeavyImmutableGenerationState : scratchbird::core::platform::u32 {
  requested = 1,
  validated = 2,
  published = 3,
  refused = 4
};

struct HeavyImmutableGenerationIdentity {
  TypedUuid generation_uuid;
  TypedUuid table_or_collection_uuid;
  TypedUuid transaction_uuid;
  std::string family;
  std::string profile;
};

struct HeavyImmutableGenerationValidationRequest {
  HeavyImmutableGenerationIdentity identity;
  u64 source_row_count = 0;
  u64 source_payload_count = 0;
  bool source_row_count_present = false;
  bool source_payload_count_present = false;
  bool validation_succeeded = false;
  bool immutable_payload_complete = false;
  bool source_counts_verified = false;
  bool checksum_verified = false;
  std::string validation_proof_ref;
  std::string engine_mga_authority_evidence_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string operation_id;
  std::vector<std::string> exact_diagnostics;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
  bool reference_authority = false;
  bool write_ahead_authority = false;
  bool cluster_provider_routed = false;
};

struct HeavyImmutableGenerationPublicationRequest {
  std::string publication_fence_ref;
  bool engine_owned_mga_publication_fence = false;
  std::string authority_source = kHeavyImmutableGenerationAuthoritySource;
  bool cluster_provider_routed = false;
};

struct HeavyImmutableGenerationDescriptor {
  HeavyImmutableGenerationIdentity identity;
  HeavyImmutableGenerationState state = HeavyImmutableGenerationState::requested;
  bool visible = false;
  bool persisted_descriptor_present = false;
  u64 source_row_count = 0;
  u64 source_payload_count = 0;
  bool validation_proof_present = false;
  bool validation_succeeded = false;
  bool immutable_payload_complete = false;
  bool source_counts_verified = false;
  bool checksum_verified = false;
  bool publication_fence_present = false;
  bool engine_owned_mga_publication_fence = false;
  bool engine_mga_finality_authority_present = false;
  bool generation_finality_authority = false;
  bool generation_visibility_authority = false;
  bool cluster_provider_routed = false;
  std::string validation_proof_ref;
  std::string publication_fence_ref;
  std::string engine_mga_authority_evidence_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string authority_source = kHeavyImmutableGenerationAuthoritySource;
  std::string operation_id;
  std::vector<std::string> exact_diagnostics;
};

struct HeavyImmutableGenerationEvidenceRow {
  u64 sequence = 0;
  TypedUuid evidence_id;
  HeavyImmutableGenerationIdentity identity;
  HeavyImmutableGenerationState state = HeavyImmutableGenerationState::requested;
  bool visible = false;
  u64 source_row_count = 0;
  u64 source_payload_count = 0;
  bool validation_proof_present = false;
  bool publication_fence_present = false;
  bool engine_owned_mga_publication_fence = false;
  bool engine_mga_finality_authority_present = false;
  bool generation_finality_authority = false;
  bool generation_visibility_authority = false;
  bool cluster_provider_routed = false;
  std::string validation_proof_ref;
  std::string publication_fence_ref;
  std::string engine_mga_authority_evidence_ref;
  std::string authority_source = kHeavyImmutableGenerationAuthoritySource;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> exact_diagnostics;
};

struct HeavyImmutableGenerationLedger {
  std::vector<HeavyImmutableGenerationDescriptor> generations;
  std::vector<HeavyImmutableGenerationEvidenceRow> evidence;
  u64 next_evidence_sequence = 1;
  u64 ledger_generation = 1;
};

struct HeavyImmutableGenerationResult {
  Status status;
  bool accepted = false;
  bool fail_closed = true;
  HeavyImmutableGenerationDescriptor generation;
  HeavyImmutableGenerationEvidenceRow evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && accepted && !fail_closed;
  }
};

const char* HeavyImmutableGenerationStateName(
    HeavyImmutableGenerationState state);

bool HeavyImmutableGenerationIdentityValid(
    const HeavyImmutableGenerationIdentity& identity);
bool HeavyImmutableGenerationValidated(
    const HeavyImmutableGenerationDescriptor& generation);
bool HeavyImmutableGenerationPublishable(
    const HeavyImmutableGenerationDescriptor& generation);
bool HeavyImmutableGenerationPublished(
    const HeavyImmutableGenerationDescriptor& generation);

HeavyImmutableGenerationResult ValidateHeavyImmutableGeneration(
    HeavyImmutableGenerationLedger* ledger,
    const HeavyImmutableGenerationValidationRequest& request);
HeavyImmutableGenerationResult PublishHeavyImmutableGeneration(
    HeavyImmutableGenerationLedger* ledger,
    HeavyImmutableGenerationDescriptor* generation,
    const HeavyImmutableGenerationPublicationRequest& request);
DiagnosticRecord MakeHeavyImmutableGenerationDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
