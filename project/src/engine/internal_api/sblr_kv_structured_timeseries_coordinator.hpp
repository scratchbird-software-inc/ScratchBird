#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_kv_structured_timeseries_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrKvStructuredTimeseriesCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrKvStructuredTimeseriesDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrKvStructuredTimeseriesCoordinationResult CompileSblrKvStructuredTimeseriesDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrKvStructuredTimeseriesCoordinationResult ConsumeSblrKvStructuredTimeseriesDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrKvStructuredTimeseriesDescriptorV1&); }
