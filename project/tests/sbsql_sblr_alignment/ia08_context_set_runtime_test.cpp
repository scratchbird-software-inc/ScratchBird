#include "engine/sblr/sblr_context_set_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextSetRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrContextSetRequestV1(q);assert(b.size()==24);SblrContextSetRequestV1 x;std::string e;assert(DecodeSblrContextSetRequestV1(b.data(),b.size(),&x,&e));return 0;}
