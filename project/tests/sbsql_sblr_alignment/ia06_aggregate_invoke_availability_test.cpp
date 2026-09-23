#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){namespace a=scratchbird::engine::internal_api;a::EngineRequestContext c;c.database_path="/tmp/sb_aggregate_invoke_2511";c.database_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002511");c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};a::SblrExecutorAvailabilityRowIdentity r{a::kSblrAggregateInvokeExecutorId,1032,"1.0",a::kSblrAggregateInvokeOperandDescriptorId,a::kSblrAggregateInvokeResultDescriptorId,1};auto s=a::LoadSblrExecutorAvailabilitySnapshot(c,r);assert(s.ok&&s.snapshot.installed);}
