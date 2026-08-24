#pragma once
#include "engine/sblr/sblr_sec_drop_group_mapping_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecDropGroupMappingCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecDropGroupMappingDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSecDropGroupMappingCoordinationResult CompileSblrSecDropGroupMappingDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t); SblrSecDropGroupMappingCoordinationResult ConsumeSblrSecDropGroupMappingDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecDropGroupMappingDescriptorV1&); }
