#include "../support/binary_uuid_fixture.hpp"
#include "sblr_database_serialize_logical_snapshot_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid=scratchbird::tests::FixtureUuid(0xc008, 1);c.trace_tags={"private_database_serialize_logical_snapshot_binder"};auto x=CompileSblrDatabaseSerializeLogicalSnapshotDescriptor(c,c.statement_uuid,1,1,1);assert(x.ok);c.trace_tags={"private_database_serialize_logical_snapshot"};auto y=ConsumeSblrDatabaseSerializeLogicalSnapshotDescriptor(c,x.descriptor);assert(y.ok);auto z=ConsumeSblrDatabaseSerializeLogicalSnapshotDescriptor(c,x.descriptor);assert(!z.ok&&z.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
