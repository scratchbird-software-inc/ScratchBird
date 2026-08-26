#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_timeseries_value_cache_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropTimeseriesValueCacheCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlDropTimeseriesValueCacheDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlDropTimeseriesValueCacheCoordinationResult CompileSblrDdlDropTimeseriesValueCacheDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t,std::uint64_t); SblrDdlDropTimeseriesValueCacheCoordinationResult ConsumeSblrDdlDropTimeseriesValueCacheDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropTimeseriesValueCacheDescriptorV1&); }

