#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_event_trigger_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlAlterEventTriggerCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterEventTriggerDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlAlterEventTriggerCoordinationResult CompileSblrDdlAlterEventTriggerDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlAlterEventTriggerCoordinationResult ConsumeSblrDdlAlterEventTriggerDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterEventTriggerDescriptorV1&);}
