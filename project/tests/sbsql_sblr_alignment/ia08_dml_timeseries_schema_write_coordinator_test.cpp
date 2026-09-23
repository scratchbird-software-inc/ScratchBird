#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_dml_timeseries_schema_write_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api; EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid=scratchbird::tests::FixtureUuid(0xc008, 1);auto q=CompileSblrDmlTimeseriesSchemaWriteDescriptor(c,c.statement_uuid,1,1,1);assert(q.ok);auto x=ConsumeSblrDmlTimeseriesSchemaWriteDescriptor(c,q.descriptor);assert(x.ok);auto y=ConsumeSblrDmlTimeseriesSchemaWriteDescriptor(c,q.descriptor);assert(!y.ok&&y.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
