#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_trigger_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateTriggerCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateTriggerCoordinationResult CompileSblrDdlCreateTriggerDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateTriggerCoordinationResult ConsumeSblrDdlCreateTriggerDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1&); }
