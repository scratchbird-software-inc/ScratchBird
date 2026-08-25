#include "engine/sblr/sblr_bitemporal_period_overlap_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrBitemporalPeriodOverlapRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrBitemporalPeriodOverlapRequestV1(q);assert(b.size()==64);SblrBitemporalPeriodOverlapRequestV1 q2;std::string e;assert(DecodeSblrBitemporalPeriodOverlapRequestV1(b.data(),b.size(),&q2,&e));b[52]=1;assert(!DecodeSblrBitemporalPeriodOverlapRequestV1(b.data(),b.size(),&q2,&e));SblrBitemporalPeriodOverlapDescriptorV1 d;d.body[0]=1;auto db=EncodeSblrBitemporalPeriodOverlapDescriptorV1(d);assert(db.size()==384);db[352]^=1;assert(!DecodeSblrBitemporalPeriodOverlapDescriptorV1(db.data(),db.size(),&d,&e));return 0;}
