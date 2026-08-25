#pragma once
#include "sblr_session_setting_set_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrSessionDiscardRequestV1=SblrSessionSettingSetRequestV1; using SblrSessionDiscardDescriptorV1=SblrSessionSettingSetDescriptorV1; using SblrSessionDiscardResultV1=SblrSessionSettingSetResultV1;
inline auto EncodeSblrSessionDiscardRequestV1(const SblrSessionDiscardRequestV1&v){return EncodeSblrSessionSettingSetRequestV1(v);} inline auto DecodeSblrSessionDiscardRequestV1(const uint8_t*d,size_t n,SblrSessionDiscardRequestV1*v,std::string*e){return DecodeSblrSessionSettingSetRequestV1(d,n,v,e);} inline auto EncodeSblrSessionDiscardDescriptorV1(const SblrSessionDiscardDescriptorV1&v,bool b){return EncodeSblrSessionSettingSetDescriptorV1(v,b);} inline auto DecodeSblrSessionDiscardDescriptorV1(const uint8_t*d,size_t n,SblrSessionDiscardDescriptorV1*v,std::string*e,bool b){return DecodeSblrSessionSettingSetDescriptorV1(d,n,v,e,b);} inline auto EncodeSblrSessionDiscardResultV1(const SblrSessionDiscardResultV1&v){return EncodeSblrSessionSettingSetResultV1(v);}
}
