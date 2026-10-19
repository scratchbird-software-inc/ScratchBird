#include "sblr_ddl_drop_fdw_runtime.hpp"
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

std::vector<std::uint8_t> EncodeSblrDdlDropFdwRequestV1(
    const SblrDdlDropFdwRequestV1& value) {
  return Rewrite(EncodeSblrDdlDropForeignTableRequestV1(value), "FDXQ");
}
bool DecodeSblrDdlDropFdwRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropFdwRequestV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "FDXQ", "DFTQ", &translated, detail))
    return false;
  return DecodeSblrDdlDropForeignTableRequestV1(
      translated.data(), translated.size(), output, detail);
}

std::vector<std::uint8_t> EncodeSblrDdlDropFdwDescriptorV1(
    const SblrDdlDropFdwDescriptorV1& value, bool operand) {
  return Rewrite(EncodeSblrDdlDropForeignTableDescriptorV1(value, operand), operand ? "FDXO" : "FDXD");
}
bool DecodeSblrDdlDropFdwDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropFdwDescriptorV1* output, std::string* detail, bool operand) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, operand ? "FDXO" : "FDXD", operand ? "DFTO" : "DFTD", &translated, detail))
    return false;
  return DecodeSblrDdlDropForeignTableDescriptorV1(
      translated.data(), translated.size(), output, detail, operand);
}

std::vector<std::uint8_t> EncodeSblrDdlDropFdwResultV1(
    const SblrDdlDropFdwResultV1& value) {
  return Rewrite(EncodeSblrDdlDropForeignTableResultV1(value), "FDXR");
}
bool DecodeSblrDdlDropFdwResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropFdwResultV1* output, std::string* detail) {
  if (output == nullptr) return false;
  std::vector<std::uint8_t> translated;
  if (!Translate(bytes, size, "FDXR", "DFTR", &translated, detail))
    return false;
  return DecodeSblrDdlDropForeignTableResultV1(
      translated.data(), translated.size(), output, detail);
}
}  // namespace scratchbird::engine::sblr
