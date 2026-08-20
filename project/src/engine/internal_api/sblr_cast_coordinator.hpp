#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_cast_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrCastCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrCastDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrCastCoordinationResult CompileSblrCastDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrCastCoordinationResult ConsumeSblrCastDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrCastDescriptorV1&);}
