#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_accel_llvm_inspect_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAccelLlvmInspectCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAccelLlvmInspectDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAccelLlvmInspectCoordinationResult CompileSblrAccelLlvmInspectDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrAccelLlvmInspectCoordinationResult ConsumeSblrAccelLlvmInspectDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAccelLlvmInspectDescriptorV1&);}
