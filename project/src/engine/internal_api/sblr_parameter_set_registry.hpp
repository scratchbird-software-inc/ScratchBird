// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class SblrParameterDirection : std::uint8_t { in = 1, out = 2, inout = 3 };
enum class SblrParameterSetState : std::uint8_t { active = 1, revoked = 2 };

struct SblrParameterSlotDescriptor {
  std::uint32_t slot_ordinal{0};
  EngineUuid slot_uuid;
  EngineUuid datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation{0};
  SblrParameterDirection direction{SblrParameterDirection::in};
  bool nullable{false};
};

struct SblrParameterSetSnapshot {
  EngineUuid snapshot_uuid;
  std::uint64_t snapshot_generation{0};
  EngineUuid database_uuid;
  EngineUuid session_uuid;
  EngineUuid statement_receipt_uuid;
  EngineUuid execution_uuid;
  EngineUuid parameter_set_descriptor_uuid;
  std::uint64_t descriptor_generation{0};
  EngineUuid prepared_statement_uuid;
  std::uint64_t prepared_generation{0};
  EngineUuid batch_uuid;
  std::uint64_t batch_generation{0};
  EngineUuid dynamic_package_uuid;
  std::uint64_t dynamic_generation{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t resource_epoch{0};
  SblrParameterSetState state{SblrParameterSetState::revoked};
  std::vector<SblrParameterSlotDescriptor> slots;
  std::string slots_sha256;
  std::string decision_evidence_sha256;
};

struct SblrParameterSlotIssueDemand {
  EngineUuid datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation{0};
  SblrParameterDirection direction{SblrParameterDirection::in};
  bool nullable{false};
};

struct SblrParameterSetIssueRequest {
  EngineUuid statement_receipt_uuid;
  EngineUuid execution_uuid;
  EngineUuid prepared_statement_uuid;
  std::uint64_t prepared_generation{0};
  EngineUuid batch_uuid;
  std::uint64_t batch_generation{0};
  EngineUuid dynamic_package_uuid;
  std::uint64_t dynamic_generation{0};
  std::vector<SblrParameterSlotIssueDemand> slots;
  std::string reason_code;
};

struct SblrParameterSetLoadResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrParameterSetSnapshot snapshot;
};

struct SblrParameterSetMutationResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrParameterSetSnapshot snapshot;
  std::vector<EngineEvidenceReference> evidence;
};

// Durable publication owned by SBLR_PARAMETER_BIND.  The canonical value
// vector remains engine-private and is never copied into diagnostics or the
// public result.  Every identity here is an exact projection from an
// authenticated statement receipt or an already-issued parameter-set row.
struct SblrParameterBindPublicationRequest {
  EngineUuid statement_receipt_uuid;
  EngineUuid execution_uuid;
  EngineUuid prepared_statement_uuid;
  std::uint64_t prepared_generation{0};
  EngineUuid parameter_set_descriptor_uuid;
  std::uint64_t parameter_set_generation{0};
  std::string ordered_slot_table_sha256;
  EngineUuid batch_uuid;
  std::uint64_t batch_generation{0};
  EngineUuid dynamic_package_uuid;
  std::uint64_t dynamic_generation{0};
  EngineUuid catalog_snapshot_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t resource_epoch{0};
  EngineUuid mga_snapshot_uuid;
  std::uint64_t executor_availability_generation{0};
  std::vector<std::uint8_t> canonical_value_vector;
  std::string value_vector_sha256;
};

struct SblrParameterBindPublicationSnapshot {
  EngineUuid database_uuid;
  EngineUuid session_uuid;
  EngineUuid statement_receipt_uuid;
  EngineUuid execution_uuid;
  EngineUuid prepared_statement_uuid;
  std::uint64_t prepared_generation{0};
  EngineUuid parameter_set_descriptor_uuid;
  std::uint64_t parameter_set_generation{0};
  std::string ordered_slot_table_sha256;
  EngineUuid batch_uuid;
  std::uint64_t batch_generation{0};
  EngineUuid dynamic_package_uuid;
  std::uint64_t dynamic_generation{0};
  EngineUuid catalog_snapshot_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t resource_epoch{0};
  EngineUuid mga_snapshot_uuid;
  std::uint64_t executor_availability_generation{0};
  std::string value_vector_sha256;
  EngineUuid bind_evidence_uuid;
  std::string publication_evidence_sha256;
  std::vector<std::uint8_t> canonical_value_vector;
};

struct SblrParameterBindPublicationResult {
  bool ok{false};
  bool replayed{false};
  EngineApiDiagnostic diagnostic;
  SblrParameterBindPublicationSnapshot snapshot;
  std::vector<EngineEvidenceReference> evidence;
};

SblrParameterSetMutationResult IssueSblrParameterSet(
    const EngineRequestContext& context,
    const SblrParameterSetIssueRequest& request);

SblrParameterSetLoadResult LoadSblrParameterSet(
    const EngineRequestContext& context,
    const EngineUuid& parameter_set_descriptor_uuid);

// Atomically publishes the canonical value vector for one active parameter
// set.  Exact replay returns the original durable evidence; any drift is a
// stale conflict and can never replace the first publication.
SblrParameterBindPublicationResult PublishSblrParameterBinding(
    const EngineRequestContext& context,
    const SblrParameterSetSnapshot& admitted_parameter_set,
    const SblrParameterBindPublicationRequest& request);

SblrParameterBindPublicationResult LoadSblrParameterBinding(
    const EngineRequestContext& context,
    const EngineUuid& parameter_set_descriptor_uuid);

// Startup/recovery boundary: durable descriptor metadata remains loadable,
// while every execution/receipt authorization is revoked and must be reissued.
EngineApiDiagnostic BeginSblrParameterSetRegistryRecovery(
    const EngineRequestContext& context);

SblrParameterSetMutationResult InvalidateSblrParameterSet(
    const EngineRequestContext& context,
    const EngineUuid& parameter_set_descriptor_uuid,
    const EngineUuid& expected_snapshot_uuid,
    std::uint64_t expected_snapshot_generation,
    const std::string& reason_code);

// Revalidates all immutable receipt/execution/prepared/batch/dynamic and epoch
// bindings. No zero/nonzero identity pair is inferred or normalized.
EngineApiDiagnostic RevalidateSblrParameterSet(
    const EngineRequestContext& context,
    const SblrParameterSetSnapshot& admitted,
    const EngineUuid& statement_receipt_uuid,
    const EngineUuid& execution_uuid,
    const EngineUuid& prepared_statement_uuid,
    std::uint64_t prepared_generation,
    const EngineUuid& batch_uuid,
    std::uint64_t batch_generation,
    const EngineUuid& dynamic_package_uuid,
    std::uint64_t dynamic_generation,
    SblrParameterSetSnapshot* current);

}  // namespace scratchbird::engine::internal_api
