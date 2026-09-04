#include "engine/internal_api/sblr_executor_availability_registry.hpp"

#include <chrono>
#include <filesystem>
#include <string>

int main() {
  namespace api = scratchbird::engine::internal_api;

  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  api::EngineRequestContext context;
  context.database_path =
      (std::filesystem::temp_directory_path() /
       ("sb_create_temporary_table_2663_" + unique))
          .string();
  context.database_uuid.canonical =
      "019d0000-0000-7000-8000-000000002663";
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};

  const api::SblrExecutorAvailabilityRowIdentity identity{
      "engine.op.ddl_create_temporary_table",
      1612,
      "1.0",
      "create_temporary_table_descriptor",
      "ddl_result",
      1};
  const auto current =
      api::LoadCurrentSblrExecutorAvailabilitySnapshot(context, identity);
  return !current.ok &&
                 current.diagnostic.code ==
                     "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"
             ? 0
             : 1;
}
