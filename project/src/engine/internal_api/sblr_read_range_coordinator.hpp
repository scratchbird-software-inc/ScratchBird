#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_read_range_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrReadRangeCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrReadRangeDescriptorV1 descriptor; EngineApiDiagnostic diagnostic; };
SblrReadRangeCoordinationResult CompileSblrReadRangeDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrReadRangeCoordinationResult ConsumeSblrReadRangeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrReadRangeDescriptorV1&);
EngineApiDiagnostic RecoverSblrReadRangeDescriptors(const EngineRequestContext&);
}
