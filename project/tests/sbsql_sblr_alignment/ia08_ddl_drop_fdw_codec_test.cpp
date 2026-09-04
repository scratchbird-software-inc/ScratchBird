#include "engine/sblr/sblr_ddl_drop_fdw_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlDropFdwRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.procedure_occurrence=1;auto b=EncodeSblrDdlDropFdwRequestV1(q);SblrDdlDropFdwRequestV1 d;std::string x;assert(DecodeSblrDdlDropFdwRequestV1(b.data(),b.size(),&d,&x)&&d.procedure_occurrence==1);assert(!DecodeSblrDdlDropFdwRequestV1(b.data(),b.size()-1,&d,&x));return 0;}
