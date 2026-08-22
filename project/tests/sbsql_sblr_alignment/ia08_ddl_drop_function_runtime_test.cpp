#include "engine/sblr/sblr_ddl_drop_function_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlDropFunctionDescriptorV1 d;d.body[0]=1;d.availability=1;auto b=EncodeSblrDdlDropFunctionDescriptorV1(d,true);assert(b.size()==488);b[20]^=1;SblrDdlDropFunctionDescriptorV1 x;std::string e;assert(!DecodeSblrDdlDropFunctionDescriptorV1(b.data(),b.size(),&x,&e,true));return 0;}
