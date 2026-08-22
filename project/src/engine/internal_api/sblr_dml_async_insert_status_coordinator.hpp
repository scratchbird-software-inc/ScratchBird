#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_dml_async_insert_status_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDmlAsyncInsertStatusCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDmlAsyncInsertStatusDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDmlAsyncInsertStatusCoordinationResult CompileSblrDmlAsyncInsertStatusDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrDmlAsyncInsertStatusCoordinationResult ConsumeSblrDmlAsyncInsertStatusDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDmlAsyncInsertStatusDescriptorV1&);
}
