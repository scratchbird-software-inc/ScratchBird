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
  std::string slot_uuid;
  std::string datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation{0};
  SblrParameterDirection direction{SblrParameterDirection::in};
  bool nullable{false};
};

struct SblrParameterSetSnapshot {
  std::string snapshot_uuid;
  std::uint64_t snapshot_generation{0};
  std::string database_uuid;
  std::string session_uuid;
  std::string statement_receipt_uuid;
  std::string execution_uuid;
  std::string parameter_set_descriptor_uuid;
  std::uint64_t descriptor_generation{0};
  std::string prepared_statement_uuid;
  std::uint64_t prepared_generation{0};
  std::string batch_uuid;
  std::uint64_t batch_generation{0};
  std::string dynamic_package_uuid;
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
  std::string datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation{0};
  SblrParameterDirection direction{SblrParameterDirection::in};
  bool nullable{false};
};

struct SblrParameterSetIssueRequest {
  std::string statement_receipt_uuid;
  std::string execution_uuid;
  std::string prepared_statement_uuid;
  std::uint64_t prepared_generation{0};
  std::string batch_uuid;
  std::uint64_t batch_generation{0};
  std::string dynamic_package_uuid;
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

SblrParameterSetMutationResult IssueSblrParameterSet(
    const EngineRequestContext& context,
    const SblrParameterSetIssueRequest& request);

SblrParameterSetLoadResult LoadSblrParameterSet(
    const EngineRequestContext& context,
    const std::string& parameter_set_descriptor_uuid);

// Startup/recovery boundary: durable descriptor metadata remains loadable,
// while every execution/receipt authorization is revoked and must be reissued.
EngineApiDiagnostic BeginSblrParameterSetRegistryRecovery(
    const EngineRequestContext& context);

SblrParameterSetMutationResult InvalidateSblrParameterSet(
    const EngineRequestContext& context,
    const std::string& parameter_set_descriptor_uuid,
    const std::string& expected_snapshot_uuid,
    std::uint64_t expected_snapshot_generation,
    const std::string& reason_code);

// Revalidates all immutable receipt/execution/prepared/batch/dynamic and epoch
// bindings. No zero/nonzero identity pair is inferred or normalized.
EngineApiDiagnostic RevalidateSblrParameterSet(
    const EngineRequestContext& context,
    const SblrParameterSetSnapshot& admitted,
    const std::string& statement_receipt_uuid,
    const std::string& execution_uuid,
    const std::string& prepared_statement_uuid,
    std::uint64_t prepared_generation,
    const std::string& batch_uuid,
    std::uint64_t batch_generation,
    const std::string& dynamic_package_uuid,
    std::uint64_t dynamic_generation,
    SblrParameterSetSnapshot* current);

}  // namespace scratchbird::engine::internal_api
