#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_sort_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrSortCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSortDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrSortCoordinationResult CompileSblrSortDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrSortCoordinationResult ConsumeSblrSortDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSortDescriptorV1&);}
