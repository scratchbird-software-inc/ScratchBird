#include "engine/sblr/sblr_ddl_alter_rewrite_rule_runtime.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::sblr;SblrDdlAlterRewriteRuleRequestV1 q;q.receipt[0]=1;q.occurrence=1;q.rule_occurrence=1;auto b=EncodeSblrDdlAlterRewriteRuleRequestV1(q);assert(b.size()==64);SblrDdlAlterRewriteRuleDescriptorV1 d;d.body[0]=1;d.availability=1;auto x=EncodeSblrDdlAlterRewriteRuleDescriptorV1(d,false);assert(x.size()==488);std::string e;assert(DecodeSblrDdlAlterRewriteRuleDescriptorV1(x.data(),x.size(),&d,&e,false));return 0;}
