#include "engine/sblr/sblr_session_discard_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionDiscardDescriptorV1 d;d.availability=9;auto b=EncodeSblrSessionDiscardDescriptorV1(d,true);SblrSessionDiscardDescriptorV1 x;std::string e;assert(DecodeSblrSessionDiscardDescriptorV1(b.data(),b.size(),&x,&e,true));assert(x.availability==9);return 0;}
