#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_llvm_policy_set_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAccelLlvmPolicySetCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelLlvmPolicySetDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAccelLlvmPolicySetCoordinationResult CompileSblrAccelLlvmPolicySetDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrAccelLlvmPolicySetCoordinationResult ConsumeSblrAccelLlvmPolicySetDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelLlvmPolicySetDescriptorV1&);}
