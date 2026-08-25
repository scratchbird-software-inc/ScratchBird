#include "engine/sblr/sblr_session_snapshot_handle_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionSnapshotHandleDescriptorV1 d;d.availability=3;auto b=EncodeSblrSessionSnapshotHandleDescriptorV1(d,true);SblrSessionSnapshotHandleDescriptorV1 x;std::string e;assert(DecodeSblrSessionSnapshotHandleDescriptorV1(b.data(),b.size(),&x,&e,true));assert(x.availability==3);return 0;}
