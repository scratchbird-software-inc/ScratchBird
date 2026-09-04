#include "engine/internal_api/sblr_executor_availability_registry.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
  namespace api = scratchbird::engine::internal_api;
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_bis_2451";
  context.database_uuid.canonical =
      "019d0000-0000-7000-8000-000000002451";
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  const api::SblrExecutorAvailabilityRowIdentity identity{
      api::kSblrBulkImportStreamExecutorId,
      775,
      "1.0",
      api::kSblrBulkImportStreamOperandDescriptorId,
      api::kSblrBulkImportStreamResultDescriptorId,
      1};
  const std::filesystem::path store =
      context.database_path +
      ".sb.sblr_executor_availability_registry.v1.bulk_import_stream";
  std::error_code ignored;
  std::filesystem::remove(store, ignored);

  const auto installed =
      api::LoadSblrExecutorAvailabilitySnapshot(context, identity);
  assert(installed.ok && installed.snapshot.installed);
  const auto replay =
      api::LoadSblrExecutorAvailabilitySnapshot(context, identity);
  assert(replay.ok &&
         replay.snapshot.snapshot_uuid == installed.snapshot.snapshot_uuid &&
         replay.snapshot.generation == installed.snapshot.generation);

  api::SblrExecutorAvailabilitySetRequest revoke;
  revoke.database_uuid = context.database_uuid.canonical;
  revoke.expected_snapshot_uuid = installed.snapshot.snapshot_uuid;
  revoke.expected_generation = installed.snapshot.generation;
  revoke.exact_row_identity = identity;
  revoke.requested_state =
      api::SblrExecutorAvailabilityState::revoked;
  revoke.reason_code = "CSC-TEST-002451";
  const auto revoked = api::SetSblrExecutorAvailability(context, revoke);
  assert(revoked.ok && revoked.snapshot.generation == 2);
  api::SblrExecutorAvailabilitySnapshot current;
  assert(api::RevalidateSblrExecutorAvailability(
             context, identity, installed.snapshot, &current)
             .code == "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(current.snapshot_uuid == revoked.snapshot.snapshot_uuid);

  {
    std::ofstream corrupt(store, std::ios::binary | std::ios::app);
    assert(corrupt.is_open());
    corrupt << "tampered\n";
  }
  const auto invalid =
      api::LoadSblrExecutorAvailabilitySnapshot(context, identity);
  assert(!invalid.ok &&
         invalid.diagnostic.code ==
             "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");

  std::filesystem::remove(store, ignored);
  return 0;
}
