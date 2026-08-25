#include "engine/internal_api/sblr_cluster_drop_placement_policy_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;auto q=CompileSblrClusterDropPlacementPolicyDescriptor(c,"policy",7);assert(!q.ok);auto r=ConsumeSblrClusterDropPlacementPolicyDescriptor(c,q.descriptor);assert(!r.ok);}
