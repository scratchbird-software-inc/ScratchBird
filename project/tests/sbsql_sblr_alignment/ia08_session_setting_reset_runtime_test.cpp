#include "engine/sblr/sblr_session_setting_reset_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionSettingResetRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrSessionSettingResetRequestV1(q);assert(b.size()==24);SblrSessionSettingResetRequestV1 x;std::string d;assert(DecodeSblrSessionSettingResetRequestV1(b.data(),b.size(),&x,&d));return 0;}
