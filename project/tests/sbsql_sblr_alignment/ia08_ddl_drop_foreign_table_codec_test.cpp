#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/sblr_ddl_drop_foreign_table_runtime.hpp"
#include <cassert>

int main() {
  using namespace scratchbird::engine::sblr;
  SblrDdlDropForeignTableRequestV1 request;
  request.receipt = scratchbird::tests::FixtureUuid(1154, 63).bytes;
  request.occurrence = 0x1020304050607080ULL;
  request.procedure_occurrence = 0x10203040;
  const auto bytes = EncodeSblrDdlDropForeignTableRequestV1(request);
  SblrDdlDropForeignTableRequestV1 decoded;
  std::string detail;
  assert(DecodeSblrDdlDropForeignTableRequestV1(bytes.data(), bytes.size(), &decoded, &detail));
  assert(decoded.receipt == request.receipt);
  assert(decoded.occurrence == request.occurrence);
  assert(decoded.procedure_occurrence == request.procedure_occurrence);
  assert(!DecodeSblrDdlDropForeignTableRequestV1(bytes.data(), bytes.size() - 1, &decoded, &detail));
  assert(!DecodeSblrDdlDropForeignTableRequestV1(nullptr, bytes.size(), &decoded, &detail));
  assert(!DecodeSblrDdlDropForeignTableRequestV1(bytes.data(), bytes.size(), nullptr, &detail));
  auto corrupt = bytes;
  corrupt[0] ^= 1;
  assert(!DecodeSblrDdlDropForeignTableRequestV1(corrupt.data(), corrupt.size(), &decoded, &detail));
  corrupt = bytes;
  corrupt[63] = 1;
  assert(!DecodeSblrDdlDropForeignTableRequestV1(corrupt.data(), corrupt.size(), &decoded, &detail));
  corrupt = bytes;
  corrupt[4] = 2;
  assert(!DecodeSblrDdlDropForeignTableRequestV1(corrupt.data(), corrupt.size(), &decoded, &detail));
  return 0;
}
