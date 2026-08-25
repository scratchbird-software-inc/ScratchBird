#include "engine/sblr/sblr_context_set_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextSetDescriptorV1 d;d.availability=2;auto b=EncodeSblrContextSetDescriptorV1(d,true);SblrContextSetDescriptorV1 x;std::string e;assert(DecodeSblrContextSetDescriptorV1(b.data(),b.size(),&x,&e,true));assert(x.availability==2);return 0;}
