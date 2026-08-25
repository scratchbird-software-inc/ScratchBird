#include "engine/internal_api/sblr_cluster_alter_placement_policy_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;auto q=CompileSblrClusterAlterPlacementPolicyDescriptor(c,"policy",7);assert(!q.ok);auto r=ConsumeSblrClusterAlterPlacementPolicyDescriptor(c,q.descriptor);assert(!r.ok);}
