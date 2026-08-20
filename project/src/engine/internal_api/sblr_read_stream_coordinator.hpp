#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_read_stream_runtime.hpp"
namespace scratchbird::engine::internal_api {struct SblrReadStreamCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrReadStreamDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrReadStreamCoordinationResult CompileSblrReadStreamDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrReadStreamCoordinationResult ConsumeSblrReadStreamDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrReadStreamDescriptorV1&);EngineApiDiagnostic RecoverSblrReadStreamDescriptors(const EngineRequestContext&);}
