#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
#include "../support/scratch_database_path.hpp"
int main(){using namespace scratchbird::engine::internal_api; EngineRequestContext c;c.database_path=scratchbird::tests::ScratchAvailabilityDatabasePath("sb_alter_type");c.database_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002956");c.security_context_present=true;c.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};SblrExecutorAvailabilityRowIdentity r{"engine.op.ddl_alter_type",1570,"1.0","alter_type_descriptor","ddl_result",1};assert(!LoadSblrExecutorAvailabilitySnapshot(c,r).ok);return 0;}
