#include "sblr_ddl_drop_type_runtime.hpp"

#include <string_view>

namespace scratchbird::engine::sblr {
namespace {
std::vector<std::uint8_t> RewriteMagic(std::vector<std::uint8_t> bytes,
                                        const char* magic) {
  if (bytes.size() >= 4) {
    for (std::size_t i = 0; i < 4; ++i) bytes[i] = magic[i];
  }
  return bytes;
}
}

std::vector<std::uint8_t> EncodeSblrDdlDropTypeDescriptorV1(
    const SblrDdlDropTypeDescriptorV1& value, bool operation) {
  return RewriteMagic(EncodeSblrDdlDropViewDescriptorV1(value, operation),
                      operation ? "DTDO" : "DTDX");
}

bool DecodeSblrDdlDropTypeDescriptorV1(const std::uint8_t* data,
                                       std::size_t size,
                                       SblrDdlDropTypeDescriptorV1* value,
                                       std::string* detail, bool operation) {
  if (data == nullptr || size < 4 ||
      (operation ? std::string_view(reinterpret_cast<const char*>(data), 4)
                 : std::string_view(reinterpret_cast<const char*>(data), 4)) !=
          (operation ? "DTDO" : "DTDX")) {
    if (detail) *detail = "DTDX invalid magic";
    return false;
  }
  std::vector<std::uint8_t> rewritten(data, data + size);
  for (std::size_t i = 0; i < 4; ++i) rewritten[i] = operation ? "DVDO"[i] : "DVDX"[i];
  return DecodeSblrDdlDropViewDescriptorV1(rewritten.data(), rewritten.size(),
                                           value, detail, operation);
}

std::vector<std::uint8_t> EncodeSblrDdlDropTypeResultV1(
    const SblrDdlDropTypeResultV1& value) {
  return RewriteMagic(EncodeSblrDdlDropViewResultV1(value), "DTRS");
}

bool DecodeSblrDdlDropTypeResultV1(const std::uint8_t* data, std::size_t size,
                                   SblrDdlDropTypeResultV1* value,
                                   std::string* detail) {
  if (data == nullptr || size < 4 ||
      std::string_view(reinterpret_cast<const char*>(data), 4) != "DTRS") {
    if (detail) *detail = "DTRS invalid magic";
    return false;
  }
  std::vector<std::uint8_t> rewritten(data, data + size);
  for (std::size_t i = 0; i < 4; ++i) rewritten[i] = "DVRS"[i];
  return DecodeSblrDdlDropViewResultV1(rewritten.data(), rewritten.size(),
                                       value, detail);
}
}
