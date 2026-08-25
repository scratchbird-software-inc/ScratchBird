#include "engine/sblr/sblr_session_snapshot_handle_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionSnapshotHandleDescriptorV1 d;auto b=EncodeSblrSessionSnapshotHandleDescriptorV1(d,true);SblrSessionSnapshotHandleDescriptorV1 x;std::string e;assert(!DecodeSblrSessionSnapshotHandleDescriptorV1(b.data(),b.size()-1,&x,&e,true));return 0;}
