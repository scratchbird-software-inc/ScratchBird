#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_timeseries_value_cache_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDdlCreateTimeseriesValueCacheCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlCreateTimeseriesValueCacheDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDdlCreateTimeseriesValueCacheCoordinationResult CompileSblrDdlCreateTimeseriesValueCacheDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t,std::uint64_t);
SblrDdlCreateTimeseriesValueCacheCoordinationResult ConsumeSblrDdlCreateTimeseriesValueCacheDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateTimeseriesValueCacheDescriptorV1&);
}
