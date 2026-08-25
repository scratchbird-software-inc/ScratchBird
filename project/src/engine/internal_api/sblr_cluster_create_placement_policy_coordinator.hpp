#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_cluster_create_placement_policy_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrClusterCreatePlacementPolicyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrClusterCreatePlacementPolicyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrClusterCreatePlacementPolicyCoordinationResult CompileSblrClusterCreatePlacementPolicyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrClusterCreatePlacementPolicyCoordinationResult ConsumeSblrClusterCreatePlacementPolicyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrClusterCreatePlacementPolicyDescriptorV1&);}
