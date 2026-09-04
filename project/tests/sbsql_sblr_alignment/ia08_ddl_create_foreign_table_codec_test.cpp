#include "engine/sblr/sblr_ddl_create_foreign_table_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlCreateForeignTableRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.table_occurrence=1;auto b=EncodeSblrDdlCreateForeignTableRequestV1(q);SblrDdlCreateForeignTableRequestV1 d;std::string x;assert(DecodeSblrDdlCreateForeignTableRequestV1(b.data(),b.size(),&d,&x)&&d.table_occurrence==1);return 0;}
