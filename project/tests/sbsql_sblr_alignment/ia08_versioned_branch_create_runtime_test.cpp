#include "engine/sblr/sblr_versioned_branch_create_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrVersionedBranchCreateRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrVersionedBranchCreateRequestV1(q);assert(b.size()==64);SblrVersionedBranchCreateRequestV1 q2;std::string e;assert(DecodeSblrVersionedBranchCreateRequestV1(b.data(),b.size(),&q2,&e));b[52]=1;assert(!DecodeSblrVersionedBranchCreateRequestV1(b.data(),b.size(),&q2,&e));SblrVersionedBranchCreateDescriptorV1 d;d.body[0]=1;auto db=EncodeSblrVersionedBranchCreateDescriptorV1(d);assert(db.size()==384);db[352]^=1;assert(!DecodeSblrVersionedBranchCreateDescriptorV1(db.data(),db.size(),&d,&e));return 0;}
