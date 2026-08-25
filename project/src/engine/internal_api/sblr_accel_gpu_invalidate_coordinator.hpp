#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_gpu_invalidate_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrAccelGpuInvalidateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelGpuInvalidateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrAccelGpuInvalidateCoordinationResult CompileSblrAccelGpuInvalidateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrAccelGpuInvalidateCoordinationResult ConsumeSblrAccelGpuInvalidateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelGpuInvalidateDescriptorV1&); }
