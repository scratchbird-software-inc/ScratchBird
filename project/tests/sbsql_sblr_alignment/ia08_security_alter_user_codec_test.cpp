#include "engine/sblr/sblr_sec_alter_user_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr; SblrSecAlterUserDescriptorV1 d; d.expected_generation=1; auto b=EncodeSblrSecAlterUserDescriptorV1(d,true); SblrSecAlterUserDescriptorV1 x; std::string e; assert(DecodeSblrSecAlterUserDescriptorV1(b.data(),b.size(),&x,&e,true)); assert(x.expected_generation==1); assert(!DecodeSblrSecAlterUserDescriptorV1(b.data(),b.size()-1,&x,&e,true)); return 0;}
