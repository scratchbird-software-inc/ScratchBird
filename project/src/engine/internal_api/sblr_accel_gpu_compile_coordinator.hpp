#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_gpu_compile_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAccelGpuCompileCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelGpuCompileDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAccelGpuCompileCoordinationResult CompileSblrAccelGpuCompileDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrAccelGpuCompileCoordinationResult ConsumeSblrAccelGpuCompileDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelGpuCompileDescriptorV1&);}
