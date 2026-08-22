#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_dml_timeseries_schema_write_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDmlTimeseriesSchemaWriteCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDmlTimeseriesSchemaWriteDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDmlTimeseriesSchemaWriteCoordinationResult CompileSblrDmlTimeseriesSchemaWriteDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t,std::uint64_t); SblrDmlTimeseriesSchemaWriteCoordinationResult ConsumeSblrDmlTimeseriesSchemaWriteDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDmlTimeseriesSchemaWriteDescriptorV1&); }
