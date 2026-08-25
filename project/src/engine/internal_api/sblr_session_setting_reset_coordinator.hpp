#pragma once
#include "sblr_session_setting_reset_runtime.hpp"
#include "sblr_session_setting_set_coordinator.hpp"
namespace scratchbird::engine::internal_api { inline auto CompileSblrSessionSettingResetDescriptor(const EngineRequestContext&c,const std::string&r,uint64_t o,uint64_t a){return CompileSblrSessionSettingSetDescriptor(c,r,o,a);} inline auto ConsumeSblrSessionSettingResetDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrSessionSettingResetDescriptorV1&v){return ConsumeSblrSessionSettingSetDescriptor(c,v);} }
