#include "engine/sblr/sblr_sec_alter_role_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecAlterRoleRequestV1 q;q.occurrence=1;auto b=EncodeSblrSecAlterRoleRequestV1(q);SblrSecAlterRoleRequestV1 o;std::string d;assert(DecodeSblrSecAlterRoleRequestV1(b.data(),b.size(),&o,&d));SblrSecAlterRoleDescriptorV1 x;x.availability=1;auto p=EncodeSblrSecAlterRoleDescriptorV1(x,true);assert(DecodeSblrSecAlterRoleDescriptorV1(p.data(),p.size(),&x,&d,true));return 0;}
