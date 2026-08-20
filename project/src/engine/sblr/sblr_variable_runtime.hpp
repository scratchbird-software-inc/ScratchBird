// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using SblrVariableUuidV1 = std::array<std::uint8_t, 16>;
using SblrVariableSha256V1 = std::array<std::uint8_t, 32>;

struct SblrVariableNodeV1 {
  std::uint64_t node_id = 0;
  std::uint32_t parent_operand_ordinal = 0;
  SblrVariableUuidV1 scope_uuid{};
  std::uint64_t scope_generation = 0;
  SblrVariableUuidV1 frame_uuid{};
  std::uint64_t frame_generation = 0;
  SblrVariableUuidV1 variable_descriptor_uuid{};
  std::uint64_t variable_descriptor_generation = 0;
  SblrVariableUuidV1 datatype_descriptor_uuid{};
  std::uint64_t datatype_descriptor_generation = 0;
  std::uint64_t value_generation = 0;
  std::uint8_t value_state_policy = 0;
};
struct SblrVariableNodeTableV1 { std::vector<SblrVariableNodeV1> nodes; };
struct SblrVariableNodeTableCodecResultV1 {
  bool ok = false;
  std::string diagnostic_id = "SBLR.OPERAND_INVALID", detail;
  SblrVariableNodeTableV1 table;
  std::vector<std::uint8_t> canonical_bytes;
};
std::vector<std::uint8_t> EncodeSblrVariableNodeTableV1(
    const SblrVariableNodeTableV1&);
SblrVariableNodeTableCodecResultV1 DecodeSblrVariableNodeTableV1(
    const std::uint8_t*, std::size_t);

struct SblrVariableNodeReferenceV1 {
  std::uint32_t occurrence_ordinal = 0;
  std::uint64_t node_id = 0;
  SblrVariableSha256V1 table_sha256{};
  SblrVariableUuidV1 scope_uuid{};
  std::uint64_t scope_generation = 0;
  SblrVariableUuidV1 frame_uuid{};
  std::uint64_t frame_generation = 0;
  SblrVariableUuidV1 variable_descriptor_uuid{};
  std::uint64_t variable_descriptor_generation = 0;
  std::uint64_t value_generation = 0;
};
std::vector<std::uint8_t> EncodeSblrVariableNodeReferenceV1(
    const SblrVariableNodeReferenceV1&);
bool DecodeSblrVariableNodeReferenceV1(const std::uint8_t*, std::size_t,
                                       SblrVariableNodeReferenceV1*);
bool ValidateSblrVariableReferenceBijectionV1(
    const SblrVariableNodeTableCodecResultV1&,
    const std::vector<SblrVariableNodeReferenceV1>&);

