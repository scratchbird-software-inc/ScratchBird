#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_ddl_drop_dictionary_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api; int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid=scratchbird::tests::FixtureUuid(0xc008, 1);c.trace_tags={"private_ddl_drop_dictionary_binder"};auto m=CompileSblrDdlDropDictionaryDescriptor(c,c.statement_uuid,1,1,1);assert(m.ok);auto u=c;u.trace_tags={"private_ddl_drop_dictionary"};assert(ConsumeSblrDdlDropDictionaryDescriptor(u,m.descriptor).ok);auto r=ConsumeSblrDdlDropDictionaryDescriptor(u,m.descriptor);assert(!r.ok&&r.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
