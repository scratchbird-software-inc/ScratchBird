#include "engine/sblr/sblr_ddl_create_aggregate_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlCreateAggregateDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlCreateAggregateDescriptorV1(d,true);assert(b.size()==488);b[20]^=1;SblrDdlCreateAggregateDescriptorV1 x;std::string e;assert(!DecodeSblrDdlCreateAggregateDescriptorV1(b.data(),b.size(),&x,&e,true));return 0;}
