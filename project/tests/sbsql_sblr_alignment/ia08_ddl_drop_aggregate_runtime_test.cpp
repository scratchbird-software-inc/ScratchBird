#include "engine/sblr/sblr_ddl_drop_aggregate_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlDropAggregateDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlDropAggregateDescriptorV1(d,true);assert(b.size()==488);b[20]^=1;SblrDdlDropAggregateDescriptorV1 x;std::string e;assert(!DecodeSblrDdlDropAggregateDescriptorV1(b.data(),b.size(),&x,&e,true));return 0;}
