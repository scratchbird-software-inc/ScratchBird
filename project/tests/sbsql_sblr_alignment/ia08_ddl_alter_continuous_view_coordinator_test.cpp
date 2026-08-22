#include "engine/internal_api/sblr_ddl_alter_continuous_view_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c{};auto x=CompileSblrDdlAlterContinuousViewDescriptor(c,"r",1,1,1);assert(x.ok);auto y=ConsumeSblrDdlAlterContinuousViewDescriptor(c,x.descriptor);assert(y.ok);auto z=ConsumeSblrDdlAlterContinuousViewDescriptor(c,x.descriptor);assert(!z.ok&&z.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
