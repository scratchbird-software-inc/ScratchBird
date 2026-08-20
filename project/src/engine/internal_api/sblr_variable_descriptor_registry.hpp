// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SBLR-VARIABLE-DESCRIPTOR-REGISTRY-V1
enum class SblrVariableValueState : std::uint8_t {
  value = 1,
  null_value = 2,
  uninitialized = 3,
};
enum class SblrVariableMutability : std::uint8_t { immutable = 0, mutable_value = 1 };
enum class SblrVariableLifecycle : std::uint8_t { active = 1, revoked = 2 };

struct SblrVariableDescriptorRow {
  std::string variable_descriptor_uuid;
  std::uint64_t variable_descriptor_generation{0};
  std::string statement_receipt_uuid;
  std::string database_uuid;
  std::string session_uuid;
  std::string transaction_uuid;
  std::string scope_uuid;
  std::uint64_t scope_generation{0};
  std::string frame_uuid;
  std::uint64_t frame_generation{0};
  std::uint32_t variable_ordinal{0};
  std::string datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation{0};
  bool nullable{false};
  SblrVariableMutability mutability{SblrVariableMutability::immutable};
  std::uint64_t value_generation{0};
  SblrVariableValueState value_state{SblrVariableValueState::uninitialized};
  std::string canonical_value_bytes;
  std::string canonical_value_sha256;
  std::string row_identity_sha256;
  std::uint64_t registry_generation{0};
  SblrVariableLifecycle lifecycle{SblrVariableLifecycle::revoked};
  std::string decision_evidence_sha256;
};

struct SblrVariableDemand {
  std::string datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation{0};
  bool nullable{false};
  SblrVariableMutability mutability{SblrVariableMutability::immutable};
  SblrVariableValueState initial_state{SblrVariableValueState::uninitialized};
  std::string canonical_value_bytes;
};

struct SblrVariableRegistryResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrVariableDescriptorRow row;
  std::vector<SblrVariableDescriptorRow> rows;
  std::vector<EngineEvidenceReference> evidence;
};

struct SblrVariableAssignment {
  std::string variable_descriptor_uuid;
  std::uint64_t variable_descriptor_generation{0};
  std::uint64_t expected_value_generation{0};
  SblrVariableValueState value_state{SblrVariableValueState::uninitialized};
  std::string canonical_value_bytes;
};

SblrVariableRegistryResult PublishSblrVariableFrame(
    const EngineRequestContext& context,
    const std::string& statement_receipt_uuid,
    const std::string& scope_uuid, std::uint64_t scope_generation,
    const std::string& frame_uuid, std::uint64_t frame_generation,
    const std::vector<SblrVariableDemand>& demands);

SblrVariableRegistryResult LookupSblrVariable(
    const EngineRequestContext& context,
    const std::string& statement_receipt_uuid,
    const std::string& scope_uuid, std::uint64_t scope_generation,
    const std::string& frame_uuid, std::uint64_t frame_generation,
    const std::string& variable_descriptor_uuid,
    std::uint64_t variable_descriptor_generation,
    std::uint64_t expected_value_generation);

SblrVariableRegistryResult AssignSblrVariable(
    const EngineRequestContext& context,
    const std::string& statement_receipt_uuid,
    const std::string& scope_uuid, std::uint64_t scope_generation,
    const std::string& frame_uuid, std::uint64_t frame_generation,
    const std::string& variable_descriptor_uuid,
    std::uint64_t variable_descriptor_generation,
    std::uint64_t expected_value_generation,
    SblrVariableValueState value_state,
    const std::string& canonical_value_bytes);

// Atomically publishes one evidence-first durable batch. No in-memory row is
// changed unless the complete batch reaches the durable journal.
SblrVariableRegistryResult AssignSblrVariableBatch(
    const EngineRequestContext& context,
    const std::string& statement_receipt_uuid,
    const std::string& scope_uuid, std::uint64_t scope_generation,
    const std::string& frame_uuid, std::uint64_t frame_generation,
    const std::vector<SblrVariableAssignment>& assignments);

EngineApiDiagnostic RevokeSblrVariableFrame(
    const EngineRequestContext& context,
    const std::string& statement_receipt_uuid,
    const std::string& scope_uuid, std::uint64_t scope_generation,
    const std::string& frame_uuid, std::uint64_t frame_generation,
    const std::string& reason_code);

EngineApiDiagnostic RecoverSblrVariableDescriptorRegistry(
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
