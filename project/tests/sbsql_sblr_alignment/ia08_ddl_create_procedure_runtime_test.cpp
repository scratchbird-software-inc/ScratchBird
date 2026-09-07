#include "engine/sblr/sblr_ddl_create_procedure_runtime.hpp"

int main() {
  namespace s = scratchbird::engine::sblr;
  std::string detail;
  s::SblrDdlCreateProcedureRequestV1 request;
  request.receipt[0] = 1;
  request.occurrence = request.procedure_occurrence = 1;
  auto request_bytes = s::EncodeSblrDdlCreateProcedureRequestV1(request);
  s::SblrDdlCreateProcedureRequestV1 decoded_request;
  if (request_bytes.size() != 64 ||
      !s::DecodeSblrDdlCreateProcedureRequestV1(
          request_bytes.data(), request_bytes.size(), &decoded_request,
          &detail)) return 1;
  request_bytes[44] = 1;
  if (s::DecodeSblrDdlCreateProcedureRequestV1(
          request_bytes.data(), request_bytes.size(), &decoded_request,
          &detail)) return 2;

  s::SblrDdlCreateProcedureBindRequestV2 bind_request;
  bind_request.receipt = request.receipt;
  bind_request.occurrence = bind_request.procedure_occurrence = 1;
  bind_request.name_atoms = {{"app", false}, {"do_nothing", false}};
  auto bind_bytes =
      s::EncodeSblrDdlCreateProcedureBindRequestV2(bind_request);
  s::SblrDdlCreateProcedureBindRequestV2 decoded_bind;
  if (bind_bytes.empty() ||
      !s::DecodeSblrDdlCreateProcedureBindRequestV2(
          bind_bytes.data(), bind_bytes.size(), &decoded_bind, &detail) ||
      decoded_bind.name_atoms.size() != 2) return 7;
  auto bad_bind = bind_bytes;
  bad_bind[49] = 1;
  if (s::DecodeSblrDdlCreateProcedureBindRequestV2(
          bad_bind.data(), bad_bind.size(), &decoded_bind, &detail)) return 8;
  bad_bind = bind_bytes;
  bad_bind.back() ^= 1;
  if (s::DecodeSblrDdlCreateProcedureBindRequestV2(
          bad_bind.data(), bad_bind.size(), &decoded_bind, &detail)) return 9;

  s::SblrDdlCreateProcedureDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto descriptor_bytes =
      s::EncodeSblrDdlCreateProcedureDescriptorV1(descriptor, false);
  s::SblrDdlCreateProcedureDescriptorV1 decoded_descriptor;
  if (descriptor_bytes.size() != 488 ||
      !s::DecodeSblrDdlCreateProcedureDescriptorV1(
          descriptor_bytes.data(), descriptor_bytes.size(),
          &decoded_descriptor, &detail, false)) return 3;
  auto operand =
      s::EncodeSblrDdlCreateProcedureDescriptorV1(decoded_descriptor, true);
  operand[416] ^= 1;
  if (s::DecodeSblrDdlCreateProcedureDescriptorV1(
          operand.data(), operand.size(), &decoded_descriptor, &detail, true))
    return 4;

  s::SblrDdlCreateProcedureResultV1 result;
  result.body[0] = 1;
  result.body[24] = 1;
  result.body[56] = 1;
  result.availability = 1;
  result.publication_barrier[0] = 1;
  auto result_bytes = s::EncodeSblrDdlCreateProcedureResultV1(result);
  s::SblrDdlCreateProcedureResultV1 decoded_result;
  if (result_bytes.size() != 320 ||
      !s::DecodeSblrDdlCreateProcedureResultV1(
          result_bytes.data(), result_bytes.size(), &decoded_result,
          &detail)) return 5;
  result_bytes[256] ^= 1;
  return s::DecodeSblrDdlCreateProcedureResultV1(
             result_bytes.data(), result_bytes.size(), &decoded_result,
             &detail)
             ? 6
             : 0;
}
