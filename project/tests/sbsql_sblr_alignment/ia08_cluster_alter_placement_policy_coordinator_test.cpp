#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_cluster_alter_placement_policy_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;auto q=CompileSblrClusterAlterPlacementPolicyDescriptor(c,scratchbird::tests::FixtureUuid(1113, 24),7);assert(!q.ok);auto r=ConsumeSblrClusterAlterPlacementPolicyDescriptor(c,q.descriptor);assert(!r.ok);}
