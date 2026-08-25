#include "engine/sblr/sblr_session_discard_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionDiscardRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrSessionDiscardRequestV1(q);assert(b.size()==24);SblrSessionDiscardRequestV1 x;std::string e;assert(DecodeSblrSessionDiscardRequestV1(b.data(),b.size(),&x,&e));SblrSessionDiscardDescriptorV1 d;d.availability=1;auto v=EncodeSblrSessionDiscardDescriptorV1(d,true);assert(v.size()==128);return 0;}
