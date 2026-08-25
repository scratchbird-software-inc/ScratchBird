#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_llvm_compile_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAccelLlvmCompileCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelLlvmCompileDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAccelLlvmCompileCoordinationResult CompileSblrAccelLlvmCompileDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrAccelLlvmCompileCoordinationResult ConsumeSblrAccelLlvmCompileDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelLlvmCompileDescriptorV1&);}
