#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_system_config_set_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrSystemConfigSetCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSystemConfigSetDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrSystemConfigSetCoordinationResult CompileSblrSystemConfigSetDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrSystemConfigSetCoordinationResult ConsumeSblrSystemConfigSetDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSystemConfigSetDescriptorV1&); }
