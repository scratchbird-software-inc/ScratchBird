#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_operator_call_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrOperatorCallCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrOperatorCallDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrOperatorCallCoordinationResult CompileSblrOperatorCallDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrOperatorCallCoordinationResult ConsumeSblrOperatorCallDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrOperatorCallDescriptorV1&);}
