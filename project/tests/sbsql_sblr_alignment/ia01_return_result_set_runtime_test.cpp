#include "engine/sblr/sblr_return_result_set_runtime.hpp"
#include <array>
int main() {
  namespace s = scratchbird::engine::sblr;
  s::SblrReturnResultSetRequestV1 q; q.receipt[0]=1; q.occurrence=1; q.return_occurrence=1;
  auto qb=s::EncodeSblrReturnResultSetRequestV1(q); s::SblrReturnResultSetRequestV1 qo; std::string d;
  if(qb.size()!=64 || !s::DecodeSblrReturnResultSetRequestV1(qb.data(),qb.size(),&qo,&d)) return 1;
  qb[44]=1; if(s::DecodeSblrReturnResultSetRequestV1(qb.data(),qb.size(),&qo,&d)) return 2;
  s::SblrReturnResultSetDescriptorV1 x; x.body[0]=1; x.availability=1;
  auto db=s::EncodeSblrReturnResultSetDescriptorV1(x,false); s::SblrReturnResultSetDescriptorV1 xo;
  if(db.size()!=488 || !s::DecodeSblrReturnResultSetDescriptorV1(db.data(),db.size(),&xo,&d,false)) return 3;
  auto ob=s::EncodeSblrReturnResultSetDescriptorV1(xo,true); ob[376]^=1;
  if(s::DecodeSblrReturnResultSetDescriptorV1(ob.data(),ob.size(),&xo,&d,true)) return 4;
  s::SblrReturnResultSetResultV1 r; r.body[24]=1; r.body[25]=1; r.body[32]=1; r.availability=1; r.publication_barrier[0]=1;
  auto rb=s::EncodeSblrReturnResultSetResultV1(r); s::SblrReturnResultSetResultV1 ro;
  if(rb.size()!=256 || !s::DecodeSblrReturnResultSetResultV1(rb.data(),rb.size(),&ro,&d)) return 5;
  rb[184]^=1; return s::DecodeSblrReturnResultSetResultV1(rb.data(),rb.size(),&ro,&d)?6:0;
}
