#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_aggregate_invoke_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAggregateInvokeCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAggregateInvokeDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAggregateInvokeCoordinationResult CompileSblrAggregateInvokeDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrAggregateInvokeCoordinationResult ConsumeSblrAggregateInvokeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAggregateInvokeDescriptorV1&);}
