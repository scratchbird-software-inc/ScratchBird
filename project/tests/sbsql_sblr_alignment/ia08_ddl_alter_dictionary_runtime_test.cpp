#include "engine/sblr/sblr_ddl_alter_dictionary_runtime.hpp"
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){SblrDdlAlterDictionaryRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.dictionary_occurrence=1;auto qb=EncodeSblrDdlAlterDictionaryRequestV1(q);assert(qb.size()==64);SblrDdlAlterDictionaryRequestV1 q2;assert(DecodeSblrDdlAlterDictionaryRequestV1(qb.data(),qb.size(),&q2,nullptr));SblrDdlAlterDictionaryDescriptorV1 d;d.body[0]=1;d.availability=1;auto db=EncodeSblrDdlAlterDictionaryDescriptorV1(d,false);assert(db.size()==488);SblrDdlAlterDictionaryDescriptorV1 d2;assert(DecodeSblrDdlAlterDictionaryDescriptorV1(db.data(),db.size(),&d2,nullptr,false));db[20]^=1;assert(!DecodeSblrDdlAlterDictionaryDescriptorV1(db.data(),db.size(),&d2,nullptr,false));return 0;}
