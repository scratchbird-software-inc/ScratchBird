#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_merge_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrMergeCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrMergeDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrMergeCoordinationResult CompileSblrMergeDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrMergeCoordinationResult ConsumeSblrMergeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrMergeDescriptorV1&);}
