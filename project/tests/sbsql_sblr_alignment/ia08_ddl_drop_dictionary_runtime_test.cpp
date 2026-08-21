#include "engine/sblr/sblr_ddl_drop_dictionary_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrDdlDropDictionaryRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.dictionary_occurrence=1;auto qb=EncodeSblrDdlDropDictionaryRequestV1(q);assert(qb.size()==64);SblrDdlDropDictionaryRequestV1 q2;assert(DecodeSblrDdlDropDictionaryRequestV1(qb.data(),qb.size(),&q2,nullptr));SblrDdlDropDictionaryDescriptorV1 d;d.body[0]=1;d.availability=1;auto db=EncodeSblrDdlDropDictionaryDescriptorV1(d,true);assert(db.size()==488);SblrDdlDropDictionaryDescriptorV1 d2;assert(DecodeSblrDdlDropDictionaryDescriptorV1(db.data(),db.size(),&d2,nullptr,true));db[416]^=1;assert(!DecodeSblrDdlDropDictionaryDescriptorV1(db.data(),db.size(),&d2,nullptr,true));return 0;}
