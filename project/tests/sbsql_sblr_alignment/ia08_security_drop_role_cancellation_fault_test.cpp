#include "engine/sblr/sblr_sec_drop_role_runtime.hpp"
#include <cassert>
#include <string>
int main(){using namespace scratchbird::engine::sblr; SblrSecDropRoleRequestV1 q; auto b=EncodeSblrSecDropRoleRequestV1(q); SblrSecDropRoleRequestV1 out; std::string d; assert(!DecodeSblrSecDropRoleRequestV1(b.data(),b.size(),&out,&d)); b[32]=1; assert(DecodeSblrSecDropRoleRequestV1(b.data(),b.size(),&out,&d)); return 0;}
