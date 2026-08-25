#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api; EngineRequestContext c;c.database_path="/tmp/sb_drop_type";c.database_uuid.canonical="019d0000-0000-7000-8000-000000002957";c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};SblrExecutorAvailabilityRowIdentity r{"engine.op.ddl_drop_type",1571,"1.0","drop_type_descriptor","ddl_result",1};assert(!LoadSblrExecutorAvailabilitySnapshot(c,r).ok);return 0;}
