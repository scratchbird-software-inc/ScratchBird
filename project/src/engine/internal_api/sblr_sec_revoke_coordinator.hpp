#pragma once
#include "engine/sblr/sblr_sec_revoke_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api{struct SblrSecRevokeCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSecRevokeDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};};SblrSecRevokeCoordinationResult CompileSblrSecRevokeDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSecRevokeCoordinationResult ConsumeSblrSecRevokeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecRevokeDescriptorV1&);}
