#include "engine/internal_api/sblr_ddl_drop_timeseries_value_cache_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="cache-receipt";auto q=CompileSblrDdlDropTimeseriesValueCacheDescriptor(c,"cache-receipt",1,2,1);assert(q.ok);auto x=ConsumeSblrDdlDropTimeseriesValueCacheDescriptor(c,q.descriptor);assert(x.ok);auto y=ConsumeSblrDdlDropTimeseriesValueCacheDescriptor(c,q.descriptor);assert(!y.ok&&y.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}

