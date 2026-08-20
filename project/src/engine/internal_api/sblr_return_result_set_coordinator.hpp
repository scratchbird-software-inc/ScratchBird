#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_return_result_set_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrReturnResultSetCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrReturnResultSetDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrReturnResultSetCoordinationResult CompileSblrReturnResultSetDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrReturnResultSetCoordinationResult ConsumeSblrReturnResultSetDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrReturnResultSetDescriptorV1&);
}
