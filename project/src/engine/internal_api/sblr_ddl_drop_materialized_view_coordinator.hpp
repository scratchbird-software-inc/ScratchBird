#pragma once
#include "sblr_ddl_create_view_coordinator.hpp"
#include "engine/sblr/sblr_ddl_drop_materialized_view_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrDdlDropMaterializedViewCoordinationResult=SblrDdlCreateViewCoordinationResult;
SblrDdlDropMaterializedViewCoordinationResult CompileSblrDdlDropMaterializedViewDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrDdlDropMaterializedViewCoordinationResult ConsumeSblrDdlDropMaterializedViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropMaterializedViewDescriptorV1&);
}
