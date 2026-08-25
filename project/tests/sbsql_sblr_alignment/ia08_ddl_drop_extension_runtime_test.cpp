#include "engine/sblr/sblr_ddl_drop_extension_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrDdlDropExtensionRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrDdlDropExtensionRequestV1(q);assert(b.size()==64);SblrDdlDropExtensionRequestV1 d;assert(DecodeSblrDdlDropExtensionRequestV1(b.data(),b.size(),&d,nullptr));b[52]=1;assert(!DecodeSblrDdlDropExtensionRequestV1(b.data(),b.size(),&d,nullptr));SblrDdlDropExtensionDescriptorV1 x;x.body[0]=1;auto z=EncodeSblrDdlDropExtensionDescriptorV1(x);assert(z.size()==384);z[352]^=1;assert(!DecodeSblrDdlDropExtensionDescriptorV1(z.data(),z.size(),&x,nullptr));}
