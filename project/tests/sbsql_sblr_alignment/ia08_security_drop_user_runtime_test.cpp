#include "engine/sblr/sblr_sec_drop_user_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecDropUserRequestV1 q;q.occurrence=1;auto b=EncodeSblrSecDropUserRequestV1(q);assert(b.size()==64);SblrSecDropUserRequestV1 x;std::string d;assert(DecodeSblrSecDropUserRequestV1(b.data(),b.size(),&x,&d));b[0]='X';assert(!DecodeSblrSecDropUserRequestV1(b.data(),b.size(),&x,&d));return 0;}
