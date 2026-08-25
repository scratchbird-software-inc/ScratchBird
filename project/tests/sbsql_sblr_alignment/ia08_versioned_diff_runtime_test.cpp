#include "engine/sblr/sblr_versioned_diff_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrVersionedDiffRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrVersionedDiffRequestV1(q);assert(b.size()==64);SblrVersionedDiffRequestV1 q2;std::string e;assert(DecodeSblrVersionedDiffRequestV1(b.data(),b.size(),&q2,&e));b[52]=1;assert(!DecodeSblrVersionedDiffRequestV1(b.data(),b.size(),&q2,&e));SblrVersionedDiffDescriptorV1 d;d.body[0]=1;auto db=EncodeSblrVersionedDiffDescriptorV1(d);assert(db.size()==384);db[352]^=1;assert(!DecodeSblrVersionedDiffDescriptorV1(db.data(),db.size(),&d,&e));return 0;}
