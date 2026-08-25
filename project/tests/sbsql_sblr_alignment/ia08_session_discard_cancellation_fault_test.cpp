#include "engine/sblr/sblr_session_discard_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionDiscardDescriptorV1 d;auto b=EncodeSblrSessionDiscardDescriptorV1(d,true);SblrSessionDiscardDescriptorV1 x;std::string e;assert(!DecodeSblrSessionDiscardDescriptorV1(b.data(),b.size()-1,&x,&e,true));return 0;}
