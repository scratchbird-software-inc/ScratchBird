#pragma once
#include "sblr_session_setting_get_runtime.hpp"
#include "sblr_session_setting_set_coordinator.hpp"
namespace scratchbird::engine::internal_api { inline auto CompileSblrSessionSettingGetDescriptor(const EngineRequestContext&c,const std::string&r,uint64_t o,uint64_t a){return CompileSblrSessionSettingSetDescriptor(c,r,o,a);} inline auto ConsumeSblrSessionSettingGetDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrSessionSettingGetDescriptorV1&v){return ConsumeSblrSessionSettingSetDescriptor(c,v);} }
