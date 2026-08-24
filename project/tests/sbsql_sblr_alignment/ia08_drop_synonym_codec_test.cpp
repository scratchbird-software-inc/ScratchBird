#include "engine/sblr/sblr_ddl_drop_synonym_runtime.hpp"
#include <cstdlib>
#include <iostream>
using namespace scratchbird::engine::sblr;
int main() {
  SblrDdlDropSynonymRequestV1 request;
  request.receipt[0] = 0x42; request.occurrence = 7; request.procedure_occurrence = 3;
  const auto bytes = EncodeSblrDdlDropSynonymRequestV1(request);
  if (bytes.size() != 64 || std::string(bytes.begin(), bytes.begin()+4) != "DSYQ") return EXIT_FAILURE;
  SblrDdlDropSynonymRequestV1 decoded; std::string detail;
  if (!DecodeSblrDdlDropSynonymRequestV1(bytes.data(), bytes.size(), &decoded, &detail) || decoded.occurrence != 7) return EXIT_FAILURE;
  auto malformed = bytes; malformed[0] = 'X';
  if (DecodeSblrDdlDropSynonymRequestV1(malformed.data(), malformed.size(), &decoded, &detail)) return EXIT_FAILURE;
  std::cout << "ia08_drop_synonym_codec=passed\n";
}
