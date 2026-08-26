#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_timeseries_value_cache_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterTimeseriesValueCacheCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlAlterTimeseriesValueCacheDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlAlterTimeseriesValueCacheCoordinationResult CompileSblrDdlAlterTimeseriesValueCacheDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t,std::uint64_t); SblrDdlAlterTimeseriesValueCacheCoordinationResult ConsumeSblrDdlAlterTimeseriesValueCacheDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterTimeseriesValueCacheDescriptorV1&); }
