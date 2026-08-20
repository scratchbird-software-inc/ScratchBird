#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_result_set_pass_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrResultSetPassCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrResultSetPassDescriptorV1 descriptor; EngineApiDiagnostic diagnostic; };
SblrResultSetPassCoordinationResult CompileSblrResultSetPassDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrResultSetPassCoordinationResult ConsumeSblrResultSetPassDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrResultSetPassDescriptorV1&);
EngineApiDiagnostic RecoverSblrResultSetPassDescriptors(const EngineRequestContext&);
}
