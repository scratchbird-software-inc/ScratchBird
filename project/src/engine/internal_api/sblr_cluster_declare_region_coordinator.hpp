#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_cluster_declare_region_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrClusterDeclareRegionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrClusterDeclareRegionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrClusterDeclareRegionCoordinationResult CompileSblrClusterDeclareRegionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrClusterDeclareRegionCoordinationResult ConsumeSblrClusterDeclareRegionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrClusterDeclareRegionDescriptorV1&);}
