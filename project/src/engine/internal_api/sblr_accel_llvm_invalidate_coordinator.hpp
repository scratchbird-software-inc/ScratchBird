#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_llvm_invalidate_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAccelLlvmInvalidateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelLlvmInvalidateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAccelLlvmInvalidateCoordinationResult CompileSblrAccelLlvmInvalidateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrAccelLlvmInvalidateCoordinationResult ConsumeSblrAccelLlvmInvalidateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelLlvmInvalidateDescriptorV1&);}
