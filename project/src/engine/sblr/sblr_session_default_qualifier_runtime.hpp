#pragma once

// The default-qualifier operation has the same authenticated descriptor and
// result wire contract as session-setting mutation.  Keep the canonical
// operation identity/opcode distinct while reusing the proven byte codec.
#include "sblr_session_setting_set_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrSessionDefaultQualifierSetRequestV1 = SblrSessionSettingSetRequestV1;
using SblrSessionDefaultQualifierSetDescriptorV1 = SblrSessionSettingSetDescriptorV1;
using SblrSessionDefaultQualifierSetResultV1 = SblrSessionSettingSetResultV1;
inline auto EncodeSblrSessionDefaultQualifierSetRequestV1(const SblrSessionDefaultQualifierSetRequestV1& v){return EncodeSblrSessionSettingSetRequestV1(v);}
inline auto DecodeSblrSessionDefaultQualifierSetRequestV1(const uint8_t* d,size_t n,SblrSessionDefaultQualifierSetRequestV1* v,std::string* e){return DecodeSblrSessionSettingSetRequestV1(d,n,v,e);}
inline auto EncodeSblrSessionDefaultQualifierSetDescriptorV1(const SblrSessionDefaultQualifierSetDescriptorV1& v,bool b){return EncodeSblrSessionSettingSetDescriptorV1(v,b);}
inline auto DecodeSblrSessionDefaultQualifierSetDescriptorV1(const uint8_t* d,size_t n,SblrSessionDefaultQualifierSetDescriptorV1* v,std::string* e,bool b){return DecodeSblrSessionSettingSetDescriptorV1(d,n,v,e,b);}
inline auto EncodeSblrSessionDefaultQualifierSetResultV1(const SblrSessionDefaultQualifierSetResultV1& v){return EncodeSblrSessionSettingSetResultV1(v);}
inline auto DecodeSblrSessionDefaultQualifierSetResultV1(const uint8_t* d,size_t n,SblrSessionDefaultQualifierSetResultV1* v,std::string* e){return DecodeSblrSessionSettingSetResultV1(d,n,v,e);}
}
