#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_trigger_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterTriggerCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterTriggerDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlAlterTriggerCoordinationResult CompileSblrDdlAlterTriggerDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterTriggerCoordinationResult ConsumeSblrDdlAlterTriggerDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterTriggerDescriptorV1&); }
