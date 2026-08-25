#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_event_trigger_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlCreateEventTriggerCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateEventTriggerDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateEventTriggerCoordinationResult CompileSblrDdlCreateEventTriggerDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlCreateEventTriggerCoordinationResult ConsumeSblrDdlCreateEventTriggerDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateEventTriggerDescriptorV1&);}
