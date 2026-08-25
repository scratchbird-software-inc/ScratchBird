#include "engine/sblr/sblr_versioned_revert_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrVersionedRevertRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrVersionedRevertRequestV1(q);assert(b.size()==64);SblrVersionedRevertRequestV1 q2;std::string e;assert(DecodeSblrVersionedRevertRequestV1(b.data(),b.size(),&q2,&e));b[52]=1;assert(!DecodeSblrVersionedRevertRequestV1(b.data(),b.size(),&q2,&e));SblrVersionedRevertDescriptorV1 d;d.body[0]=1;auto db=EncodeSblrVersionedRevertDescriptorV1(d);assert(db.size()==384);db[352]^=1;assert(!DecodeSblrVersionedRevertDescriptorV1(db.data(),db.size(),&d,&e));SblrVersionedRevertResultV1 r;r.body[0]=1;auto rb=EncodeSblrVersionedRevertResultV1(r);assert(rb.size()==384);return 0;}
