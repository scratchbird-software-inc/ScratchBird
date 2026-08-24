#include "engine/sblr/sblr_ddl_drop_foreign_table_runtime.hpp"
#include <cassert>
#include <iostream>
int main(){using namespace scratchbird::engine::sblr;SblrDdlDropForeignTableRequestV1 v;v.occurrence=7;auto b=EncodeSblrDdlDropForeignTableRequestV1(v);SblrDdlDropForeignTableRequestV1 d;std::string x;assert(DecodeSblrDdlDropForeignTableRequestV1(b.data(),b.size(),&d,&x)&&d.occurrence==7);b.pop_back();assert(!DecodeSblrDdlDropForeignTableRequestV1(b.data(),b.size(),&d,&x));std::cout<<"ia08_drop_foreign_table_codec=passed\n";}
