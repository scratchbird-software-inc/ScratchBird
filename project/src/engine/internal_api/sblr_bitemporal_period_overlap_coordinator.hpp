#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bitemporal_period_overlap_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrBitemporalPeriodOverlapCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBitemporalPeriodOverlapDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrBitemporalPeriodOverlapCoordinationResult CompileSblrBitemporalPeriodOverlapDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrBitemporalPeriodOverlapCoordinationResult ConsumeSblrBitemporalPeriodOverlapDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBitemporalPeriodOverlapDescriptorV1&);}
