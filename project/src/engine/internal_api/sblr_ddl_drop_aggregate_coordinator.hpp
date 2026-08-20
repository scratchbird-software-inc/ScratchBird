#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_aggregate_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropAggregateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropAggregateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropAggregateCoordinationResult CompileSblrDdlDropAggregateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropAggregateCoordinationResult ConsumeSblrDdlDropAggregateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropAggregateDescriptorV1&); }
