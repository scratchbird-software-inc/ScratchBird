#include "engine/sblr/sblr_ddl_drop_type_runtime.hpp"
#include "ia08_ddl_type_fail_closed_test_support.hpp"

int main() {
  namespace s = scratchbird::engine::sblr;
  std::string detail;
  s::SblrDdlDropTypeDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto encoded = s::EncodeSblrDdlDropTypeDescriptorV1(descriptor, false);
  s::SblrDdlDropTypeDescriptorV1 decoded;
  if (encoded.size() != 488 || !s::DecodeSblrDdlDropTypeDescriptorV1(
      encoded.data(), encoded.size(), &decoded, &detail, false)) return 1;
  const auto operation = s::EncodeSblrDdlDropTypeDescriptorV1(decoded, true);
  if (operation.size() != 488 || operation[0] != 'D' || operation[1] != 'T' ||
      operation[2] != 'D' || operation[3] != 'O') return 2;
  auto malformed = operation;
  malformed[416] ^= 1;
  if (s::DecodeSblrDdlDropTypeDescriptorV1(malformed.data(), malformed.size(),
                                           &decoded, &detail, true)) return 3;
  scratchbird::tests::ia08::VerifyTypeDdlFailsClosed(
      "engine.op.ddl_drop_type", "SBLR_DDL_DROP_TYPE", 1571,
      "drop_type_descriptor", s::SblrValueKind::drop_type_descriptor,
      operation);
  return 0;
}
