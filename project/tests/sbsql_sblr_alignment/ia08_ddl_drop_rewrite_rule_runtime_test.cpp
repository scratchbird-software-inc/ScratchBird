#include "engine/sblr/sblr_ddl_drop_rewrite_rule_runtime.hpp"
#include <array>
#include <cassert>
int main(){using namespace scratchbird::engine::sblr; SblrDdlDropRewriteRuleRequestV1 q{}; q.receipt[0]=1; q.occurrence=1; q.rule_occurrence=1; auto b=EncodeSblrDdlDropRewriteRuleRequestV1(q); assert(b.size()==64); SblrDdlDropRewriteRuleRequestV1 d{}; assert(DecodeSblrDdlDropRewriteRuleRequestV1(b.data(),b.size(),&d,nullptr)); b[0]^=1; assert(!DecodeSblrDdlDropRewriteRuleRequestV1(b.data(),b.size(),&d,nullptr)); return 0;}
