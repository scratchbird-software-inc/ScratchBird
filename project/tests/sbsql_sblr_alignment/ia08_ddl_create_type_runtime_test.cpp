#include "engine/sblr/sblr_ddl_create_type_runtime.hpp"
#include "ia08_ddl_type_fail_closed_test_support.hpp"

int main() {
  namespace sblr = scratchbird::engine::sblr;
  std::string detail;
  sblr::SblrDdlCreateTypeDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  const auto descriptor_bytes =
      sblr::EncodeSblrDdlCreateTypeDescriptorV1(descriptor, false);
  sblr::SblrDdlCreateTypeDescriptorV1 decoded;
  if (descriptor_bytes.size() != 488 ||
      !sblr::DecodeSblrDdlCreateTypeDescriptorV1(
          descriptor_bytes.data(), descriptor_bytes.size(), &decoded, &detail,
          false)) {
    return 1;
  }
  const auto execution_descriptor =
      sblr::EncodeSblrDdlCreateTypeDescriptorV1(decoded, true);
  if (execution_descriptor.size() != 488 || execution_descriptor[0] != 'T' ||
      execution_descriptor[1] != 'Y' || execution_descriptor[2] != 'D' ||
      execution_descriptor[3] != 'O') {
    return 2;
  }
  auto malformed = execution_descriptor;
  malformed[416] ^= 1;
  if (sblr::DecodeSblrDdlCreateTypeDescriptorV1(
          malformed.data(), malformed.size(), &decoded, &detail, true)) {
    return 3;
  }
  scratchbird::tests::ia08::VerifyTypeDdlFailsClosed(
      "engine.op.ddl_create_type", "SBLR_DDL_CREATE_TYPE", 1569,
      "create_type_descriptor", sblr::SblrValueKind::create_type_descriptor,
      execution_descriptor);
  return 0;
}
