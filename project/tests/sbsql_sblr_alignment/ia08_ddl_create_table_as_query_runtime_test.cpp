#include "engine/sblr/sblr_ddl_create_table_as_query_runtime.hpp"
#include <array>
#include <cassert>
using namespace scratchbird::engine::sblr;
int main(){
 SblrCreateTableAsQueryRequestV1 q{};q.receipt[0]=1;q.occurrence[0]=2;q.occurrence_ordinal=7;q.flags=1;q.catalog_epoch=4;q.authority_generation=9;
 auto qb=EncodeSblrCreateTableAsQueryRequestV1(q);assert(qb.size()==64);SblrCreateTableAsQueryRequestV1 qo{};assert(DecodeSblrCreateTableAsQueryRequestV1(qb.data(),qb.size(),&qo,nullptr));qb[0]^=1;assert(!DecodeSblrCreateTableAsQueryRequestV1(qb.data(),qb.size(),&qo,nullptr));
 SblrCreateTableAsQueryDescriptorV1 d{};d.opcode=1669;d.table_uuid[0]=3;d.query_plan_uuid[0]=4;d.query_plan={1,2,3};d.columns.resize(16,5);d.authority_proof={6,7};auto db=EncodeSblrCreateTableAsQueryDescriptorV1(d);assert(db.size()==512);SblrCreateTableAsQueryDescriptorV1 dout{};assert(DecodeSblrCreateTableAsQueryDescriptorV1(db.data(),db.size(),&dout,nullptr));db[508]^=1;assert(!DecodeSblrCreateTableAsQueryDescriptorV1(db.data(),db.size(),&dout,nullptr));
 SblrCreateTableAsQueryResultV1 r{};r.status=0;r.materialization=1;r.table_uuid[0]=3;r.publication_uuid[0]=4;r.result_hash[0]=8;r.diagnostic_hash[0]=9;auto rb=EncodeSblrCreateTableAsQueryResultV1(r);assert(rb.size()==488);SblrCreateTableAsQueryResultV1 ro{};assert(DecodeSblrCreateTableAsQueryResultV1(rb.data(),rb.size(),&ro,nullptr));rb[484]^=1;assert(!DecodeSblrCreateTableAsQueryResultV1(rb.data(),rb.size(),&ro,nullptr));return 0;
}
