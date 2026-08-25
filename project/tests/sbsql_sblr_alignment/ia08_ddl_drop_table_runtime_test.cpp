#include "engine/sblr/sblr_ddl_drop_table_runtime.hpp"
#include <algorithm>
#include <cstdint>
#include <string>

int main() {
  using namespace scratchbird::engine::sblr;
  SblrDdlDropTableRequestV1 request;
  request.receipt.fill(0x11);
  request.occurrence = 7;
  request.table_occurrence = 9;
  std::string detail;
  auto request_bytes = EncodeSblrDdlDropTableRequestV1(request);
  if (request_bytes.size() != 64 || request_bytes[0] != 'D' ||
      !DecodeSblrDdlDropTableRequestV1(request_bytes.data(), request_bytes.size(), &request, &detail)) return 10;
  request_bytes[44] = 1;
  if (DecodeSblrDdlDropTableRequestV1(request_bytes.data(), request_bytes.size(), &request, &detail)) return 11;

  SblrDdlDropTableDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto coordination = EncodeSblrDdlDropTableDescriptorV1(descriptor, false);
  if (coordination.size() != 488 || coordination[0] != 'T' || coordination[1] != 'B' ||
      coordination[2] != 'D' || coordination[3] != 'X') return 12;
  SblrDdlDropTableDescriptorV1 decoded;
  if (!DecodeSblrDdlDropTableDescriptorV1(coordination.data(), coordination.size(), &decoded, &detail, false)) return 13;
  auto operand = EncodeSblrDdlDropTableDescriptorV1(decoded, true);
  if (operand.size() != 488 || operand[3] != 'O') return 14;
  if (!DecodeSblrDdlDropTableDescriptorV1(operand.data(), operand.size(), &decoded, &detail, true)) return 15;
  operand[416] ^= 1;
  if (DecodeSblrDdlDropTableDescriptorV1(operand.data(), operand.size(), &decoded, &detail, true)) return 16;
  if (DecodeSblrDdlDropTableDescriptorV1(operand.data(), 487, &decoded, &detail, true)) return 17;

  SblrDdlDropTableResultV1 result;
  result.body[0] = 1;
  result.body[24] = 1;
  result.body[56] = 1;
  result.availability = 1;
  result.publication_barrier.fill(0x22);
  auto result_bytes = EncodeSblrDdlDropTableResultV1(result);
  if (result_bytes.size() != 320 || result_bytes[0] != 'D' || result_bytes[1] != 'T' ||
      result_bytes[2] != 'R' || result_bytes[3] != 'S') return 18;
  if (!DecodeSblrDdlDropTableResultV1(result_bytes.data(), result_bytes.size(), &result, &detail)) return 19;
  result_bytes[0] = 'X';
  if (DecodeSblrDdlDropTableResultV1(result_bytes.data(), result_bytes.size(), &result, &detail)) return 20;
  return 0;
}
