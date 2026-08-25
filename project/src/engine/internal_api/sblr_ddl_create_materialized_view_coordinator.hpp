#pragma once
#include "api_types.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
#include "engine/sblr/sblr_ddl_create_materialized_view_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrDdlCreateMaterializedViewCoordinationResult = SblrDdlCreateViewCoordinationResult;
SblrDdlCreateMaterializedViewCoordinationResult CompileSblrDdlCreateMaterializedViewDescriptor(const EngineRequestContext&, const std::string&, std::uint64_t, std::uint32_t, std::uint64_t);
SblrDdlCreateMaterializedViewCoordinationResult ConsumeSblrDdlCreateMaterializedViewDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrDdlCreateMaterializedViewDescriptorV1&);
}
