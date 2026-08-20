#include "engine/sblr/sblr_ddl_purge_system_history_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlPurgeSystemHistoryDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlPurgeSystemHistoryDescriptorV1(d,true);assert(b.size()==488);b[20]^=1;SblrDdlPurgeSystemHistoryDescriptorV1 x;std::string e;assert(!DecodeSblrDdlPurgeSystemHistoryDescriptorV1(b.data(),b.size(),&x,&e,true));return 0;}
