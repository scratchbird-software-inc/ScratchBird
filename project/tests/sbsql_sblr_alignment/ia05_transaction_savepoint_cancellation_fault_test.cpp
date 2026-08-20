#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_savepoint_runtime.hpp"

#include <atomic>

namespace s = scratchbird::engine::sblr;

int main() {
  s::SblrSavepointDescriptorV1 descriptor;
  descriptor.descriptor_uuid[0]=1; descriptor.descriptor_generation=1;
  descriptor.savepoint_uuid[0]=2; descriptor.savepoint_generation=2;
  descriptor.transaction_uuid[0]=3; descriptor.local_transaction_id=4;
  descriptor.transaction_ordinal=1; descriptor.descriptor_evidence_sha256[0]=5;
  auto envelope=s::MakeSblrEnvelope("engine.op.txn_savepoint","SBLR_TXN_SAVEPOINT",
                                    "ia05.txn_savepoint.cancel");
  envelope.opcode_code=259;envelope.result_shape="savepoint_handle";
  envelope.diagnostic_shape="diagnostic_vector";
  envelope.parser_package_uuid="019d0000-0000-7000-8000-000000000360";
  envelope.registry_snapshot_uuid="019d0000-0000-7000-8000-000000000361";
  envelope.parser_resolved_names_to_uuids=true;
  s::SblrOperand operand;operand.ordinal=1;operand.type="savepoint.descriptor";
  operand.name="savepoint";operand.value_kind=s::SblrValueKind::savepoint_descriptor;
  operand.value_body=s::EncodeSblrSavepointDescriptorV1(descriptor);
  envelope.operands.push_back(std::move(operand));
  if(!s::ValidateSblrEnvelope(envelope).ok)return 1;
  std::atomic<unsigned> checks{0};
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present=true;
  context.query_cancellation_requested=[&]{++checks;return true;};
  const auto dispatched=s::DispatchSblrOperation({context,std::move(envelope),{},std::nullopt});
  if(dispatched.accepted||dispatched.api_result.ok||!dispatched.api_result.evidence.empty()||
     checks!=1||dispatched.api_result.diagnostics.empty()||
     dispatched.api_result.diagnostics.front().code!="PROCESS.CANCELLED"||
     dispatched.api_result.diagnostics.front().message_key!=
       "sblr.txn_savepoint.cancelled_before_stack_push")return 2;
  return 0;
}
