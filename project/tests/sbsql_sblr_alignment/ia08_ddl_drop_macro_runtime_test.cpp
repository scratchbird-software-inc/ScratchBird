#include "engine/sblr/sblr_ddl_drop_macro_runtime.hpp"
#include <cassert>

using namespace scratchbird::engine::sblr;

int main() {
  SblrDdlDropMacroRequestV1 request;
  request.receipt[0] = 1;
  request.occurrence = 1;
  request.macro_occurrence = 1;
  const auto request_bytes = EncodeSblrDdlDropMacroRequestV1(request);
  SblrDdlDropMacroRequestV1 decoded_request;
  assert(DecodeSblrDdlDropMacroRequestV1(request_bytes.data(), request_bytes.size(), &decoded_request, nullptr));

  SblrDdlDropMacroDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto descriptor_bytes = EncodeSblrDdlDropMacroDescriptorV1(descriptor, false);
  assert(descriptor_bytes.size() == 488);
  SblrDdlDropMacroDescriptorV1 decoded_descriptor;
  assert(DecodeSblrDdlDropMacroDescriptorV1(descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, nullptr, false));
  descriptor_bytes[20] ^= 1;
  assert(!DecodeSblrDdlDropMacroDescriptorV1(descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, nullptr, false));
  descriptor_bytes = EncodeSblrDdlDropMacroDescriptorV1(descriptor, false);
  descriptor_bytes[416] ^= 1;
  assert(!DecodeSblrDdlDropMacroDescriptorV1(descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, nullptr, false));
  descriptor_bytes = EncodeSblrDdlDropMacroDescriptorV1(descriptor, false);
  descriptor_bytes[448] = 0;
  assert(!DecodeSblrDdlDropMacroDescriptorV1(descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, nullptr, false));
  descriptor_bytes = EncodeSblrDdlDropMacroDescriptorV1(descriptor, false);
  descriptor_bytes[456] = 1;
  assert(!DecodeSblrDdlDropMacroDescriptorV1(descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, nullptr, false));

  SblrDdlDropMacroResultV1 result;
  result.body[0] = 1;
  result.availability = 1;
  result.publication_barrier[0] = 1;
  auto result_bytes = EncodeSblrDdlDropMacroResultV1(result);
  assert(result_bytes.size() == 320);
  SblrDdlDropMacroResultV1 decoded_result;
  assert(DecodeSblrDdlDropMacroResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, nullptr));
  result_bytes[20] ^= 1;
  assert(!DecodeSblrDdlDropMacroResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, nullptr));
  result_bytes = EncodeSblrDdlDropMacroResultV1(result);
  result_bytes[264] ^= 1;
  assert(!DecodeSblrDdlDropMacroResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, nullptr));
  return 0;
}
