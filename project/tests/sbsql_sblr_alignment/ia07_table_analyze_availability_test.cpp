#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>

int main() {
  namespace a = scratchbird::engine::internal_api;
  a::EngineRequestContext context;
  context.database_path = "/tmp/sb_tta_2447";
  context.database_uuid.canonical = "019d0000-0000-7000-8000-000000002447";
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  a::SblrExecutorAvailabilityRowIdentity identity{a::kSblrTableAnalyzeExecutorId, 774, "1.0", a::kSblrTableAnalyzeOperandDescriptorId, a::kSblrTableAnalyzeResultDescriptorId, 1};
  const auto snapshot = a::LoadSblrExecutorAvailabilitySnapshot(context, identity);
  assert(snapshot.ok && snapshot.snapshot.installed);
}
