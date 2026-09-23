#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_sec_grant_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid=scratchbird::tests::FixtureUuid(1154, 17);c.trace_tags={"private_sec_grant_binder"};assert(CompileSblrSecGrantDescriptor(c,scratchbird::tests::FixtureUuid(1154, 17),1,1).ok);}
