#include "sblr_executor_availability_registry.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "uuid.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>

namespace api = scratchbird::engine::internal_api;
namespace {
std::string Id(scratchbird::core::platform::UuidKind kind) {
  static std::uint64_t stamp = 1787003000000ull;
  auto value = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, ++stamp);
  assert(value.ok());
  return scratchbird::core::uuid::UuidToString(value.value.value);
}
}

int main() {
  const auto base = std::filesystem::temp_directory_path() /
      ("sb_savepoint_" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  api::EngineRequestContext context;
  context.database_path = base.string();
  context.database_uuid.canonical = Id(scratchbird::core::platform::UuidKind::database);
  context.session_uuid.canonical = Id(scratchbird::core::platform::UuidKind::session);
  context.transaction_uuid.canonical = Id(scratchbird::core::platform::UuidKind::object);
  context.statement_uuid.canonical = Id(scratchbird::core::platform::UuidKind::object);
  context.local_transaction_id = 41;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_savepoint_coordination"};

  const std::string transaction_evidence = "sha256:" + std::string(64, 'a');
  const std::string symbol_sha = "sha256:" + std::string(64, 'b');
  auto reserved = api::ReserveSblrSavepoint(
      context, context.statement_uuid.canonical, transaction_evidence, 7, symbol_sha);
  assert(reserved.ok && reserved.snapshot.transaction_ordinal == 1 &&
         reserved.snapshot.state == api::SblrSavepointState::reserved);
  auto replay = api::ActivateSblrSavepoint(
      context, context.statement_uuid.canonical, reserved.snapshot.descriptor_uuid,
      reserved.snapshot.descriptor_generation + 1,
      reserved.snapshot.descriptor_evidence_sha256, 1);
  assert(!replay.ok && replay.diagnostic.code == "MGA.SAVEPOINT.STALE");
  auto active = api::ActivateSblrSavepoint(
      context, context.statement_uuid.canonical, reserved.snapshot.descriptor_uuid,
      reserved.snapshot.descriptor_generation,
      reserved.snapshot.descriptor_evidence_sha256, 1);
  assert(active.ok && active.snapshot.state == api::SblrSavepointState::active &&
         active.snapshot.stack_generation == 1);
  replay = api::ActivateSblrSavepoint(
      context, context.statement_uuid.canonical, reserved.snapshot.descriptor_uuid,
      reserved.snapshot.descriptor_generation,
      reserved.snapshot.descriptor_evidence_sha256, 1);
  assert(!replay.ok && replay.diagnostic.code == "MGA.SAVEPOINT.STALE");

  api::SblrExecutorAvailabilityRowIdentity row;
  row.executor_id = api::kSblrTxnSavepointExecutorId;
  row.opcode_code = api::kSblrTxnSavepointOpcodeCode;
  row.opcode_version = api::kSblrTxnSavepointOpcodeVersion;
  row.operand_descriptor_id = api::kSblrTxnSavepointOperandDescriptorId;
  row.result_descriptor_id = api::kSblrTxnSavepointResultDescriptorId;
  row.result_descriptor_version = api::kSblrTxnSavepointResultDescriptorVersion;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  const auto installed = api::LoadSblrExecutorAvailabilitySnapshot(context, row);
  assert(installed.ok && installed.snapshot.installed);
  api::SblrExecutorAvailabilitySetRequest request;
  request.database_uuid = context.database_uuid.canonical;
  request.expected_snapshot_uuid = installed.snapshot.snapshot_uuid;
  request.expected_generation = installed.snapshot.generation;
  request.exact_row_identity = row;
  request.requested_state = api::SblrExecutorAvailabilityState::revoked;
  request.reason_code = "CSC-TEST-002359";
  assert(api::SetSblrExecutorAvailability(context, request).ok);
  api::SblrExecutorAvailabilitySnapshot current;
  assert(api::RevalidateSblrExecutorAvailability(
             context, row, installed.snapshot, &current).code ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");

  std::error_code ec;
  std::filesystem::remove(base.string()+".sb.sblr_savepoint_coordinator.v1",ec);
  std::filesystem::remove(base.string()+".sb.sblr_executor_availability_registry.v1.txn_savepoint",ec);
  return 0;
}
