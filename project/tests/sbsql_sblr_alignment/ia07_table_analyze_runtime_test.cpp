#include "engine/sblr/sblr_table_analyze_runtime.hpp"

int main() {
  namespace s = scratchbird::engine::sblr;
  std::string detail;
  s::SblrTableAnalyzeRequestV1 request;
  request.receipt[0] = 1;
  request.occurrence = request.analyze_occurrence = 1;
  auto request_bytes = s::EncodeSblrTableAnalyzeRequestV1(request);
  s::SblrTableAnalyzeRequestV1 decoded_request;
  if (request_bytes.size() != 64 || !s::DecodeSblrTableAnalyzeRequestV1(request_bytes.data(), request_bytes.size(), &decoded_request, &detail)) return 1;
  request_bytes[44] = 1;
  if (s::DecodeSblrTableAnalyzeRequestV1(request_bytes.data(), request_bytes.size(), &decoded_request, &detail)) return 2;
  s::SblrTableAnalyzeDescriptorV1 descriptor;
  descriptor.canonical_body[0] = 1;
  descriptor.availability_generation = 1;
  auto descriptor_bytes = s::EncodeSblrTableAnalyzeDescriptorV1(descriptor, false);
  s::SblrTableAnalyzeDescriptorV1 decoded_descriptor;
  if (descriptor_bytes.size() != 424 || !s::DecodeSblrTableAnalyzeDescriptorV1(descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, &detail, false)) return 3;
  auto operand = s::EncodeSblrTableAnalyzeDescriptorV1(decoded_descriptor, true);
  if (operand.size() != 424 || !s::DecodeSblrTableAnalyzeDescriptorV1(operand.data(), operand.size(), &decoded_descriptor, &detail, true)) return 4;
  operand[384] ^= 1;
  if (s::DecodeSblrTableAnalyzeDescriptorV1(operand.data(), operand.size(), &decoded_descriptor, &detail, true)) return 5;
  s::SblrTableAnalyzeResultV1 result;
  result.canonical_body[0] = 1;
  result.availability_generation = 1;
  auto result_bytes = s::EncodeSblrTableAnalyzeResultV1(result);
  s::SblrTableAnalyzeResultV1 decoded_result;
  if (result_bytes.size() != 192 || !s::DecodeSblrTableAnalyzeResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, &detail)) return 6;
  result_bytes[152] ^= 1;
  return s::DecodeSblrTableAnalyzeResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, &detail) ? 7 : 0;
}
