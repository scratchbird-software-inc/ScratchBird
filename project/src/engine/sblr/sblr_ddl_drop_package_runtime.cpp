#include "sblr_ddl_drop_package_runtime.hpp"
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
               std::vector<std::uint8_t>* translated, std::string* detail) {
  if (bytes == nullptr || size < expected.size() ||
      !std::equal(expected.begin(), expected.end(), bytes)) {
    if (detail) *detail = std::string(expected) + " identity invalid";
    return false;
  }
  *translated = Rewrite(std::vector<std::uint8_t>(bytes, bytes + size), underlying);
  return true;
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrDdlDropPackageRequestV1(
    const SblrDdlDropPackageRequestV1& value) {
  return Rewrite(EncodeSblrDdlCreatePackageRequestV1(value), "PDQX");
}
bool DecodeSblrDdlDropPackageRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropPackageRequestV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "PDQX", "PCQX", &translated, detail))
    return false;
  return DecodeSblrDdlCreatePackageRequestV1(
      translated.data(), translated.size(), output, detail);
}

std::vector<std::uint8_t> EncodeSblrDdlDropPackageDescriptorV1(
    const SblrDdlDropPackageDescriptorV1& value, bool operand) {
  return Rewrite(EncodeSblrDdlCreatePackageDescriptorV1(value, operand), operand ? "PDDO" : "PDDX");
}
bool DecodeSblrDdlDropPackageDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropPackageDescriptorV1* output, std::string* detail, bool operand) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, operand ? "PDDO" : "PDDX", operand ? "PCDO" : "PCDX", &translated, detail))
    return false;
  return DecodeSblrDdlCreatePackageDescriptorV1(
      translated.data(), translated.size(), output, detail, operand);
}

std::vector<std::uint8_t> EncodeSblrDdlDropPackageResultV1(
    const SblrDdlDropPackageResultV1& value) {
  return Rewrite(EncodeSblrDdlCreatePackageResultV1(value), "PDRS");
}
bool DecodeSblrDdlDropPackageResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropPackageResultV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "PDRS", "PCRS", &translated, detail))
    return false;
  return DecodeSblrDdlCreatePackageResultV1(
      translated.data(), translated.size(), output, detail);
}
}  // namespace scratchbird::engine::sblr
