#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_insert_runtime.hpp"
namespace scratchbird::engine::internal_api {struct SblrInsertCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrInsertDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrInsertCoordinationResult CompileSblrInsertDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrInsertCoordinationResult ConsumeSblrInsertDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrInsertDescriptorV1&);}
