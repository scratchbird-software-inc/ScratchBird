#include "engine/sblr/sblr_ddl_create_operator_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrDdlCreateOperatorRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrDdlCreateOperatorRequestV1(q);assert(b.size()==64);SblrDdlCreateOperatorRequestV1 q2;assert(DecodeSblrDdlCreateOperatorRequestV1(b.data(),b.size(),&q2,nullptr));SblrDdlCreateOperatorDescriptorV1 d;d.body[0]=1;auto db=EncodeSblrDdlCreateOperatorDescriptorV1(d);assert(db.size()==384);SblrDdlCreateOperatorDescriptorV1 d2;assert(DecodeSblrDdlCreateOperatorDescriptorV1(db.data(),db.size(),&d2,nullptr));db[352]^=1;assert(!DecodeSblrDdlCreateOperatorDescriptorV1(db.data(),db.size(),&d2,nullptr));return 0;}
