#include "engine/sblr/sblr_ddl_create_timeseries_value_cache_runtime.hpp"
#include <algorithm>
#include <cassert>
#include <string>
using namespace scratchbird::engine::sblr;
int main(){
  SblrDdlCreateTimeseriesValueCacheRequestV1 rq{}; rq.receipt[0]=7; rq.occurrence=1; rq.cache_occurrence=2; auto rb=EncodeSblrDdlCreateTimeseriesValueCacheRequestV1(rq); SblrDdlCreateTimeseriesValueCacheRequestV1 rq2{}; std::string e; assert(DecodeSblrDdlCreateTimeseriesValueCacheRequestV1(rb.data(),rb.size(),&rq2,&e)); rb[48]=1; assert(!DecodeSblrDdlCreateTimeseriesValueCacheRequestV1(rb.data(),rb.size(),&rq2,&e));
  SblrDdlCreateTimeseriesValueCacheDescriptorV1 d{}; d.body[0]=3; d.availability=1; auto db=EncodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(d,false); SblrDdlCreateTimeseriesValueCacheDescriptorV1 d2{}; assert(DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(db.data(),db.size(),&d2,&e,false)); assert(EncodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(d2,false)==db); auto operand=EncodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(d2,true); assert(operand.size()==db.size()); assert(std::equal(db.begin()+4,db.end(),operand.begin()+4)); assert(DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(operand.data(),operand.size(),&d2,&e,true)); operand[408]^=1; assert(!DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(operand.data(),operand.size(),&d2,&e,true));
  SblrDdlCreateTimeseriesValueCacheResultV1 r{}; r.availability=1; r.publication_barrier[0]=6; auto xb=EncodeSblrDdlCreateTimeseriesValueCacheResultV1(r); SblrDdlCreateTimeseriesValueCacheResultV1 r2{}; assert(DecodeSblrDdlCreateTimeseriesValueCacheResultV1(xb.data(),xb.size(),&r2,&e)); assert(EncodeSblrDdlCreateTimeseriesValueCacheResultV1(r2)==xb); xb[256]^=1; assert(!DecodeSblrDdlCreateTimeseriesValueCacheResultV1(xb.data(),xb.size(),&r2,&e)); return 0;
}
