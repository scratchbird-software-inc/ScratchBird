#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_cluster_declare_data_placement_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrClusterDeclareDataPlacementCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrClusterDeclareDataPlacementDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrClusterDeclareDataPlacementCoordinationResult CompileSblrClusterDeclareDataPlacementDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrClusterDeclareDataPlacementCoordinationResult ConsumeSblrClusterDeclareDataPlacementDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrClusterDeclareDataPlacementDescriptorV1&);}
