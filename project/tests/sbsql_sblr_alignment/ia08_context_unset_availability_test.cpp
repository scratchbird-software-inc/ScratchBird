#include "engine/sblr/sblr_context_unset_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextUnsetDescriptorV1 d;d.availability=2;auto b=EncodeSblrContextUnsetDescriptorV1(d,true);SblrContextUnsetDescriptorV1 x;std::string e;assert(DecodeSblrContextUnsetDescriptorV1(b.data(),b.size(),&x,&e,true));assert(x.availability==2);return 0;}
