#include "engine/sblr/sblr_context_unset_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrContextUnsetRequestV1 q;q.receipt[0]=1;q.occurrence=1;auto b=EncodeSblrContextUnsetRequestV1(q);assert(b.size()==24);SblrContextUnsetRequestV1 x;std::string e;assert(DecodeSblrContextUnsetRequestV1(b.data(),b.size(),&x,&e));return 0;}
