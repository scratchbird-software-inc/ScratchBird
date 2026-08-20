#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_table_analyze_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrTableAnalyzeCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrTableAnalyzeDescriptorV1 descriptor;EngineApiDiagnostic diagnostic;};SblrTableAnalyzeCoordinationResult CompileSblrTableAnalyzeDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrTableAnalyzeCoordinationResult ConsumeSblrTableAnalyzeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrTableAnalyzeDescriptorV1&);}
