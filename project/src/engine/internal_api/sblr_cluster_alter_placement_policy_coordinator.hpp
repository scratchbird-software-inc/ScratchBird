#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_cluster_alter_placement_policy_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrClusterAlterPlacementPolicyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrClusterAlterPlacementPolicyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrClusterAlterPlacementPolicyCoordinationResult CompileSblrClusterAlterPlacementPolicyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrClusterAlterPlacementPolicyCoordinationResult ConsumeSblrClusterAlterPlacementPolicyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrClusterAlterPlacementPolicyDescriptorV1&);}
