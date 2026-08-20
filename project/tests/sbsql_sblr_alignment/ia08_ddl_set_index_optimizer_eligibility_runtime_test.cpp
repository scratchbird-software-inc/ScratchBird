#include "engine/sblr/sblr_ddl_set_index_optimizer_eligibility_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlSetIndexOptimizerEligibilityDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlSetIndexOptimizerEligibilityDescriptorV1(d,true);assert(b.size()==488);b[20]^=1;SblrDdlSetIndexOptimizerEligibilityDescriptorV1 x;std::string e;assert(!DecodeSblrDdlSetIndexOptimizerEligibilityDescriptorV1(b.data(),b.size(),&x,&e,true));return 0;}
