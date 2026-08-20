#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_aggregate_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterAggregateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterAggregateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlAlterAggregateCoordinationResult CompileSblrDdlAlterAggregateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterAggregateCoordinationResult ConsumeSblrDdlAlterAggregateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterAggregateDescriptorV1&); }
