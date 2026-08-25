#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bitemporal_for_versions_between_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrBitemporalForVersionsBetweenCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBitemporalForVersionsBetweenDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrBitemporalForVersionsBetweenCoordinationResult CompileSblrBitemporalForVersionsBetweenDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrBitemporalForVersionsBetweenCoordinationResult ConsumeSblrBitemporalForVersionsBetweenDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBitemporalForVersionsBetweenDescriptorV1&);}
