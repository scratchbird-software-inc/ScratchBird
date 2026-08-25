#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_cluster_drop_placement_policy_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrClusterDropPlacementPolicyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrClusterDropPlacementPolicyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrClusterDropPlacementPolicyCoordinationResult CompileSblrClusterDropPlacementPolicyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrClusterDropPlacementPolicyCoordinationResult ConsumeSblrClusterDropPlacementPolicyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrClusterDropPlacementPolicyDescriptorV1&);}
