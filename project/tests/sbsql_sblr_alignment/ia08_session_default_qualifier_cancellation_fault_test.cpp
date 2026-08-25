#include "engine/sblr/sblr_session_default_qualifier_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrSessionDefaultQualifierSetDescriptorV1 d;auto b=EncodeSblrSessionDefaultQualifierSetDescriptorV1(d,true);SblrSessionDefaultQualifierSetDescriptorV1 x;std::string e;assert(!DecodeSblrSessionDefaultQualifierSetDescriptorV1(b.data(),b.size()-1,&x,&e,true));return 0;}
