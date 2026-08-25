#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_event_trigger_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropEventTriggerCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropEventTriggerDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropEventTriggerCoordinationResult CompileSblrDdlDropEventTriggerDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropEventTriggerCoordinationResult ConsumeSblrDdlDropEventTriggerDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropEventTriggerDescriptorV1&);}
