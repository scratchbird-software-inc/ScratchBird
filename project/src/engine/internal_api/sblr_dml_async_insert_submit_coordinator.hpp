#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_dml_async_insert_submit_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDmlAsyncInsertSubmitCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDmlAsyncInsertSubmitDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDmlAsyncInsertSubmitCoordinationResult CompileSblrDmlAsyncInsertSubmitDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDmlAsyncInsertSubmitCoordinationResult ConsumeSblrDmlAsyncInsertSubmitDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDmlAsyncInsertSubmitDescriptorV1&); }
