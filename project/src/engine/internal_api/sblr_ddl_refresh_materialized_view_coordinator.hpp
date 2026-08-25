#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_refresh_materialized_view_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDdlRefreshMaterializedViewCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlRefreshMaterializedViewDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDdlRefreshMaterializedViewCoordinationResult CompileSblrDdlRefreshMaterializedViewDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrDdlRefreshMaterializedViewCoordinationResult ConsumeSblrDdlRefreshMaterializedViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlRefreshMaterializedViewDescriptorV1&);
}
