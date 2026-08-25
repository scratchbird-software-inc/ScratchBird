#include "engine/sblr/sblr_session_role_switch_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionRoleSwitchRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrSessionRoleSwitchRequestV1(q);assert(b.size()==64);SblrSessionRoleSwitchRequestV1 x;std::string d;assert(DecodeSblrSessionRoleSwitchRequestV1(b.data(),b.size(),&x,&d));return 0;}
