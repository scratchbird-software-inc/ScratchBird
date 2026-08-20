#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_kv_structured_stream_read_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrKvStructuredStreamReadCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrKvStructuredStreamReadDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrKvStructuredStreamReadCoordinationResult CompileSblrKvStructuredStreamReadDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrKvStructuredStreamReadCoordinationResult ConsumeSblrKvStructuredStreamReadDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrKvStructuredStreamReadDescriptorV1&); }
