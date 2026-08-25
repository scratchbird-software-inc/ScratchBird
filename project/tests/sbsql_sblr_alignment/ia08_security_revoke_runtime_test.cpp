#include "engine/sblr/sblr_sec_revoke_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecRevokeDescriptorV1 d;d.availability=1;auto b=EncodeSblrSecRevokeDescriptorV1(d,true);SblrSecRevokeDescriptorV1 x;std::string e;assert(DecodeSblrSecRevokeDescriptorV1(b.data(),b.size(),&x,&e,true));assert(!DecodeSblrSecRevokeDescriptorV1(b.data(),b.size()-1,&x,&e,true));}
