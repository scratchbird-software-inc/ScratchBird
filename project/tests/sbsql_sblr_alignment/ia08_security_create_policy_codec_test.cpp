#include "engine/sblr/sblr_sec_create_policy_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr; SblrSecCreatePolicyRequestV1 q;q.occurrence=1;auto b=EncodeSblrSecCreatePolicyRequestV1(q);SblrSecCreatePolicyRequestV1 x;assert(!DecodeSblrSecCreatePolicyRequestV1(b.data(),b.size()-1,&x,nullptr));SblrSecCreatePolicyDescriptorV1 d;d.availability=1;auto o=EncodeSblrSecCreatePolicyDescriptorV1(d,true);assert(o.size()==488);assert(DecodeSblrSecCreatePolicyDescriptorV1(o.data(),o.size(),&d,nullptr,true));return 0;}
