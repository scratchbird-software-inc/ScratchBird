#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){namespace a=scratchbird::engine::internal_api;a::EngineRequestContext c;c.database_path="/tmp/sb_refresh_mv_2951";c.database_uuid.canonical="019d0000-0000-7000-8000-000000002951";c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};a::SblrExecutorAvailabilityRowIdentity r{"engine.op.ddl_refresh_materialized_view",1567,"1.0","refresh_materialized_view_descriptor","ddl_result",1};auto s=a::LoadSblrExecutorAvailabilitySnapshot(c,r);assert(!s.ok);return 0;}
