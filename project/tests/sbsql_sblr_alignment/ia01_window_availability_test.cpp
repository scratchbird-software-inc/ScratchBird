#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){ SblrExecutorAvailabilityRowIdentity r{}; r.executor_id="engine.op.window"; r.opcode_code=1285; r.opcode_version="1.0"; r.operand_descriptor_id="window_descriptor"; r.result_descriptor_id="rowset_descriptor"; r.result_descriptor_version=1; assert(IsAdmittedExecutorAvailabilityIdentity(r)); r.opcode_code=1284; assert(!IsAdmittedExecutorAvailabilityIdentity(r)); return 0; }
