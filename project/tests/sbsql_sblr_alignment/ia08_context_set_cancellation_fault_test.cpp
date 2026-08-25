#include "engine/sblr/sblr_context_set_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextSetDescriptorV1 d;auto b=EncodeSblrContextSetDescriptorV1(d,true);SblrContextSetDescriptorV1 x;std::string e;assert(!DecodeSblrContextSetDescriptorV1(b.data(),b.size()-1,&x,&e,true));return 0;}
