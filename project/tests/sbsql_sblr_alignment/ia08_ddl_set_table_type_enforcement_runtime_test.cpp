#include "engine/sblr/sblr_ddl_set_table_type_enforcement_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlSetTableTypeEnforcementDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlSetTableTypeEnforcementDescriptorV1(d,true);assert(b.size()==488);b[20]^=1;SblrDdlSetTableTypeEnforcementDescriptorV1 x;std::string e;assert(!DecodeSblrDdlSetTableTypeEnforcementDescriptorV1(b.data(),b.size(),&x,&e,true));return 0;}
