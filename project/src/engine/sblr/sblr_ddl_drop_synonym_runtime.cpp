#include "sblr_ddl_drop_synonym_runtime.hpp"
#include <algorithm>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {
std::vector<std::uint8_t> Rewrite(std::vector<std::uint8_t> bytes,
                                  std::string_view magic) {
  if (bytes.size() >= magic.size())
    std::copy(magic.begin(), magic.end(), bytes.begin());
  return bytes;
}
bool Translate(const std::uint8_t* bytes, std::size_t size,
               std::string_view expected, std::string_view underlying,
               std::size_t frame_size,
               std::vector<std::uint8_t>* translated, std::string* detail) {
  if (bytes == nullptr || size < expected.size() ||
      !std::equal(expected.begin(), expected.end(), bytes)) {
    if (detail) *detail = std::string(expected) + " identity invalid";
    return false;
  }
  if (size != frame_size) {
    if (detail) *detail = std::string(expected) + " length invalid";
    return false;
  }
  *translated = Rewrite(std::vector<std::uint8_t>(bytes, bytes + size), underlying);
  return true;
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrDdlDropSynonymRequestV1(
    const SblrDdlDropSynonymRequestV1& value) {
  return Rewrite(EncodeSblrDdlDropPackageRequestV1(value), "DSYQ");
}
bool DecodeSblrDdlDropSynonymRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropSynonymRequestV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "DSYQ", "PDQX", 64, &translated, detail))
    return false;
  return DecodeSblrDdlDropPackageRequestV1(
      translated.data(), translated.size(), output, detail);
}

std::vector<std::uint8_t> EncodeSblrDdlDropSynonymDescriptorV1(
    const SblrDdlDropSynonymDescriptorV1& value, bool operand) {
  return Rewrite(EncodeSblrDdlDropPackageDescriptorV1(value, operand), operand ? "DSYO" : "DSYD");
}
bool DecodeSblrDdlDropSynonymDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropSynonymDescriptorV1* output, std::string* detail, bool operand) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, operand ? "DSYO" : "DSYD", operand ? "PDDO" : "PDDX", 488, &translated, detail))
    return false;
  return DecodeSblrDdlDropPackageDescriptorV1(
      translated.data(), translated.size(), output, detail, operand);
}

std::vector<std::uint8_t> EncodeSblrDdlDropSynonymResultV1(
    const SblrDdlDropSynonymResultV1& value) {
  return Rewrite(EncodeSblrDdlDropPackageResultV1(value), "DYRS");
}
bool DecodeSblrDdlDropSynonymResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropSynonymResultV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "DYRS", "PDRS", 320, &translated, detail))
    return false;
  return DecodeSblrDdlDropPackageResultV1(
      translated.data(), translated.size(), output, detail);
}
}  // namespace scratchbird::engine::sblr
