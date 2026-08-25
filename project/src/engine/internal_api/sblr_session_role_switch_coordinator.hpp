#pragma once
#include "engine/sblr/sblr_session_role_switch_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSessionRoleSwitchCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSessionRoleSwitchDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};};SblrSessionRoleSwitchCoordinationResult CompileSblrSessionRoleSwitchDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSessionRoleSwitchCoordinationResult ConsumeSblrSessionRoleSwitchDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSessionRoleSwitchDescriptorV1&);}
