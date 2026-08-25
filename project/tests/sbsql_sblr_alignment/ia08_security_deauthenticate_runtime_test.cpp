#include "engine/sblr/sblr_sec_deauthenticate_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecDeauthenticateRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrSecDeauthenticateRequestV1(q);assert(b.size()==64);SblrSecDeauthenticateRequestV1 x;std::string d;assert(DecodeSblrSecDeauthenticateRequestV1(b.data(),b.size(),&x,&d));return 0;}
