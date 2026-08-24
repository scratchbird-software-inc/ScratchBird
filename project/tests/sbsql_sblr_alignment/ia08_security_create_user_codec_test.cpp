#include "engine/sblr/sblr_security_create_user_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSecurityCreateUserRequestV1 q;q.template_occurrence=1;auto b=EncodeSblrSecurityCreateUserRequestV1(q);SblrSecurityCreateUserRequestV1 d;std::string x;assert(DecodeSblrSecurityCreateUserRequestV1(b.data(),b.size(),&d,&x)&&d.template_occurrence==1);assert(!DecodeSblrSecurityCreateUserRequestV1(b.data(),b.size()-1,&d,&x));return 0;}
