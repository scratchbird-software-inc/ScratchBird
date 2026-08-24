#include "engine/sblr/sblr_sec_create_role_runtime.hpp"
#include <cassert>
#include <string>
int main(){using namespace scratchbird::engine::sblr; SblrSecCreateRoleRequestV1 q; auto b=EncodeSblrSecCreateRoleRequestV1(q); SblrSecCreateRoleRequestV1 out; std::string d; assert(!DecodeSblrSecCreateRoleRequestV1(b.data(),b.size(),&out,&d)); b[32]=1; assert(DecodeSblrSecCreateRoleRequestV1(b.data(),b.size(),&out,&d)); return 0;}
