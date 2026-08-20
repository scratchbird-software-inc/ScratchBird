#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_compare_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrCompareCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrCompareDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrCompareCoordinationResult CompileSblrCompareDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrCompareCoordinationResult ConsumeSblrCompareDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrCompareDescriptorV1&);}
