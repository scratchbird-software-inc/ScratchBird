#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_timeseries_series_cardinality_policy_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDdlTimeseriesSeriesCardinalityPolicyCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDdlTimeseriesSeriesCardinalityPolicyCoordinationResult CompileSblrDdlTimeseriesSeriesCardinalityPolicyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t,std::uint64_t);
SblrDdlTimeseriesSeriesCardinalityPolicyCoordinationResult ConsumeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1&);
}
