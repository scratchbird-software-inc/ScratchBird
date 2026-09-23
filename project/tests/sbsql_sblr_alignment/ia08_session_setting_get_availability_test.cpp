#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_session_setting_get_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid=scratchbird::tests::FixtureUuid(1154, 23);auto x=CompileSblrSessionSettingGetDescriptor(c,scratchbird::tests::FixtureUuid(1154, 23),1,1);assert(x.ok);return 0;}
