#pragma once
#include "engine/sblr/sblr_session_setting_set_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSessionSettingSetCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSessionSettingSetCoordinationResult CompileSblrSessionSettingSetDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t); SblrSessionSettingSetCoordinationResult ConsumeSblrSessionSettingSetDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1&); }
