#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_udr_invoke_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrUdrInvokeCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrUdrInvokeDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrUdrInvokeCoordinationResult CompileSblrUdrInvokeDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrUdrInvokeCoordinationResult ConsumeSblrUdrInvokeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrUdrInvokeDescriptorV1&);}
