#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_set_table_type_enforcement_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlSetTableTypeEnforcementCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlSetTableTypeEnforcementDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlSetTableTypeEnforcementCoordinationResult CompileSblrDdlSetTableTypeEnforcementDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlSetTableTypeEnforcementCoordinationResult ConsumeSblrDdlSetTableTypeEnforcementDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlSetTableTypeEnforcementDescriptorV1&); }
