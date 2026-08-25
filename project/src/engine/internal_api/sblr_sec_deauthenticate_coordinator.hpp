#pragma once
#include "engine/sblr/sblr_sec_deauthenticate_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecDeauthenticateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSecDeauthenticateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};};SblrSecDeauthenticateCoordinationResult CompileSblrSecDeauthenticateDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSecDeauthenticateCoordinationResult ConsumeSblrSecDeauthenticateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecDeauthenticateDescriptorV1&);}
