#include "sblr_ddl_drop_foreign_table_runtime.hpp"
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

std::vector<std::uint8_t> EncodeSblrDdlDropForeignTableRequestV1(
    const SblrDdlDropForeignTableRequestV1& value) {
  return Rewrite(EncodeSblrDdlDropPackageRequestV1(value), "DFTQ");
}
bool DecodeSblrDdlDropForeignTableRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropForeignTableRequestV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "DFTQ", "PDQX", &translated, detail))
    return false;
  return DecodeSblrDdlDropPackageRequestV1(
      translated.data(), translated.size(), output, detail);
}

std::vector<std::uint8_t> EncodeSblrDdlDropForeignTableDescriptorV1(
    const SblrDdlDropForeignTableDescriptorV1& value, bool operand) {
  return Rewrite(EncodeSblrDdlDropPackageDescriptorV1(value, operand), operand ? "DFTO" : "DFTD");
}
bool DecodeSblrDdlDropForeignTableDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropForeignTableDescriptorV1* output, std::string* detail, bool operand) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, operand ? "DFTO" : "DFTD", operand ? "PDDO" : "PDDX", &translated, detail))
    return false;
  return DecodeSblrDdlDropPackageDescriptorV1(
      translated.data(), translated.size(), output, detail, operand);
}

std::vector<std::uint8_t> EncodeSblrDdlDropForeignTableResultV1(
    const SblrDdlDropForeignTableResultV1& value) {
  return Rewrite(EncodeSblrDdlDropPackageResultV1(value), "DFTR");
}
bool DecodeSblrDdlDropForeignTableResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropForeignTableResultV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "DFTR", "PDRS", &translated, detail))
    return false;
  return DecodeSblrDdlDropPackageResultV1(
      translated.data(), translated.size(), output, detail);
}
}  // namespace scratchbird::engine::sblr
