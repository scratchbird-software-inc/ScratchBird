#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_query_numeric_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrQueryNumericCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrQueryNumericDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrQueryNumericCoordinationResult CompileSblrQueryNumericDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrQueryNumericCoordinationResult ConsumeSblrQueryNumericDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrQueryNumericDescriptorV1&);}
