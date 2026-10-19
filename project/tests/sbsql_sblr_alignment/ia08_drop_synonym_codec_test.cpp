#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/sblr_ddl_drop_synonym_runtime.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

using namespace scratchbird::engine::sblr;
namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}

int main() {
  SblrDdlDropSynonymRequestV1 request;
  request.receipt = scratchbird::tests::FixtureUuid(1154, 64).bytes;
  request.occurrence = 0x1020304050607080ULL;
  request.procedure_occurrence = 0x10203040;
  const auto bytes = EncodeSblrDdlDropSynonymRequestV1(request);
  Check(bytes.size() == 64 && std::equal(bytes.begin(), bytes.begin() + 4, "DSYQ"),
        "synonym request wire identity");
  SblrDdlDropSynonymRequestV1 decoded;
  std::string detail;
  Check(DecodeSblrDdlDropSynonymRequestV1(bytes.data(), bytes.size(), &decoded, &detail) &&
            decoded.receipt == request.receipt && decoded.occurrence == request.occurrence &&
            decoded.procedure_occurrence == request.procedure_occurrence,
        "synonym request must preserve binary identity and full-width ordinals");
  for (std::size_t length = 0; length < bytes.size(); ++length)
    Check(!DecodeSblrDdlDropSynonymRequestV1(bytes.data(), length, &decoded, &detail),
          "truncated synonym request accepted");
  Check(!DecodeSblrDdlDropSynonymRequestV1(nullptr, bytes.size(), &decoded, &detail) &&
            !DecodeSblrDdlDropSynonymRequestV1(bytes.data(), bytes.size(), nullptr, &detail),
        "null synonym request or output accepted");
  Check(!DecodeSblrDdlDropSynonymRequestV1(bytes.data(), std::numeric_limits<std::size_t>::max(),
                                         &decoded, &detail), "oversized request copied before framing validation");
  for (const auto magic : {"PCQX", "PDQX", "DFTQ", "FDXQ"}) {
    auto wrong_operation = bytes;
    std::copy_n(magic, 4, wrong_operation.begin());
    Check(!DecodeSblrDdlDropSynonymRequestV1(wrong_operation.data(), wrong_operation.size(),
                                           &decoded, &detail),
          "foreign operation accepted as a synonym request");
  }
  for (const auto offset : {4, 6, 8, 12, 44, 63}) {
    auto malformed = bytes;
    malformed[offset] ^= 1;
    Check(!DecodeSblrDdlDropSynonymRequestV1(malformed.data(), malformed.size(), &decoded, &detail),
          "invalid synonym version, length or reserved bytes accepted");
  }
  SblrDdlDropSynonymDescriptorV1 descriptor;
  SblrDdlDropSynonymResultV1 result;
  // Codec-only bodies exercise framing and integrity, not catalog authority.
  // The live server independently requires an admitted executor and receipt.
  descriptor.body[0] = 1;
  descriptor.availability = 0x1020304050607080ULL;
  for (const bool operand : {false, true}) {
    const auto framed = EncodeSblrDdlDropSynonymDescriptorV1(descriptor, operand);
    SblrDdlDropSynonymDescriptorV1 roundtrip;
    Check(framed.size() == 488 && DecodeSblrDdlDropSynonymDescriptorV1(
              framed.data(), framed.size(), &roundtrip, &detail, operand) &&
              roundtrip.body == descriptor.body && roundtrip.availability == descriptor.availability,
          "synonym descriptor framing roundtrip failed");
    Check(!DecodeSblrDdlDropSynonymDescriptorV1(framed.data(), framed.size(), &roundtrip, &detail, !operand) &&
              !DecodeSblrDdlDropSynonymDescriptorV1(framed.data(), std::numeric_limits<std::size_t>::max(),
                                                  &roundtrip, &detail, operand),
          "wrong descriptor framing or unbounded copy accepted");
    auto tampered = framed;
    tampered[16] ^= 2;
    Check(!DecodeSblrDdlDropSynonymDescriptorV1(tampered.data(), tampered.size(), &roundtrip, &detail, operand) &&
              roundtrip.body == descriptor.body, "descriptor corruption accepted or modified output");
  }
  result.body[24] = 1;
  result.body[56] = 1;
  result.availability = descriptor.availability;
  result.publication_barrier = scratchbird::tests::FixtureUuid(1154, 65).bytes;
  const auto framed_result = EncodeSblrDdlDropSynonymResultV1(result);
  SblrDdlDropSynonymResultV1 result_roundtrip;
  Check(framed_result.size() == 320 && DecodeSblrDdlDropSynonymResultV1(
            framed_result.data(), framed_result.size(), &result_roundtrip, &detail) &&
            result_roundtrip.body == result.body && result_roundtrip.availability == result.availability &&
            result_roundtrip.publication_barrier == result.publication_barrier,
        "synonym result framing roundtrip failed");
  Check(!DecodeSblrDdlDropSynonymResultV1(framed_result.data(), std::numeric_limits<std::size_t>::max(),
                                        &result_roundtrip, &detail), "oversized result copied before validation");
  for (const auto length : {std::size_t{0}, std::size_t{3}, std::size_t{464},
                            std::numeric_limits<std::size_t>::max()}) {
    for (const bool operand : {false, true})
      Check(!DecodeSblrDdlDropSynonymDescriptorV1(nullptr, length, &descriptor, &detail, operand),
            "null synonym descriptor reached pointer arithmetic");
    Check(!DecodeSblrDdlDropSynonymResultV1(nullptr, length, &result, &detail),
          "null synonym result reached pointer arithmetic");
  }
  // Foreign framed bodies must be rejected before translation, regardless of
  // whether a shared underlying layout could otherwise decode the body.
  for (const bool operand : {false, true}) {
    auto foreign = bytes;
    std::copy_n(operand ? "PDDO" : "PDDX", 4, foreign.begin());
    Check(!DecodeSblrDdlDropSynonymDescriptorV1(foreign.data(), foreign.size(), &descriptor, &detail, operand) &&
              detail == std::string(operand ? "DSYO" : "DSYD") + " identity invalid",
          "foreign descriptor did not fail at synonym identity boundary");
  }
  Check(!DecodeSblrDdlDropSynonymResultV1(bytes.data(), bytes.size(), &result, &detail) &&
            detail == "DYRS identity invalid", "foreign result bypassed synonym identity boundary");
  std::cout << "ia08_drop_synonym_codec=passed\n";
}
