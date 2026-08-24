#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api; SblrExecutorAvailabilityRowIdentity r{"engine.op.sec_drop_policy",1803,"1.0","drop_security_policy_descriptor","security_result",1}; assert(r.opcode_code==1803&&r.operand_descriptor_id=="drop_security_policy_descriptor"&&r.result_descriptor_id=="security_result"); return 0;}
