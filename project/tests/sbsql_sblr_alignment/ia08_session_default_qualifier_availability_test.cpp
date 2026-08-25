#include "engine/sblr/sblr_session_default_qualifier_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionDefaultQualifierSetDescriptorV1 d;d.availability=7;auto b=EncodeSblrSessionDefaultQualifierSetDescriptorV1(d,true);SblrSessionDefaultQualifierSetDescriptorV1 x;std::string e;assert(DecodeSblrSessionDefaultQualifierSetDescriptorV1(b.data(),b.size(),&x,&e,true));assert(x.availability==7);return 0;}
