#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>

int main() {
  namespace a = scratchbird::engine::internal_api;
  a::EngineRequestContext c;
  c.database_path = "/tmp/sb_drop_synonym_availability";
  c.database_uuid.canonical = "019d0000-0000-7000-8000-000000002947";
  c.security_context_present = true;
  c.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  a::SblrExecutorAvailabilityRowIdentity row{
      a::kSblrDdlDropSynonymExecutorId, a::kSblrDdlDropSynonymOpcodeCode,
      a::kSblrDdlDropSynonymOpcodeVersion,
      a::kSblrDdlDropSynonymOperandDescriptorId,
      a::kSblrDdlDropSynonymResultDescriptorId,
      a::kSblrDdlDropSynonymResultDescriptorVersion};
  const auto loaded = a::LoadSblrExecutorAvailabilitySnapshot(c, row);
  assert(loaded.ok && loaded.snapshot.installed);
  assert(loaded.snapshot.row_identity_sha256 ==
         a::ComputeSblrExecutorAvailabilityRowIdentitySha256(row));
  return 0;
}