struct SblrVariableDemandV1 {
  std::uint64_t occurrence_id = 0;
  std::uint32_t parent_operand_ordinal = 0, variable_ordinal = 0;
  SblrVariableUuidV1 scope_uuid{};
};
struct SblrVariableFrameDemandV1 {
  std::uint64_t declaration_occurrence_id = 0;
  std::uint16_t datatype_context_code = 0;
  std::uint8_t nullable = 0, mutability = 0, initial_value_state = 0;
  SblrVariableSha256V1 declaration_token_sha256{};
};
struct SblrVariableFrameBeginRequestV1 {
  SblrVariableUuidV1 operation_uuid{}, transaction_uuid{};
  std::uint64_t expires_after_ns = 0;
  std::vector<SblrVariableFrameDemandV1> demands;
  SblrVariableSha256V1 demand_sha256{};
};
struct SblrVariableFrameMappingV1 {
  std::uint64_t declaration_occurrence_id = 0;
  std::uint32_t variable_ordinal = 0;
  std::uint8_t nullable = 0, mutability = 0, value_state = 0;
  SblrVariableUuidV1 variable_descriptor_uuid{}, datatype_descriptor_uuid{},
      datatype_type_uuid{};
  std::uint64_t variable_descriptor_generation = 0,
      datatype_descriptor_generation = 0, value_generation = 0;
};
struct SblrVariableFrameBeginResultV1 {
  SblrVariableUuidV1 public_coordination_uuid{}, operation_uuid{}, scope_uuid{},
      frame_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0,
      registry_generation = 0, coordinator_generation = 0;
  std::vector<SblrVariableFrameMappingV1> mappings;
  SblrVariableSha256V1 mapping_sha256{};
};
struct SblrVariableFrameCloseRequestV1 {
  SblrVariableUuidV1 public_coordination_uuid{}, operation_uuid{};
  std::uint64_t expected_frame_generation = 0;
  std::uint32_t reason_code = 0;
};
struct SblrVariableFrameCloseResultV1 {
  SblrVariableUuidV1 public_coordination_uuid{};
  std::uint64_t revoked_frame_generation = 0,
      decision_evidence_generation = 0;
};
struct SblrVariableNegotiateRequestV1 {
  SblrVariableUuidV1 preliminary_receipt_uuid{}, scope_uuid{}, frame_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0;
  std::vector<SblrVariableDemandV1> demands;
  SblrVariableSha256V1 demand_sha256{};
};
struct SblrVariableMappingV1 {
  std::uint64_t occurrence_id = 0;
  std::uint32_t variable_ordinal = 0;
  std::uint8_t nullable = 0, mutability = 0, value_state = 0;
  SblrVariableUuidV1 variable_descriptor_uuid{}, datatype_descriptor_uuid{},
      datatype_type_uuid{};
  std::uint64_t variable_descriptor_generation = 0,
      datatype_descriptor_generation = 0, value_generation = 0;
};
struct SblrVariableNegotiateResultV1 {
  SblrVariableUuidV1 preliminary_receipt_uuid{}, scope_uuid{}, frame_uuid{},
      registry_snapshot_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0,
      registry_generation = 0;
  std::vector<SblrVariableMappingV1> mappings;
  SblrVariableSha256V1 mapping_sha256{};
};
struct SblrVariableFinalizeRequestV1 {
  SblrVariableUuidV1 preliminary_receipt_uuid{}, scope_uuid{}, frame_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0,
      registry_generation = 0;
  SblrVariableSha256V1 demand_sha256{}, mapping_sha256{}, sbvn_sha256{};
  std::vector<std::uint8_t> canonical_sbvn;
};
struct SblrVariableAdmissionV1 {
  SblrVariableUuidV1 final_receipt_uuid{}, admission_token_uuid{}, scope_uuid{},
      frame_uuid{}, registry_snapshot_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0,
      registry_generation = 0, executor_availability_generation = 0,
      expires_at_monotonic_ns = 0;
  SblrVariableSha256V1 binding_sha256{};
};
struct SblrVariableExecutionBindingV1 {
  SblrVariableUuidV1 execution_uuid{}, statement_receipt_uuid{},
      variable_final_receipt_uuid{}, admission_token_uuid{}, scope_uuid{},
      frame_uuid{}, registry_snapshot_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0,
      registry_generation = 0, executor_availability_generation = 0;
  SblrVariableSha256V1 binding_sha256{};
};
struct SblrVariableAssignmentRecordV1 {
  std::uint64_t assignment_occurrence_id = 0;
  std::uint32_t variable_ordinal = 0;
  SblrVariableUuidV1 variable_descriptor_uuid{}, datatype_descriptor_uuid{};
  std::uint64_t variable_descriptor_generation = 0,
      expected_value_generation = 0, datatype_descriptor_generation = 0;
  std::uint8_t value_state = 0;
  SblrVariableSha256V1 canonical_value_sha256{};
  std::vector<std::uint8_t> canonical_value_bytes;
};
struct SblrVariableAssignmentRequestV1 {
  SblrVariableUuidV1 preliminary_receipt_uuid{}, public_coordination_uuid{},
      operation_uuid{}, scope_uuid{}, frame_uuid{},
      registry_snapshot_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0,
      registry_generation = 0;
  SblrVariableSha256V1 assignment_sha256{};
  std::vector<SblrVariableAssignmentRecordV1> assignments;
};
struct SblrVariableAssignmentResultRecordV1 {
  std::uint64_t assignment_occurrence_id = 0;
  std::uint32_t variable_ordinal = 0;
  SblrVariableUuidV1 variable_descriptor_uuid{};
  std::uint64_t variable_descriptor_generation = 0,
      new_value_generation = 0, decision_evidence_generation = 0;
};
struct SblrVariableAssignmentResultV1 {
  SblrVariableUuidV1 preliminary_receipt_uuid{}, public_coordination_uuid{},
      scope_uuid{}, frame_uuid{};
  std::uint64_t scope_generation = 0, frame_generation = 0,
      new_registry_generation = 0;
  SblrVariableSha256V1 result_sha256{};
  std::vector<SblrVariableAssignmentResultRecordV1> results;
};

