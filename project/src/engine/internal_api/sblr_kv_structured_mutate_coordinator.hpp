#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_kv_structured_mutate_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrKvStructuredMutateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrKvStructuredMutateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrKvStructuredMutateCoordinationResult CompileSblrKvStructuredMutateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrKvStructuredMutateCoordinationResult ConsumeSblrKvStructuredMutateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrKvStructuredMutateDescriptorV1&); }
