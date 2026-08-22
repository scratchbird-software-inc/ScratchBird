#include "engine/sblr/sblr_dml_conditional_mutate_runtime.hpp"

#include <cassert>

int main() {
  using namespace scratchbird::engine::sblr;
  SblrDmlConditionalMutateRequestV1 request;
  request.receipt[0] = 0x71;
  request.occurrence = 1;
  request.mutation_occurrence = 2;
  auto request_wire = EncodeSblrDmlConditionalMutateRequestV1(request);
  SblrDmlConditionalMutateRequestV1 decoded_request;
  assert(DecodeSblrDmlConditionalMutateRequestV1(
      request_wire.data(), request_wire.size(), &decoded_request, nullptr));
  request_wire[16] ^= 0x01;
  assert(!DecodeSblrDmlConditionalMutateRequestV1(
      request_wire.data(), request_wire.size(), &decoded_request, nullptr));

  SblrDmlConditionalMutateDescriptorV1 descriptor;
  descriptor.body[0] = 0x11;
  descriptor.evidence[0] = 0x22;
  descriptor.availability = 1;
  auto descriptor_wire = EncodeSblrDmlConditionalMutateDescriptorV1(descriptor, true);
  SblrDmlConditionalMutateDescriptorV1 decoded_descriptor;
  assert(DecodeSblrDmlConditionalMutateDescriptorV1(
      descriptor_wire.data(), descriptor_wire.size(), &decoded_descriptor, nullptr, true));
  descriptor_wire[416] ^= 0x01;
  assert(!DecodeSblrDmlConditionalMutateDescriptorV1(
      descriptor_wire.data(), descriptor_wire.size(), &decoded_descriptor, nullptr, true));

  SblrDmlConditionalMutateResultV1 result;
  result.evidence[0] = 0x33;
  result.availability = 1;
  result.publication_barrier[0] = 0x44;
  auto result_wire = EncodeSblrDmlConditionalMutateResultV1(result);
  SblrDmlConditionalMutateResultV1 decoded_result;
  assert(DecodeSblrDmlConditionalMutateResultV1(
      result_wire.data(), result_wire.size(), &decoded_result, nullptr));
  result_wire[256] ^= 0x01;
  assert(!DecodeSblrDmlConditionalMutateResultV1(
      result_wire.data(), result_wire.size(), &decoded_result, nullptr));
  return 0;
}