SblrVariableSha256V1 ComputeSblrVariableDemandSha256V1(
    const std::vector<SblrVariableDemandV1>&);
SblrVariableSha256V1 ComputeSblrVariableMappingSha256V1(
    const std::vector<SblrVariableMappingV1>&);
SblrVariableSha256V1 ComputeSblrVariableFrameDemandSha256V1(
    const std::vector<SblrVariableFrameDemandV1>&);
SblrVariableSha256V1 ComputeSblrVariableFrameMappingSha256V1(
    const std::vector<SblrVariableFrameMappingV1>&);
std::vector<std::uint8_t> EncodeSblrVariableFrameBeginRequestV1(
    SblrVariableFrameBeginRequestV1*);
bool DecodeSblrVariableFrameBeginRequestV1(
    const std::uint8_t*, std::size_t, SblrVariableFrameBeginRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrVariableFrameBeginResultV1(
    SblrVariableFrameBeginResultV1*);
bool DecodeSblrVariableFrameBeginResultV1(
    const std::uint8_t*, std::size_t, SblrVariableFrameBeginResultV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrVariableFrameCloseRequestV1(
    const SblrVariableFrameCloseRequestV1&);
bool DecodeSblrVariableFrameCloseRequestV1(
    const std::uint8_t*, std::size_t, SblrVariableFrameCloseRequestV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrVariableFrameCloseResultV1(
    const SblrVariableFrameCloseResultV1&);
bool DecodeSblrVariableFrameCloseResultV1(
    const std::uint8_t*, std::size_t, SblrVariableFrameCloseResultV1*,
    std::string*);
std::vector<std::uint8_t> EncodeSblrVariableNegotiateRequestV1(
    SblrVariableNegotiateRequestV1*);
bool DecodeSblrVariableNegotiateRequestV1(const std::uint8_t*, std::size_t,
                                          SblrVariableNegotiateRequestV1*,
                                          std::string*);
std::vector<std::uint8_t> EncodeSblrVariableNegotiateResultV1(
    SblrVariableNegotiateResultV1*);
bool DecodeSblrVariableNegotiateResultV1(const std::uint8_t*, std::size_t,
                                         SblrVariableNegotiateResultV1*,
                                         std::string*);
std::vector<std::uint8_t> EncodeSblrVariableFinalizeRequestV1(
    const SblrVariableFinalizeRequestV1&);
bool DecodeSblrVariableFinalizeRequestV1(const std::uint8_t*, std::size_t,
                                         SblrVariableFinalizeRequestV1*,
                                         std::string*);
std::vector<std::uint8_t> EncodeSblrVariableAdmissionV1(
    SblrVariableAdmissionV1*);
bool DecodeSblrVariableAdmissionV1(const std::uint8_t*, std::size_t,
                                   SblrVariableAdmissionV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrVariableExecutionBindingV1(
    const SblrVariableExecutionBindingV1&);
bool DecodeSblrVariableExecutionBindingV1(const std::uint8_t*, std::size_t,
                                          SblrVariableExecutionBindingV1*,
                                          std::string*);
std::vector<std::uint8_t> EncodeSblrVariableAssignmentRequestV1(
    SblrVariableAssignmentRequestV1*);
bool DecodeSblrVariableAssignmentRequestV1(const std::uint8_t*, std::size_t,
                                           SblrVariableAssignmentRequestV1*,
                                           std::string*);
std::vector<std::uint8_t> EncodeSblrVariableAssignmentResultV1(
    SblrVariableAssignmentResultV1*);
bool DecodeSblrVariableAssignmentResultV1(const std::uint8_t*, std::size_t,
                                          SblrVariableAssignmentResultV1*,
                                          std::string*);

}  // namespace scratchbird::engine::sblr
