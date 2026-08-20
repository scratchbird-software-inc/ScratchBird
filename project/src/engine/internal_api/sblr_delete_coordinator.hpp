#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_delete_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDeleteCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDeleteDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrDeleteCoordinationResult CompileSblrDeleteDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrDeleteCoordinationResult ConsumeSblrDeleteDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDeleteDescriptorV1&);}
