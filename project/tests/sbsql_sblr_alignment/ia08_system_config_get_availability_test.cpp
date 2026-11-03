#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
#include "../support/scratch_database_path.hpp"
int main(){namespace a=scratchbird::engine::internal_api;a::EngineRequestContext c;c.database_path=scratchbird::tests::ScratchAvailabilityDatabasePath("sb_system_config_get");c.database_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000003961");c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};a::SblrExecutorAvailabilityRowIdentity r{a::kSblrSystemConfigGetExecutorId,5126,"1.0",a::kSblrSystemConfigGetOperandDescriptorId,a::kSblrSystemConfigGetResultDescriptorId,1};auto s=a::LoadSblrExecutorAvailabilitySnapshot(c,r);assert(s.ok&&s.snapshot.installed);}
