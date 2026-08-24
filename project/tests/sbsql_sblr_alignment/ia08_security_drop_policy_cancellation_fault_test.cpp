#include "engine/sblr/sblr_sec_drop_policy_runtime.hpp"
#include <cassert>
#include <string>
int main(){using namespace scratchbird::engine::sblr; SblrSecDropPolicyDescriptorV1 d; d.availability=1; auto b=EncodeSblrSecDropPolicyDescriptorV1(d,true); std::string e; assert(DecodeSblrSecDropPolicyDescriptorV1(b.data(),b.size(),&d,&e,true)); b[16]=0xff; assert(DecodeSblrSecDropPolicyDescriptorV1(b.data(),b.size(),&d,&e,true)); b[6]=0; assert(!DecodeSblrSecDropPolicyDescriptorV1(b.data(),b.size(),&d,&e,true)); return 0;}
