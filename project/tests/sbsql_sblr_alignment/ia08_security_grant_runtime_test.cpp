#include "engine/sblr/sblr_sec_grant_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecGrantDescriptorV1 d;d.availability=1;auto b=EncodeSblrSecGrantDescriptorV1(d,true);SblrSecGrantDescriptorV1 x;std::string e;assert(DecodeSblrSecGrantDescriptorV1(b.data(),b.size(),&x,&e,true));assert(!DecodeSblrSecGrantDescriptorV1(b.data(),b.size()-1,&x,&e,true));}
