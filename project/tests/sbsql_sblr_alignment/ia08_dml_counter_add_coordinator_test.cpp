#include "engine/internal_api/sblr_dml_counter_add_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="counter-receipt";auto q=CompileSblrDmlCounterAddDescriptor(c,"counter-receipt",1,2,1);assert(q.ok);auto x=ConsumeSblrDmlCounterAddDescriptor(c,q.descriptor);assert(x.ok);auto y=ConsumeSblrDmlCounterAddDescriptor(c,q.descriptor);assert(!y.ok&&y.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
