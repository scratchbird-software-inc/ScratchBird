#include "sblr_executor_availability_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace api = scratchbird::engine::internal_api;

int main() {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, 1786839000000ull);
  if (!generated.ok()) return EXIT_FAILURE;
  api::EngineRequestContext context;
  context.database_uuid.canonical =
      scratchbird::core::uuid::UuidToString(generated.value.value);
  context.database_path =
      (std::filesystem::temp_directory_path() /
       ("sb_variable_executor_" + std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()))).string();
  context.security_context_present = true;
  context.trace_tags.push_back("right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  api::SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = api::kSblrVariableExecutorId;
  identity.opcode_code = api::kSblrVariableOpcodeCode;
  identity.opcode_version = api::kSblrVariableOpcodeVersion;
  identity.operand_descriptor_id = api::kSblrVariableOperandDescriptorId;
  identity.result_descriptor_id = api::kSblrVariableResultDescriptorId;
  identity.result_descriptor_version = api::kSblrVariableResultDescriptorVersion;
  const auto admitted = api::LoadSblrExecutorAvailabilitySnapshot(context, identity);
  if (!admitted.ok || !admitted.snapshot.installed) return EXIT_FAILURE;
  api::SblrExecutorAvailabilitySetRequest request;
  request.database_uuid = context.database_uuid.canonical;
  request.exact_row_identity = identity;
  request.expected_snapshot_uuid = admitted.snapshot.snapshot_uuid;
  request.expected_generation = admitted.snapshot.generation;
  request.requested_state = api::SblrExecutorAvailabilityState::revoked;
  request.reason_code = "test.variable.revoke.after.admission";
  const auto revoked = api::SetSblrExecutorAvailability(context, request);
  api::SblrExecutorAvailabilitySnapshot observed;
  const auto diagnostic = api::RevalidateSblrExecutorAvailability(
      context, identity, admitted.snapshot, &observed);
  std::error_code ignored;
  std::filesystem::remove(
      context.database_path +
          ".sb.sblr_executor_availability_registry.v1.variable",
      ignored);
  if (!revoked.ok ||
      diagnostic.code != "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" ||
      observed.generation != revoked.snapshot.generation) {
    std::cerr << "variable executor revocation did not produce exact missing evidence\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
