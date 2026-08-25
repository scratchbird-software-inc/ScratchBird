#include "engine/sblr/sblr_sec_authenticate_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecAuthenticateRequestV1 q;q.occurrence=1;auto b=EncodeSblrSecAuthenticateRequestV1(q);assert(b.size()==64);SblrSecAuthenticateRequestV1 x;std::string d;assert(DecodeSblrSecAuthenticateRequestV1(b.data(),b.size(),&x,&d));return 0;}
