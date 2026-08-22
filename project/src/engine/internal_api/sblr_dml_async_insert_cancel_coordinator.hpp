#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_dml_async_insert_cancel_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDmlAsyncInsertCancelCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDmlAsyncInsertCancelDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDmlAsyncInsertCancelCoordinationResult CompileSblrDmlAsyncInsertCancelDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrDmlAsyncInsertCancelCoordinationResult ConsumeSblrDmlAsyncInsertCancelDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDmlAsyncInsertCancelDescriptorV1&);
}
