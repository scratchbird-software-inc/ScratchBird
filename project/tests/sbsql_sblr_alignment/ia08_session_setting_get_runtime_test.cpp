#include "engine/sblr/sblr_session_setting_get_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionSettingGetRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrSessionSettingGetRequestV1(q);assert(b.size()==24);SblrSessionSettingGetRequestV1 x;std::string d;assert(DecodeSblrSessionSettingGetRequestV1(b.data(),b.size(),&x,&d));return 0;}
