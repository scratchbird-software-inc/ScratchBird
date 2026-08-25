#include "engine/sblr/sblr_ddl_drop_collation_runtime.hpp"
int main(){scratchbird::engine::sblr::SblrDdlDropCollationRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;(void)scratchbird::engine::sblr::EncodeSblrDdlDropCollationRequestV1(q);return 0;}
