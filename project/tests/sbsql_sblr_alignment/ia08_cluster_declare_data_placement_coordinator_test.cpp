#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_cluster_declare_data_placement_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;auto q=CompileSblrClusterDeclareDataPlacementDescriptor(c,scratchbird::tests::FixtureUuid(1113, 27),7);assert(!q.ok);auto r=ConsumeSblrClusterDeclareDataPlacementDescriptor(c,q.descriptor);assert(!r.ok);}
