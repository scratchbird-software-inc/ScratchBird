#include "engine/sblr/sblr_ddl_create_publication_runtime.hpp"
#include <cassert>

using namespace scratchbird::engine::sblr;

int main() {
  SblrDdlCreatePublicationRequestV1 request;
  request.operation[0] = 1;
  request.receipt[0] = 2;
  auto encoded_request = EncodeSblrDdlCreatePublicationRequestV1(request);
  assert(encoded_request.size() == 64);
  SblrDdlCreatePublicationRequestV1 decoded_request;
  assert(DecodeSblrDdlCreatePublicationRequestV1(
      encoded_request.data(), encoded_request.size(), &decoded_request, nullptr));
  assert(!DecodeSblrDdlCreatePublicationRequestV1(
      encoded_request.data(), encoded_request.size() - 1, &decoded_request, nullptr));

  SblrDdlCreatePublicationDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  auto encoded_descriptor = EncodeSblrDdlCreatePublicationDescriptorV1(descriptor);
  assert(encoded_descriptor.size() == 320);
  assert(!DecodeSblrDdlCreatePublicationDescriptorV1(
      encoded_descriptor.data(), encoded_descriptor.size() + 1, &descriptor, nullptr));
  SblrDdlCreatePublicationResultV1 result;
  result.body[0] = 1;
  auto encoded_result = EncodeSblrDdlCreatePublicationResultV1(result);
  assert(encoded_result.size() == 192);
  assert(!DecodeSblrDdlCreatePublicationResultV1(
      encoded_result.data(), encoded_result.size() - 1, &result, nullptr));
  return 0;
}
