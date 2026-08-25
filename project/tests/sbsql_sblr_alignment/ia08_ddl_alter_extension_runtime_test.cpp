#include "engine/sblr/sblr_ddl_alter_extension_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrDdlAlterExtensionRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrDdlAlterExtensionRequestV1(q);assert(b.size()==64);SblrDdlAlterExtensionRequestV1 d;assert(DecodeSblrDdlAlterExtensionRequestV1(b.data(),b.size(),&d,nullptr));b[52]=1;assert(!DecodeSblrDdlAlterExtensionRequestV1(b.data(),b.size(),&d,nullptr));SblrDdlAlterExtensionDescriptorV1 x;x.body[0]=1;auto z=EncodeSblrDdlAlterExtensionDescriptorV1(x);assert(z.size()==384);z[352]^=1;assert(!DecodeSblrDdlAlterExtensionDescriptorV1(z.data(),z.size(),&x,nullptr));return 0;}
