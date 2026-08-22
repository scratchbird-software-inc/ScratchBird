#include "engine/sblr/sblr_ddl_create_timeseries_value_cache_runtime.hpp"
#include <cassert>
#include <string>
using namespace scratchbird::engine::sblr;
int main(){
  SblrDdlCreateTimeseriesValueCacheRequestV1 rq{}; rq.receipt[0]=7; rq.occurrence=1; rq.cache_occurrence=2; auto rb=EncodeSblrDdlCreateTimeseriesValueCacheRequestV1(rq); SblrDdlCreateTimeseriesValueCacheRequestV1 rq2{}; std::string e; assert(DecodeSblrDdlCreateTimeseriesValueCacheRequestV1(rb.data(),rb.size(),&rq2,&e)); rb[48]=1; assert(!DecodeSblrDdlCreateTimeseriesValueCacheRequestV1(rb.data(),rb.size(),&rq2,&e));
  SblrDdlCreateTimeseriesValueCacheDescriptorV1 d{}; d.body[0]=3; d.evidence[0]=4; d.availability=1; auto db=EncodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(d,true); SblrDdlCreateTimeseriesValueCacheDescriptorV1 d2{}; assert(DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(db.data(),db.size(),&d2,&e,true)); db[408]^=1; assert(!DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(db.data(),db.size(),&d2,&e,true));
  SblrDdlCreateTimeseriesValueCacheResultV1 r{}; r.evidence[0]=5; r.availability=1; r.publication_barrier[0]=6; auto xb=EncodeSblrDdlCreateTimeseriesValueCacheResultV1(r); SblrDdlCreateTimeseriesValueCacheResultV1 r2{}; assert(DecodeSblrDdlCreateTimeseriesValueCacheResultV1(xb.data(),xb.size(),&r2,&e)); xb[256]^=1; assert(!DecodeSblrDdlCreateTimeseriesValueCacheResultV1(xb.data(),xb.size(),&r2,&e)); return 0;
}
