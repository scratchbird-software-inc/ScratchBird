#pragma once
#include "engine/sblr/sblr_sec_drop_policy_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecDropPolicyCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecDropPolicyDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSecDropPolicyCoordinationResult CompileSblrSecDropPolicyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t); SblrSecDropPolicyCoordinationResult ConsumeSblrSecDropPolicyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecDropPolicyDescriptorV1&); }
