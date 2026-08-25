#include "engine/sblr/sblr_stmt_prepare_runtime.hpp"
#include <array>
#include <cstdlib>
#include <string>

int main() {
  scratchbird::engine::sblr::SblrStmtPrepareDescriptorV1 value;
  value.client_statement_uuid[0] = 1;
  value.catalog_generation = 2;
  value.security_epoch = 3;
  value.policy_generation = 4;
  value.canonical_sblr_envelope = {'S','B','O','S',1,0,0,0};
  const auto encoded = scratchbird::engine::sblr::EncodeSblrStmtPrepareDescriptorV1(value);
  if (encoded.empty()) return EXIT_FAILURE;
  scratchbird::engine::sblr::SblrStmtPrepareDescriptorV1 decoded;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrStmtPrepareDescriptorV1(encoded.data(), encoded.size(), &decoded, &detail)) return EXIT_FAILURE;
  if (decoded.client_statement_uuid != value.client_statement_uuid || decoded.canonical_sblr_envelope != value.canonical_sblr_envelope) return EXIT_FAILURE;
  auto malformed = encoded;
  malformed[8] ^= 1;
  if (scratchbird::engine::sblr::DecodeSblrStmtPrepareDescriptorV1(malformed.data(), malformed.size(), &decoded, &detail)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
