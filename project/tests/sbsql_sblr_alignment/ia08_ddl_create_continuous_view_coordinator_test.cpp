#include "engine/internal_api/sblr_ddl_create_continuous_view_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api; EngineRequestContext c{};auto x=CompileSblrDdlCreateContinuousViewDescriptor(c,"r",1,1,1);assert(x.ok);auto y=ConsumeSblrDdlCreateContinuousViewDescriptor(c,x.descriptor);assert(y.ok);auto z=ConsumeSblrDdlCreateContinuousViewDescriptor(c,x.descriptor);assert(!z.ok);assert(z.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
