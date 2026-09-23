#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){namespace a=scratchbird::engine::internal_api;a::EngineRequestContext c;c.database_path="/tmp/sb_atomic_rmw_2467";c.database_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002467");c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};a::SblrExecutorAvailabilityRowIdentity r{a::kSblrAtomicRmwExecutorId,779,"1.0",a::kSblrAtomicRmwOperandDescriptorId,a::kSblrAtomicRmwResultDescriptorId,1};auto s=a::LoadSblrExecutorAvailabilitySnapshot(c,r);assert(s.ok&&s.snapshot.installed);}
