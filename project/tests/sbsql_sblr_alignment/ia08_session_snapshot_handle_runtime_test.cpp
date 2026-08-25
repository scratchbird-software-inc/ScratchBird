#include "engine/sblr/sblr_session_snapshot_handle_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionSnapshotHandleRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrSessionSnapshotHandleRequestV1(q);assert(b.size()==24);SblrSessionSnapshotHandleRequestV1 x;std::string e;assert(DecodeSblrSessionSnapshotHandleRequestV1(b.data(),b.size(),&x,&e));return 0;}
