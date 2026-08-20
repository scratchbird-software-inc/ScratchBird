#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_limit_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrLimitCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrLimitDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrLimitCoordinationResult CompileSblrLimitDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrLimitCoordinationResult ConsumeSblrLimitDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrLimitDescriptorV1&); }
