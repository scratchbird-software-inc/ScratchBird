#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_kv_structured_read_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrKvStructuredReadCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrKvStructuredReadDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrKvStructuredReadCoordinationResult CompileSblrKvStructuredReadDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrKvStructuredReadCoordinationResult ConsumeSblrKvStructuredReadDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrKvStructuredReadDescriptorV1&); }
