#include "engine/sblr/sblr_ddl_alter_aggregate_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlAlterAggregateDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlAlterAggregateDescriptorV1(d,true);assert(b.size()==488);b[20]^=1;SblrDdlAlterAggregateDescriptorV1 x;std::string e;assert(!DecodeSblrDdlAlterAggregateDescriptorV1(b.data(),b.size(),&x,&e,true));return 0;}
