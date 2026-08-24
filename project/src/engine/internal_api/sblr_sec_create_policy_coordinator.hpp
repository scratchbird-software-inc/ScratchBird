#pragma once
#include "engine/sblr/sblr_sec_create_policy_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecCreatePolicyCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecCreatePolicyDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSecCreatePolicyCoordinationResult CompileSblrSecCreatePolicyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t); SblrSecCreatePolicyCoordinationResult ConsumeSblrSecCreatePolicyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecCreatePolicyDescriptorV1&); }
