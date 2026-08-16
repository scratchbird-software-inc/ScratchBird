#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kSblrLiteralExecutorId = "engine.op.literal";
inline constexpr std::uint16_t kSblrLiteralOpcodeCode = 3;
inline constexpr const char* kSblrLiteralOpcodeVersion = "1.0";
inline constexpr const char* kSblrLiteralOperandDescriptorId = "typed_literal";
inline constexpr const char* kSblrLiteralResultDescriptorId = "typed_value";
inline constexpr std::uint16_t kSblrLiteralResultDescriptorVersion = 1;
inline constexpr const char* kSblrParameterExecutorId = "engine.op.parameter";
inline constexpr std::uint16_t kSblrParameterOpcodeCode = 4;
inline constexpr const char* kSblrParameterOpcodeVersion = "1.0";
inline constexpr const char* kSblrParameterOperandDescriptorId =
    "parameter_descriptor_ref";
inline constexpr const char* kSblrParameterResultDescriptorId = "typed_value";
inline constexpr std::uint16_t kSblrParameterResultDescriptorVersion = 1;

enum class SblrExecutorAvailabilityState : std::uint8_t {
  installed = 1,
  revoked = 2,
  unavailable = 3,
};

struct SblrExecutorAvailabilityRowIdentity {
  std::string executor_id{kSblrLiteralExecutorId};
  std::uint16_t opcode_code{kSblrLiteralOpcodeCode};
  std::string opcode_version{kSblrLiteralOpcodeVersion};
  std::string operand_descriptor_id{kSblrLiteralOperandDescriptorId};
  std::string result_descriptor_id{kSblrLiteralResultDescriptorId};
  std::uint16_t result_descriptor_version{kSblrLiteralResultDescriptorVersion};
};

struct SblrExecutorAvailabilitySnapshot {
  std::string snapshot_uuid;
  std::uint64_t generation{0};
  std::string database_uuid;
  std::string row_identity_sha256;
  bool installed{false};
  SblrExecutorAvailabilityState availability_state{
      SblrExecutorAvailabilityState::unavailable};
  std::string decision_evidence_sha256;
};

struct SblrExecutorAvailabilityLoadResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrExecutorAvailabilitySnapshot snapshot;
};

struct SblrExecutorAvailabilitySetRequest {
  std::string database_uuid;
  std::string expected_snapshot_uuid;
  std::uint64_t expected_generation{0};
  SblrExecutorAvailabilityRowIdentity exact_row_identity;
  SblrExecutorAvailabilityState requested_state{
      SblrExecutorAvailabilityState::unavailable};
  std::string reason_code;
};

struct SblrExecutorAvailabilitySetResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrExecutorAvailabilitySnapshot snapshot;
  std::vector<EngineEvidenceReference> evidence;
};

// Loads one immutable owning-database snapshot. A genuinely absent store is
// initialized with the exact admitted literal row; any present but incomplete,
// torn, or contradictory store fails closed.
SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context);
SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity);

// engine.sblr_executor_availability_registry.set.v1
SblrExecutorAvailabilitySetResult SetSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySetRequest& request);

// Exact dispatch-time check. The supplied token snapshot is immutable
// admission evidence; this call reloads current durable authority.
EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot);
EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot);

std::string ComputeSblrExecutorAvailabilityRowIdentitySha256(
    const SblrExecutorAvailabilityRowIdentity& identity);

}  // namespace scratchbird::engine::internal_api
