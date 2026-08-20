#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){namespace a=scratchbird::engine::internal_api;a::EngineRequestContext c;c.database_path="/tmp/sb_sort_2547";c.database_uuid.canonical="019d0000-0000-7000-8000-000000002547";c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};a::SblrExecutorAvailabilityRowIdentity r{a::kSblrSortExecutorId,1283,"1.0",a::kSblrSortOperandDescriptorId,a::kSblrSortResultDescriptorId,1};auto s=a::LoadSblrExecutorAvailabilitySnapshot(c,r);assert(s.ok&&s.snapshot.installed);}
