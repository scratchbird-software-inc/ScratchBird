// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_executor_availability_registry.hpp"
#include "uuid.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>

namespace a = scratchbird::engine::internal_api;

int main() {
  const auto database = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, 1787003000000ULL);
  assert(database.ok());

  a::EngineRequestContext context;
  context.database_uuid.canonical =
      scratchbird::core::uuid::UuidToString(database.value.value);
  context.database_path =
      (std::filesystem::temp_directory_path() /
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
          .string();
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};

  a::SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = a::kSblrTxnRollbackExecutorId;
  identity.opcode_code = a::kSblrTxnRollbackOpcodeCode;
  identity.opcode_version = a::kSblrTxnRollbackOpcodeVersion;
  identity.operand_descriptor_id = a::kSblrTxnRollbackOperandDescriptorId;
  identity.result_descriptor_id = a::kSblrTxnRollbackResultDescriptorId;
  identity.result_descriptor_version =
      a::kSblrTxnRollbackResultDescriptorVersion;

  const auto installed =
      a::LoadSblrExecutorAvailabilitySnapshot(context, identity);
  assert(installed.ok && installed.snapshot.installed);

  a::SblrExecutorAvailabilitySetRequest request;
  request.database_uuid = context.database_uuid.canonical;
  request.exact_row_identity = identity;
  request.expected_snapshot_uuid = installed.snapshot.snapshot_uuid;
  request.expected_generation = installed.snapshot.generation;
  request.requested_state = a::SblrExecutorAvailabilityState::revoked;
  request.reason_code = "CSC-TEST-002355";
  assert(a::SetSblrExecutorAvailability(context, request).ok);

  a::SblrExecutorAvailabilitySnapshot current;
  assert(a::RevalidateSblrExecutorAvailability(
             context, identity, installed.snapshot, &current)
             .code == "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  std::error_code error;
  std::filesystem::remove(
      context.database_path +
          ".sb.sblr_executor_availability_registry.v1.txn_rollback",
      error);
  return 0;
}
