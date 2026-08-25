#include "engine/sblr/sblr_bitemporal_as_of_valid_time_runtime.hpp"
#include <cassert>
#include <cstring>
using namespace scratchbird::engine::sblr;
int main(){
  SblrBitemporalAsOfValidTimeRequestV1 q; q.operation[0]=1; q.receipt[0]=2; q.descriptor_length=384;
  auto qb=EncodeSblrBitemporalAsOfValidTimeRequestV1(q); assert(qb.size()==64);
  SblrBitemporalAsOfValidTimeRequestV1 q2; std::string e; assert(DecodeSblrBitemporalAsOfValidTimeRequestV1(qb.data(),qb.size(),&q2,&e));
  qb[52]=1; assert(!DecodeSblrBitemporalAsOfValidTimeRequestV1(qb.data(),qb.size(),&q2,&e));
  SblrBitemporalAsOfValidTimeDescriptorV1 d; d.body[0]=1; auto db=EncodeSblrBitemporalAsOfValidTimeDescriptorV1(d); assert(db.size()==384);
  db[352]^=1; assert(!DecodeSblrBitemporalAsOfValidTimeDescriptorV1(db.data(),db.size(),&d,&e));
  return 0;
}
