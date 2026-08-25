#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_gpu_inspect_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAccelGpuInspectCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelGpuInspectDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAccelGpuInspectCoordinationResult CompileSblrAccelGpuInspectDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrAccelGpuInspectCoordinationResult ConsumeSblrAccelGpuInspectDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelGpuInspectDescriptorV1&);}
