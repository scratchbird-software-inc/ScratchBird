#include "engine/sblr/sblr_context_unset_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextUnsetDescriptorV1 d;auto b=EncodeSblrContextUnsetDescriptorV1(d,true);SblrContextUnsetDescriptorV1 x;std::string e;assert(!DecodeSblrContextUnsetDescriptorV1(b.data(),b.size()-1,&x,&e,true));return 0;}
