#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_kv_structured_scan_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrKvStructuredScanCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrKvStructuredScanDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrKvStructuredScanCoordinationResult CompileSblrKvStructuredScanDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrKvStructuredScanCoordinationResult ConsumeSblrKvStructuredScanDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrKvStructuredScanDescriptorV1&); }
