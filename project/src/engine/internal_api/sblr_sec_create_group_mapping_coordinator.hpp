#pragma once
#include "engine/sblr/sblr_sec_create_group_mapping_runtime.hpp"
#include "api_context.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecCreateGroupMappingCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecCreateGroupMappingDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSecCreateGroupMappingCoordinationResult CompileSblrSecCreateGroupMappingDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t); SblrSecCreateGroupMappingCoordinationResult ConsumeSblrSecCreateGroupMappingDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecCreateGroupMappingDescriptorV1&); }
