#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_update_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrUpdateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrUpdateDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrUpdateCoordinationResult CompileSblrUpdateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrUpdateCoordinationResult ConsumeSblrUpdateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrUpdateDescriptorV1&);}
