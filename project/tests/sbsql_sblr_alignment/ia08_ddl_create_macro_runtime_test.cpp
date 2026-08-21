#include "engine/sblr/sblr_ddl_create_macro_runtime.hpp"
#include <cassert>
#include <cstdint>
using namespace scratchbird::engine::sblr;
int main(){SblrDdlCreateMacroRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.macro_occurrence=1;auto qb=EncodeSblrDdlCreateMacroRequestV1(q);SblrDdlCreateMacroRequestV1 q2;assert(DecodeSblrDdlCreateMacroRequestV1(qb.data(),qb.size(),&q2,nullptr));SblrDdlCreateMacroDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlCreateMacroDescriptorV1(d,false);assert(b.size()==488);SblrDdlCreateMacroDescriptorV1 d2;assert(DecodeSblrDdlCreateMacroDescriptorV1(b.data(),b.size(),&d2,nullptr,false));b[20]^=1;assert(!DecodeSblrDdlCreateMacroDescriptorV1(b.data(),b.size(),&d2,nullptr,false));SblrDdlCreateMacroResultV1 r;r.body[0]=1;r.availability=1;r.publication_barrier[0]=1;auto rb=EncodeSblrDdlCreateMacroResultV1(r);assert(rb.size()==320);SblrDdlCreateMacroResultV1 r2;assert(DecodeSblrDdlCreateMacroResultV1(rb.data(),rb.size(),&r2,nullptr));return 0;}
