#include "engine/sblr/sblr_sec_alter_policy_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecAlterPolicyDescriptorV1 d;d.availability=1;auto b=EncodeSblrSecAlterPolicyDescriptorV1(d,true);SblrSecAlterPolicyDescriptorV1 x;std::string e;assert(DecodeSblrSecAlterPolicyDescriptorV1(b.data(),b.size(),&x,&e,true));assert(!DecodeSblrSecAlterPolicyDescriptorV1(b.data(),b.size()-1,&x,&e,true));}
