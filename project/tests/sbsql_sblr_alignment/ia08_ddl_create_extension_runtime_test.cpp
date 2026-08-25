#include "engine/sblr/sblr_ddl_create_extension_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrDdlCreateExtensionRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrDdlCreateExtensionRequestV1(q);assert(b.size()==64);b[8]^=1;SblrDdlCreateExtensionRequestV1 d;std::string e;assert(!DecodeSblrDdlCreateExtensionRequestV1(b.data(),b.size(),&d,&e));SblrDdlCreateExtensionDescriptorV1 x;x.body[0]=1;auto z=EncodeSblrDdlCreateExtensionDescriptorV1(x);assert(z.size()==384);z[352]^=1;assert(!DecodeSblrDdlCreateExtensionDescriptorV1(z.data(),z.size(),&x,&e));return 0;}
