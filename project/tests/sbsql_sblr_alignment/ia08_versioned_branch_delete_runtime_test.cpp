#include "engine/sblr/sblr_versioned_branch_delete_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrVersionedBranchDeleteRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrVersionedBranchDeleteRequestV1(q);assert(b.size()==64);SblrVersionedBranchDeleteRequestV1 q2;std::string e;assert(DecodeSblrVersionedBranchDeleteRequestV1(b.data(),b.size(),&q2,&e));b[52]=1;assert(!DecodeSblrVersionedBranchDeleteRequestV1(b.data(),b.size(),&q2,&e));SblrVersionedBranchDeleteDescriptorV1 d;d.body[0]=1;auto db=EncodeSblrVersionedBranchDeleteDescriptorV1(d);assert(db.size()==384);db[352]^=1;assert(!DecodeSblrVersionedBranchDeleteDescriptorV1(db.data(),db.size(),&d,&e));return 0;}
