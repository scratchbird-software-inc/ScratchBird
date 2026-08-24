#include "engine/sblr/sblr_sec_drop_group_mapping_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr; SblrSecDropGroupMappingDescriptorV1 d; d.availability=7; auto b=EncodeSblrSecDropGroupMappingDescriptorV1(d,true); SblrSecDropGroupMappingDescriptorV1 x; std::string e; assert(DecodeSblrSecDropGroupMappingDescriptorV1(b.data(),b.size(),&x,&e,true)); assert(!DecodeSblrSecDropGroupMappingDescriptorV1(b.data(),b.size()-1,&x,&e,true)); return 0;}
