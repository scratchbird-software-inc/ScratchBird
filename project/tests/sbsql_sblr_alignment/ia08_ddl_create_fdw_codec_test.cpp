#include "engine/sblr/sblr_ddl_create_fdw_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlCreateFdwRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.table_occurrence=1;auto b=EncodeSblrDdlCreateFdwRequestV1(q);SblrDdlCreateFdwRequestV1 d;std::string x;assert(DecodeSblrDdlCreateFdwRequestV1(b.data(),b.size(),&d,&x)&&d.table_occurrence==1);assert(!DecodeSblrDdlCreateFdwRequestV1(b.data(),b.size()-1,&d,&x));return 0;}
