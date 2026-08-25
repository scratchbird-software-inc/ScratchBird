#pragma once
#include "sblr_context_set_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrContextUnsetRequestV1 = SblrContextSetRequestV1;
using SblrContextUnsetDescriptorV1 = SblrContextSetDescriptorV1;
using SblrContextUnsetResultV1 = SblrContextSetResultV1;
inline auto EncodeSblrContextUnsetRequestV1(const SblrContextUnsetRequestV1& v) { return EncodeSblrContextSetRequestV1(v); }
inline auto DecodeSblrContextUnsetRequestV1(const uint8_t* d, size_t n, SblrContextUnsetRequestV1* v, std::string* e) { return DecodeSblrContextSetRequestV1(d,n,v,e); }
inline auto EncodeSblrContextUnsetDescriptorV1(const SblrContextUnsetDescriptorV1& v, bool b) { return EncodeSblrContextSetDescriptorV1(v,b); }
inline auto DecodeSblrContextUnsetDescriptorV1(const uint8_t* d, size_t n, SblrContextUnsetDescriptorV1* v, std::string* e, bool b) { return DecodeSblrContextSetDescriptorV1(d,n,v,e,b); }
}
