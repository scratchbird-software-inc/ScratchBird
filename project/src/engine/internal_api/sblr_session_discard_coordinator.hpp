#pragma once
#include "sblr_session_setting_set_coordinator.hpp"
#include "../sblr/sblr_session_discard_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrSessionDiscardCoordinationResult=SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrSessionDiscardDescriptor(const EngineRequestContext&c,const std::string&r,uint64_t o,uint64_t a){return CompileSblrSessionSettingSetDescriptor(c,r,o,a);}
inline auto ConsumeSblrSessionDiscardDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrSessionDiscardDescriptorV1&v){return ConsumeSblrSessionSettingSetDescriptor(c,v);}
}
