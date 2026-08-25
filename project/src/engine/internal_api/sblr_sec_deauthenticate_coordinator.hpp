#pragma once
#include "engine/sblr/sblr_sec_deauthenticate_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecDedeauthenticateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSecDedeauthenticateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};};SblrSecDedeauthenticateCoordinationResult CompileSblrSecDedeauthenticateDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSecDedeauthenticateCoordinationResult ConsumeSblrSecDedeauthenticateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecDedeauthenticateDescriptorV1&);}
