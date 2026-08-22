#include "engine/internal_api/sblr_ddl_create_timeseries_value_cache_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api; EngineRequestContext c{}; c.security_context_present=true; c.statement_metadata_snapshot_engine_owned=true; c.statement_uuid.canonical="s"; auto a=CompileSblrDdlCreateTimeseriesValueCacheDescriptor(c,"s",1,2,1); assert(a.ok); auto b=ConsumeSblrDdlCreateTimeseriesValueCacheDescriptor(c,a.descriptor); assert(b.ok); auto z=ConsumeSblrDdlCreateTimeseriesValueCacheDescriptor(c,a.descriptor); assert(!z.ok); return 0;}
