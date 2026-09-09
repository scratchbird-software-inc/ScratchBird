#include "sblr_security_create_user_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {

void PutLe(std::vector<std::uint8_t>* out, std::uint64_t value,
           std::size_t bytes) {
  for (std::size_t index = 0; index < bytes; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

std::uint64_t GetLe(const std::uint8_t* bytes, std::size_t count) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

template <typename T>
bool NonZero(const T& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool AllZero(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

std::vector<std::uint8_t> Header(std::string_view magic, std::size_t size) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, size, 2);
  PutLe(&out, size, 4);
  PutLe(&out, 0, 4);
  return out;
}

bool ValidHeader(const std::uint8_t* bytes, std::size_t size,
                 std::string_view magic, std::size_t expected_size) {
  return bytes != nullptr && size == expected_size &&
         std::equal(bytes, bytes + 4, magic.begin(), magic.end()) &&
         GetLe(bytes + 4, 2) == 1 && GetLe(bytes + 6, 2) == expected_size &&
         GetLe(bytes + 8, 4) == expected_size &&
         AllZero(bytes + 12, bytes + 16);
}

SecurityCreateUserSha Evidence(std::string_view domain,
                               const std::uint8_t* bytes,
                               std::size_t size) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrSecurityCreateUserRequestV1(
    const SblrSecurityCreateUserRequestV1& value) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.template_occurrence == 0) {
    return {};
  }
  auto out = Header("SCUQ", 64);
  out.insert(out.end(), value.receipt.begin(), value.receipt.end());
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.template_occurrence, 4);
  out.insert(out.end(), 20, 0);
  return out;
}

bool DecodeSblrSecurityCreateUserRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecurityCreateUserRequestV1* out, std::string* detail) {
  if (out == nullptr || !ValidHeader(bytes, size, "SCUQ", 64) ||
      !AllZero(bytes + 44, bytes + 64)) {
    if (detail != nullptr) *detail = "SCUQ invalid";
    return false;
  }
  SblrSecurityCreateUserRequestV1 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = GetLe(bytes + 32, 8);
  value.template_occurrence =
      static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  if (EncodeSblrSecurityCreateUserRequestV1(value).empty()) {
    if (detail != nullptr) *detail = "SCUQ identity invalid";
    return false;
  }
  *out = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrSecurityCreateUserDescriptorV1(
    const SblrSecurityCreateUserDescriptorV1& value, bool operand_magic) {
  if (!NonZero(value.body) || value.availability == 0) return {};
  auto out = Header(operand_magic ? "SCUO" : "SCUD", 488);
  out.insert(out.end(), value.body.begin(), value.body.end());
  // Preserve the historical carrier bytes while removing the source-level
  // type alias that coupled CREATE USER to privilege-template evolution.
  const auto computed = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateDescriptor.V1",
      out.data() + 16, 400);
  if (NonZero(value.evidence) && value.evidence != computed) return {};
  out.insert(out.end(), computed.begin(), computed.end());
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), 32, 0);
  return out;
}

bool DecodeSblrSecurityCreateUserDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecurityCreateUserDescriptorV1* out, std::string* detail,
    bool operand_magic) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, operand_magic ? "SCUO" : "SCUD", 488) ||
      !AllZero(bytes + 456, bytes + 488)) {
    if (detail != nullptr) *detail = "SCU descriptor invalid";
    return false;
  }
  SblrSecurityCreateUserDescriptorV1 value;
  std::copy_n(bytes + 16, 400, value.body.begin());
  std::copy_n(bytes + 416, 32, value.evidence.begin());
  value.availability = GetLe(bytes + 448, 8);
  if (EncodeSblrSecurityCreateUserDescriptorV1(value, operand_magic) !=
      std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "SCU descriptor evidence invalid";
    return false;
  }
  *out = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrSecurityCreateUserResultV1(
    const SblrSecurityCreateUserResultV1& value) {
  if (!NonZero(value.body) || value.availability == 0 ||
      !NonZero(value.publication_barrier)) {
    return {};
  }
  auto out = Header("SCUR", 320);
  out.insert(out.end(), value.body.begin(), value.body.end());
  const auto computed = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateResult.V1",
      out.data() + 16, 240);
  if (NonZero(value.evidence) && value.evidence != computed) return {};
  out.insert(out.end(), computed.begin(), computed.end());
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), value.publication_barrier.begin(),
             value.publication_barrier.end());
  out.insert(out.end(), 8, 0);
  return out;
}

bool DecodeSblrSecurityCreateUserResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecurityCreateUserResultV1* out, std::string* detail) {
  if (out == nullptr || !ValidHeader(bytes, size, "SCUR", 320) ||
      !AllZero(bytes + 312, bytes + 320)) {
    if (detail != nullptr) *detail = "SCUR invalid";
    return false;
  }
  SblrSecurityCreateUserResultV1 value;
  std::copy_n(bytes + 16, 240, value.body.begin());
  std::copy_n(bytes + 256, 32, value.evidence.begin());
  value.availability = GetLe(bytes + 288, 8);
  std::copy_n(bytes + 296, 16, value.publication_barrier.begin());
  if (EncodeSblrSecurityCreateUserResultV1(value) !=
      std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "SCUR evidence invalid";
    return false;
  }
  *out = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
