#include "engine/sblr/sblr_ddl_alter_collation_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;int main(){SblrDdlAlterCollationRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrDdlAlterCollationRequestV1(q);assert(b.size()==64);SblrDdlAlterCollationRequestV1 q2;assert(DecodeSblrDdlAlterCollationRequestV1(b.data(),b.size(),&q2,nullptr));SblrDdlAlterCollationDescriptorV1 d;d.body[0]=1;auto x=EncodeSblrDdlAlterCollationDescriptorV1(d);assert(x.size()==384);}
