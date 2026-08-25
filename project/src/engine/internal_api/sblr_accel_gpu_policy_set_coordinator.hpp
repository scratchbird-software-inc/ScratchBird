#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_gpu_policy_set_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAccelGpuPolicySetCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelGpuPolicySetDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAccelGpuPolicySetCoordinationResult CompileSblrAccelGpuPolicySetDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrAccelGpuPolicySetCoordinationResult ConsumeSblrAccelGpuPolicySetDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelGpuPolicySetDescriptorV1&);}
