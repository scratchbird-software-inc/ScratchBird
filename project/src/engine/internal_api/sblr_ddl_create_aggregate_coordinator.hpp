#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_aggregate_runtime.hpp"
namespace scratchbird::engine::internal_api {struct SblrDdlCreateAggregateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateAggregateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateAggregateCoordinationResult CompileSblrDdlCreateAggregateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrDdlCreateAggregateCoordinationResult ConsumeSblrDdlCreateAggregateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateAggregateDescriptorV1&);}
