#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_function_invoke_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrFunctionInvokeCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrFunctionInvokeDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrFunctionInvokeCoordinationResult CompileSblrFunctionInvokeDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrFunctionInvokeCoordinationResult ConsumeSblrFunctionInvokeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrFunctionInvokeDescriptorV1&);}
