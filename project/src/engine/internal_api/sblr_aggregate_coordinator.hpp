#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_aggregate_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAggregateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAggregateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAggregateCoordinationResult CompileSblrAggregateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrAggregateCoordinationResult ConsumeSblrAggregateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAggregateDescriptorV1&);}
