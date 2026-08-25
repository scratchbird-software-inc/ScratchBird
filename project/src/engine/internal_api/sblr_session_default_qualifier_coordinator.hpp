#pragma once
#include "sblr_session_setting_set_coordinator.hpp"
#include "../sblr/sblr_session_default_qualifier_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrSessionDefaultQualifierSetCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrSessionDefaultQualifierSetDescriptor(const EngineRequestContext& c,const std::string& r,uint64_t o,uint64_t a){return CompileSblrSessionSettingSetDescriptor(c,r,o,a);}
inline auto ConsumeSblrSessionDefaultQualifierSetDescriptor(const EngineRequestContext& c,const scratchbird::engine::sblr::SblrSessionDefaultQualifierSetDescriptorV1& d){return ConsumeSblrSessionSettingSetDescriptor(c,d);}
}
