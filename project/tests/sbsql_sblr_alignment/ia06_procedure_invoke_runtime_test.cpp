#include "engine/sblr/sblr_procedure_invoke_runtime.hpp"

int main() {
  namespace s = scratchbird::engine::sblr;
  std::string detail;
  s::SblrProcedureInvokeRequestV1 request;
  request.receipt[0] = 1; request.occurrence = request.invocation_occurrence = 1;
  auto request_bytes = s::EncodeSblrProcedureInvokeRequestV1(request);
  s::SblrProcedureInvokeRequestV1 decoded_request;
  if (request_bytes.size() != 64 ||
      !s::DecodeSblrProcedureInvokeRequestV1(request_bytes.data(), request_bytes.size(), &decoded_request, &detail)) return 1;
  request_bytes[44] = 1;
  if (s::DecodeSblrProcedureInvokeRequestV1(request_bytes.data(), request_bytes.size(), &decoded_request, &detail)) return 2;
  s::SblrProcedureInvokeBindRequestV2 bind_request;
  bind_request.receipt = request.receipt;
  bind_request.occurrence = bind_request.invocation_occurrence = 1;
  bind_request.name_atoms = {{"app", false}, {"do_nothing", false}};
  auto bind_bytes = s::EncodeSblrProcedureInvokeBindRequestV2(bind_request);
  s::SblrProcedureInvokeBindRequestV2 decoded_bind;
  if (bind_bytes.empty() ||
      !s::DecodeSblrProcedureInvokeBindRequestV2(
          bind_bytes.data(), bind_bytes.size(), &decoded_bind, &detail) ||
      decoded_bind.name_atoms.size() != 2) return 7;
  auto bad_bind = bind_bytes;
  bad_bind[47] = 1;
  if (s::DecodeSblrProcedureInvokeBindRequestV2(
          bad_bind.data(), bad_bind.size(), &decoded_bind, &detail)) return 8;
  bad_bind = bind_bytes;
  bad_bind.back() ^= 1;
  if (s::DecodeSblrProcedureInvokeBindRequestV2(
          bad_bind.data(), bad_bind.size(), &decoded_bind, &detail)) return 9;
  s::SblrProcedureInvokeDescriptorV1 descriptor;
  descriptor.body[0] = 1; descriptor.availability = 1;
  auto descriptor_bytes = s::EncodeSblrProcedureInvokeDescriptorV1(descriptor, false);
  s::SblrProcedureInvokeDescriptorV1 decoded_descriptor;
  if (descriptor_bytes.size() != 488 ||
      !s::DecodeSblrProcedureInvokeDescriptorV1(descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, &detail, false)) return 3;
  auto operand = s::EncodeSblrProcedureInvokeDescriptorV1(decoded_descriptor, true);
  operand[432] ^= 1;
  if (s::DecodeSblrProcedureInvokeDescriptorV1(operand.data(), operand.size(), &decoded_descriptor, &detail, true)) return 4;
  s::SblrProcedureInvokeResultV1 result;
  result.body[0] = 1; result.body[24] = 1; result.availability = 1; result.barrier[0] = 1;
  auto result_bytes = s::EncodeSblrProcedureInvokeResultV1(result);
  s::SblrProcedureInvokeResultV1 decoded_result;
  if (result_bytes.size() != 320 ||
      !s::DecodeSblrProcedureInvokeResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, &detail)) return 5;
  result_bytes[256] ^= 1;
  return s::DecodeSblrProcedureInvokeResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, &detail) ? 6 : 0;
}
