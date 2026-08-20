#include "engine/sblr/sblr_atomic_read_modify_write_runtime.hpp"

int main() {
  namespace s = scratchbird::engine::sblr;
  std::string detail;
  s::SblrAtomicRmwRequestV1 request;
  request.receipt[0] = 1;
  request.occurrence = 1;
  request.rmw_occurrence = 1;
  auto request_bytes = s::EncodeSblrAtomicRmwRequestV1(request);
  s::SblrAtomicRmwRequestV1 decoded_request;
  if (request_bytes.size() != 64 || !s::DecodeSblrAtomicRmwRequestV1(
          request_bytes.data(), request_bytes.size(), &decoded_request, &detail)) return 1;
  request_bytes[44] = 1;
  if (s::DecodeSblrAtomicRmwRequestV1(request_bytes.data(), request_bytes.size(),
                                     &decoded_request, &detail)) return 2;

  s::SblrAtomicRmwDescriptorV1 descriptor;
  descriptor.canonical_body[0] = 1;
  descriptor.availability_generation = 1;
  auto descriptor_bytes = s::EncodeSblrAtomicRmwDescriptorV1(descriptor, false);
  s::SblrAtomicRmwDescriptorV1 decoded_descriptor;
  if (descriptor_bytes.size() != 488 || !s::DecodeSblrAtomicRmwDescriptorV1(
          descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor,
          &detail, false)) return 3;
  auto operand_bytes = s::EncodeSblrAtomicRmwDescriptorV1(decoded_descriptor, true);
  if (operand_bytes.size() != 488 || !s::DecodeSblrAtomicRmwDescriptorV1(
          operand_bytes.data(), operand_bytes.size(), &decoded_descriptor,
          &detail, true)) return 4;
  operand_bytes[448] ^= 1;
  if (s::DecodeSblrAtomicRmwDescriptorV1(operand_bytes.data(), operand_bytes.size(),
                                        &decoded_descriptor, &detail, true)) return 5;

  s::SblrAtomicRmwResultV1 result;
  result.canonical_body[0] = 1;
  result.availability_generation = 1;
  auto result_bytes = s::EncodeSblrAtomicRmwResultV1(result);
  s::SblrAtomicRmwResultV1 decoded_result;
  if (result_bytes.size() != 224 || !s::DecodeSblrAtomicRmwResultV1(
          result_bytes.data(), result_bytes.size(), &decoded_result, &detail)) return 6;
  result_bytes[184] ^= 1;
  return s::DecodeSblrAtomicRmwResultV1(result_bytes.data(), result_bytes.size(),
                                        &decoded_result, &detail) ? 7 : 0;
}
