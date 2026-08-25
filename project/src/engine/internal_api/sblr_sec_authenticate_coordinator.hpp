#pragma once
#include "engine/sblr/sblr_sec_authenticate_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecAuthenticateCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSecAuthenticateDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};};SblrSecAuthenticateCoordinationResult CompileSblrSecAuthenticateDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSecAuthenticateCoordinationResult ConsumeSblrSecAuthenticateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecAuthenticateDescriptorV1&);}
