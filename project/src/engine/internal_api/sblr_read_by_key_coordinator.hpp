#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_read_by_key_runtime.hpp"
namespace scratchbird::engine::internal_api {struct SblrReadByKeyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrReadByKeyDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrReadByKeyCoordinationResult CompileSblrReadByKeyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrReadByKeyCoordinationResult ConsumeSblrReadByKeyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrReadByKeyDescriptorV1&);EngineApiDiagnostic RecoverSblrReadByKeyDescriptors(const EngineRequestContext&);}
