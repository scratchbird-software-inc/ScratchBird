#include "engine/sblr/sblr_sec_drop_policy_runtime.hpp"
#include <cassert>
#include <string>
int main(){using namespace scratchbird::engine::sblr; SblrSecDropPolicyRequestV1 q; auto b=EncodeSblrSecDropPolicyRequestV1(q); SblrSecDropPolicyRequestV1 out; std::string d; assert(!DecodeSblrSecDropPolicyRequestV1(b.data(),b.size(),&out,&d)); b[32]=1; assert(DecodeSblrSecDropPolicyRequestV1(b.data(),b.size(),&out,&d)); SblrSecDropPolicyDescriptorV1 x; x.availability=1; auto op=EncodeSblrSecDropPolicyDescriptorV1(x,true); assert(op.size()==128); assert(DecodeSblrSecDropPolicyDescriptorV1(op.data(),op.size(),&x,&d,true)); op[6]=0; assert(!DecodeSblrSecDropPolicyDescriptorV1(op.data(),op.size(),&x,&d,true)); return 0;}
