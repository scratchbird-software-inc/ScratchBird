#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_kv_structured_stream_append_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrKvStructuredStreamAppendCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrKvStructuredStreamAppendDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrKvStructuredStreamAppendCoordinationResult CompileSblrKvStructuredStreamAppendDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrKvStructuredStreamAppendCoordinationResult ConsumeSblrKvStructuredStreamAppendDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrKvStructuredStreamAppendDescriptorV1&); }
