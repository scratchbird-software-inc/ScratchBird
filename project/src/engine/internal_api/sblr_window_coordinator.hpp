#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_window_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrWindowCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrWindowDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrWindowCoordinationResult CompileSblrWindowDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrWindowCoordinationResult ConsumeSblrWindowDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrWindowDescriptorV1&); }
