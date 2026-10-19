#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/sblr_ddl_drop_fdw_runtime.hpp"
#include <cassert>

int main() {
  using namespace scratchbird::engine::sblr;
  SblrDdlDropFdwRequestV1 request;
  request.receipt = scratchbird::tests::FixtureUuid(1154, 63).bytes;
  request.occurrence = 0x1020304050607080ULL;
  request.procedure_occurrence = 0x10203040;
  const auto bytes = EncodeSblrDdlDropFdwRequestV1(request);
  SblrDdlDropFdwRequestV1 decoded;
  std::string detail;
  assert(DecodeSblrDdlDropFdwRequestV1(bytes.data(), bytes.size(), &decoded, &detail));
  assert(decoded.receipt == request.receipt);
  assert(decoded.occurrence == request.occurrence);
  assert(decoded.procedure_occurrence == request.procedure_occurrence);
  assert(!DecodeSblrDdlDropFdwRequestV1(bytes.data(), bytes.size() - 1, &decoded, &detail));
  assert(!DecodeSblrDdlDropFdwRequestV1(nullptr, bytes.size(), &decoded, &detail));
  assert(!DecodeSblrDdlDropFdwRequestV1(bytes.data(), bytes.size(), nullptr, &detail));
  auto corrupt = bytes;
  corrupt[0] ^= 1;
  assert(!DecodeSblrDdlDropFdwRequestV1(corrupt.data(), corrupt.size(), &decoded, &detail));
  corrupt = bytes;
  corrupt[63] = 1;
  assert(!DecodeSblrDdlDropFdwRequestV1(corrupt.data(), corrupt.size(), &decoded, &detail));
  corrupt = bytes;
  corrupt[4] = 2;
  assert(!DecodeSblrDdlDropFdwRequestV1(corrupt.data(), corrupt.size(), &decoded, &detail));
  return 0;
}
