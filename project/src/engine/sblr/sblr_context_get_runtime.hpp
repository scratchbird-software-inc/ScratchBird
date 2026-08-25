#pragma once
#include "sblr_context_set_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrContextGetRequestV1 = SblrContextSetRequestV1;
using SblrContextGetDescriptorV1 = SblrContextSetDescriptorV1;
using SblrContextGetResultV1 = SblrContextSetResultV1;
inline auto EncodeSblrContextGetRequestV1(const SblrContextGetRequestV1& v) { return EncodeSblrContextSetRequestV1(v); }
inline auto DecodeSblrContextGetRequestV1(const uint8_t* d, size_t n, SblrContextGetRequestV1* v, std::string* e) { return DecodeSblrContextSetRequestV1(d,n,v,e); }
inline auto EncodeSblrContextGetDescriptorV1(const SblrContextGetDescriptorV1& v, bool b) { return EncodeSblrContextSetDescriptorV1(v,b); }
inline auto DecodeSblrContextGetDescriptorV1(const uint8_t* d, size_t n, SblrContextGetDescriptorV1* v, std::string* e, bool b) { return DecodeSblrContextSetDescriptorV1(d,n,v,e,b); }
}
