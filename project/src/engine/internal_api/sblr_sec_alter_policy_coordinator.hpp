#pragma once
#include "engine/sblr/sblr_sec_alter_policy_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api{struct SblrSecAlterPolicyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};};SblrSecAlterPolicyCoordinationResult CompileSblrSecAlterPolicyDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSecAlterPolicyCoordinationResult ConsumeSblrSecAlterPolicyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1&);}
