#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_group_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrGroupCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrGroupDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrGroupCoordinationResult CompileSblrGroupDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrGroupCoordinationResult ConsumeSblrGroupDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrGroupDescriptorV1&);}
