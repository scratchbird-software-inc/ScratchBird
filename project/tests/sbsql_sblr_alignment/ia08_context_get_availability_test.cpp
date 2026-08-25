#include "engine/sblr/sblr_context_get_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextGetDescriptorV1 d;d.availability=2;auto b=EncodeSblrContextGetDescriptorV1(d,true);SblrContextGetDescriptorV1 x;std::string e;assert(DecodeSblrContextGetDescriptorV1(b.data(),b.size(),&x,&e,true));assert(x.availability==2);return 0;}
