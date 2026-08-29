#include "engine/internal_api/sblr_executor_availability_registry.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef NDEBUG
#undef assert
#define assert(condition) \
  ((condition) ? static_cast<void>(0) : std::abort())
#endif

int main() {
  namespace api = scratchbird::engine::internal_api;

  const auto nonce = std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count();
  api::EngineRequestContext context;
  context.database_path =
      (std::filesystem::temp_directory_path() /
       ("sb_create_index_fail_closed_2603_" + std::to_string(nonce)))
          .string();
  context.database_uuid.canonical =
      "019d0000-0000-7000-8000-000000002571";
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};

  const api::SblrExecutorAvailabilityRowIdentity row{
      api::kSblrDdlCreateIndexExecutorId,
      api::kSblrDdlCreateIndexOpcodeCode,
      api::kSblrDdlCreateIndexOpcodeVersion,
      api::kSblrDdlCreateIndexOperandDescriptorId,
      api::kSblrDdlCreateIndexResultDescriptorId,
      api::kSblrDdlCreateIndexResultDescriptorVersion};
  const std::filesystem::path registry_path =
      context.database_path +
      ".sb.sblr_executor_availability_registry.v1.ddl_create_index";

  assert(api::IsAdmittedExecutorAvailabilityIdentity(row));
  assert(!api::ComputeSblrExecutorAvailabilityRowIdentitySha256(row).empty());
  assert(!std::filesystem::exists(registry_path));

  const auto loaded = api::LoadSblrExecutorAvailabilitySnapshot(context, row);
  assert(loaded.ok);
  assert(loaded.diagnostic.code == "OK");
  assert(loaded.snapshot.installed);
  assert(loaded.snapshot.availability_state ==
         api::SblrExecutorAvailabilityState::installed);
  assert(loaded.snapshot.generation != 0);
  assert(!loaded.snapshot.snapshot_uuid.empty());
  assert(loaded.snapshot.database_uuid == context.database_uuid.canonical);
  assert(loaded.snapshot.row_identity_sha256 ==
         api::ComputeSblrExecutorAvailabilityRowIdentitySha256(row));
  assert(!loaded.snapshot.decision_evidence_sha256.empty());
  assert(std::filesystem::exists(registry_path));

  const auto recovered =
      api::LoadCurrentSblrExecutorAvailabilitySnapshot(context, row);
  assert(recovered.ok);
  assert(recovered.snapshot.snapshot_uuid == loaded.snapshot.snapshot_uuid);
  assert(recovered.snapshot.generation == loaded.snapshot.generation);
  assert(recovered.snapshot.row_identity_sha256 ==
         loaded.snapshot.row_identity_sha256);
  assert(recovered.snapshot.installed);
  assert(recovered.snapshot.availability_state ==
         api::SblrExecutorAvailabilityState::installed);

  api::SblrExecutorAvailabilitySetRequest set_request;
  set_request.database_uuid = context.database_uuid.canonical;
  set_request.expected_snapshot_uuid = loaded.snapshot.snapshot_uuid;
  set_request.expected_generation = loaded.snapshot.generation;
  set_request.exact_row_identity = row;
  set_request.requested_state =
      api::SblrExecutorAvailabilityState::unavailable;
  set_request.reason_code = "test.unavailable";
  const auto set = api::SetSblrExecutorAvailability(context, set_request);
  assert(set.ok);
  assert(set.snapshot.generation == loaded.snapshot.generation + 1);
  assert(!set.snapshot.installed);
  assert(set.snapshot.availability_state ==
         api::SblrExecutorAvailabilityState::unavailable);
  assert(!set.evidence.empty());

  const auto unavailable =
      api::LoadSblrExecutorAvailabilitySnapshot(context, row);
  assert(unavailable.ok);
  assert(unavailable.snapshot.snapshot_uuid == set.snapshot.snapshot_uuid);
  assert(unavailable.snapshot.generation == set.snapshot.generation);
  assert(!unavailable.snapshot.installed);
  assert(unavailable.snapshot.availability_state ==
         api::SblrExecutorAvailabilityState::unavailable);

  const auto unavailable_current =
      api::LoadCurrentSblrExecutorAvailabilitySnapshot(context, row);
  assert(unavailable_current.ok);
  assert(unavailable_current.snapshot.snapshot_uuid ==
         set.snapshot.snapshot_uuid);
  assert(unavailable_current.snapshot.generation == set.snapshot.generation);
  assert(!unavailable_current.snapshot.installed);
  assert(unavailable_current.snapshot.availability_state ==
         api::SblrExecutorAvailabilityState::unavailable);

  api::SblrExecutorAvailabilitySnapshot current;
  const auto revalidated = api::RevalidateSblrExecutorAvailability(
      context, row, loaded.snapshot, &current);
  assert(revalidated.code == "SBLR.OPCODE.EXECUTOR_UNAVAILABLE");
  assert(!current.installed);
  assert(current.availability_state ==
         api::SblrExecutorAvailabilityState::unavailable);
  assert(current.generation == set.snapshot.generation);
  assert(std::filesystem::exists(registry_path));
  return 0;
}
