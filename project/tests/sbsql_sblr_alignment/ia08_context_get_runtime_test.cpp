#include "engine/sblr/sblr_context_get_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextGetRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrContextGetRequestV1(q);assert(b.size()==24);SblrContextGetRequestV1 x;std::string e;assert(DecodeSblrContextGetRequestV1(b.data(),b.size(),&x,&e));return 0;}
