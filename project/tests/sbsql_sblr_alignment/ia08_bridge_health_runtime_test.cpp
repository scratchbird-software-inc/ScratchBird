#include "engine/sblr/sblr_bridge_health_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr; int main(){SblrBridgeHealthRequestV1 q;q.operation[0]=1;q.receipt[0]=2;q.descriptor_length=384;auto b=EncodeSblrBridgeHealthRequestV1(q);assert(b.size()==64);SblrBridgeHealthRequestV1 q2;std::string e;assert(DecodeSblrBridgeHealthRequestV1(b.data(),b.size(),&q2,&e));SblrBridgeHealthDescriptorV1 d;d.body[0]=1;auto db=EncodeSblrBridgeHealthDescriptorV1(d);assert(db.size()==384);db[352]^=1;assert(!DecodeSblrBridgeHealthDescriptorV1(db.data(),db.size(),&d,&e));return 0;}
