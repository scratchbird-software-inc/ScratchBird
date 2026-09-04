#include "engine/sblr/sblr_dml_async_insert_cancel_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDmlAsyncInsertCancelRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.cancel_occurrence=1;auto b=EncodeSblrDmlAsyncInsertCancelRequestV1(q);SblrDmlAsyncInsertCancelRequestV1 d;assert(DecodeSblrDmlAsyncInsertCancelRequestV1(b.data(),b.size(),&d,nullptr));b[16]^=1;assert(!DecodeSblrDmlAsyncInsertCancelRequestV1(b.data(),b.size(),&d,nullptr));return 0;}
