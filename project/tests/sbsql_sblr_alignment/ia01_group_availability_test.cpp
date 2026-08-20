#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){namespace a=scratchbird::engine::internal_api;a::EngineRequestContext c;c.database_path="/tmp/sb_group_2543";c.database_uuid.canonical="019d0000-0000-7000-8000-000000002543";c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};a::SblrExecutorAvailabilityRowIdentity r{a::kSblrGroupExecutorId,1282,"1.0",a::kSblrGroupOperandDescriptorId,a::kSblrGroupResultDescriptorId,1};auto s=a::LoadSblrExecutorAvailabilitySnapshot(c,r);assert(s.ok&&s.snapshot.installed);}
