#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bitemporal_as_of_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBitemporalAsOfCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBitemporalAsOfDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBitemporalAsOfCoordinationResult CompileSblrBitemporalAsOfDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBitemporalAsOfCoordinationResult ConsumeSblrBitemporalAsOfDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBitemporalAsOfDescriptorV1&); }
