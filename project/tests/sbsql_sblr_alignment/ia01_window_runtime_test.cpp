#include "engine/sblr/sblr_window_runtime.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
using namespace scratchbird::engine::sblr;
int main(){
  SblrWindowRequestV1 q; q.receipt.fill(7); q.occurrence=9; q.window_occurrence=3; auto qb=EncodeSblrWindowRequestV1(q); SblrWindowRequestV1 q2; assert(DecodeSblrWindowRequestV1(qb.data(),qb.size(),&q2,nullptr));
  SblrWindowDescriptorV1 d; d.body.fill(4); d.availability=1; auto db=EncodeSblrWindowDescriptorV1(d,false); SblrWindowDescriptorV1 d2; assert(DecodeSblrWindowDescriptorV1(db.data(),db.size(),&d2,nullptr,false)); db[408]^=1; assert(!DecodeSblrWindowDescriptorV1(db.data(),db.size(),&d2,nullptr,false));
  SblrWindowResultV1 r; r.body.fill(5); r.body[24]=1; r.body[25]=1; r.body[56]=1; r.availability=1; r.publication_barrier.fill(8); auto rb=EncodeSblrWindowResultV1(r); SblrWindowResultV1 r2; assert(DecodeSblrWindowResultV1(rb.data(),rb.size(),&r2,nullptr)); rb[256]^=1; assert(!DecodeSblrWindowResultV1(rb.data(),rb.size(),&r2,nullptr)); return 0;
}
