#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_function_call_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrFunctionCallCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrFunctionCallDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrFunctionCallCoordinationResult CompileSblrFunctionCallDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrFunctionCallCoordinationResult ConsumeSblrFunctionCallDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrFunctionCallDescriptorV1&);
}
