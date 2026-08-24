#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api; SblrExecutorAvailabilityRowIdentity r{"engine.op.sec_create_role",1794,"1.0","create_role_descriptor","security_result",1}; assert(IsAdmittedExecutorAvailabilityIdentity(r)); return 0;}
