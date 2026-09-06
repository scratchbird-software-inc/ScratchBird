#include "sblr_catalog_introspect_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr {
namespace {

void put(std::vector<std::uint8_t>& output, std::uint64_t value,
         std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    output.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
  }
}

std::uint64_t get(const std::uint8_t* bytes, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < width; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
  }
  return value;
}

template <class T>
bool nz(const T& value) {
  return std::any_of(value.begin(), value.end(),
                     [](auto byte) { return byte != 0; });
}

std::vector<std::uint8_t> header(const char* magic, std::size_t size) {
  std::vector<std::uint8_t> output(magic, magic + 4);
  put(output, 1, 2);
  put(output, size, 2);
  put(output, size, 4);
  put(output, 0, 4);
  return output;
}

bool valid_header(const std::uint8_t* bytes, std::size_t size,
                  const char* magic, std::size_t expected_size) {
  return bytes != nullptr && size == expected_size &&
         std::equal(bytes, bytes + 4, magic) && get(bytes + 4, 2) == 1 &&
         get(bytes + 6, 2) == expected_size &&
         get(bytes + 8, 4) == expected_size &&
         std::all_of(bytes + 12, bytes + 16,
                     [](auto byte) { return byte == 0; });
}

CatalogSha hash(const char* domain, const std::uint8_t* bytes,
                std::size_t size) {
  std::vector<std::uint8_t> material(domain, domain + std::strlen(domain));
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

bool valid_utf8(std::string_view text) {
  if (text.empty()) return false;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = bytes[i];
    if (lead == 0) return false;
    if (lead < 0x80) { ++i; continue; }
    std::size_t count = 0;
    std::uint32_t value = 0;
    if ((lead & 0xe0) == 0xc0) { count = 2; value = lead & 0x1f; }
    else if ((lead & 0xf0) == 0xe0) { count = 3; value = lead & 0x0f; }
    else if ((lead & 0xf8) == 0xf0) { count = 4; value = lead & 0x07; }
    else return false;
    if (i + count > text.size()) return false;
    for (std::size_t j = 1; j < count; ++j) {
      if ((bytes[i + j] & 0xc0) != 0x80) return false;
      value = (value << 6) | (bytes[i + j] & 0x3f);
    }
    if ((count == 2 && value < 0x80) ||
        (count == 3 && value < 0x800) ||
        (count == 4 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return false;
    i += count;
  }
  return true;
}

bool valid_descriptor(const SblrCatalogIntrospectDescriptorV1& v) {
  return v.object_kind == kSblrCatalogIntrospectObjectKindTableV1 &&
         v.profile == kSblrCatalogIntrospectProfileShowObjectDetailV1 &&
         v.flags == kSblrCatalogIntrospectDetailFlagV1 && nz(v.object_uuid) &&
         v.catalog_epoch != 0 && v.security_epoch != 0 &&
         v.canonical_path_utf8.size() <= 352 &&
         valid_utf8(v.canonical_path_utf8) && v.availability != 0;
}

bool valid_result(const SblrCatalogIntrospectResultV1& v) {
  const std::array<CatalogUuid, 7> identities{
      v.request_uuid, v.readable_projection_uuid, v.row_descriptor_uuid,
      v.result_set_uuid, v.object_uuid, v.statement_snapshot_uuid,
      v.publication_barrier};
  if (std::any_of(identities.begin(), identities.end(),
                  [](const auto& id) { return !nz(id); })) return false;
  for (std::size_t left = 0; left < identities.size(); ++left) {
    for (std::size_t right = left + 1; right < identities.size(); ++right) {
      if (identities[left] == identities[right]) return false;
    }
  }
  return v.catalog_epoch != 0 && v.security_epoch != 0 && v.row_count != 0 &&
         v.object_kind == kSblrCatalogIntrospectObjectKindTableV1 &&
         v.profile == kSblrCatalogIntrospectProfileShowObjectDetailV1 &&
         v.flags == kSblrCatalogIntrospectDetailFlagV1 &&
         nz(v.row_material_sha256) && nz(v.descriptor_evidence_sha256) &&
         v.availability != 0;
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrCatalogIntrospectRequestV1(
    const SblrCatalogIntrospectRequestV1& request) {
  if (!nz(request.receipt) || request.occurrence == 0 ||
      request.object_occurrence == 0) {
    return {};
  }
  auto output = header("CIRQ", 64);
  output.insert(output.end(), request.receipt.begin(), request.receipt.end());
  put(output, request.occurrence, 8);
  put(output, request.object_occurrence, 4);
  output.insert(output.end(), 20, 0);
  return output;
}

bool DecodeSblrCatalogIntrospectRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrCatalogIntrospectRequestV1* output, std::string* detail) {
  if (output == nullptr || !valid_header(bytes, size, "CIRQ", 64) ||
      std::any_of(bytes + 44, bytes + 64,
                  [](auto byte) { return byte != 0; })) {
    if (detail != nullptr) *detail = "CIRQ invalid";
    return false;
  }
  SblrCatalogIntrospectRequestV1 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = get(bytes + 32, 8);
  value.object_occurrence = static_cast<std::uint32_t>(get(bytes + 40, 4));
  if (EncodeSblrCatalogIntrospectRequestV1(value).empty()) return false;
  *output = value;
  return true;
}
std::vector<uint8_t> EncodeSblrCatalogIntrospectDescriptorV1(
    const SblrCatalogIntrospectDescriptorV1& v, bool operand) {
  if (!valid_descriptor(v)) return {};
  std::vector<std::uint8_t> out(488, 0);
  std::copy_n(operand ? "CIDO" : "CIDD", 4, out.begin());
  out[4] = 1;
  out[6] = static_cast<std::uint8_t>(v.object_kind);
  out[7] = static_cast<std::uint8_t>(v.object_kind >> 8);
  out[8] = static_cast<std::uint8_t>(v.profile);
  out[9] = static_cast<std::uint8_t>(v.profile >> 8);
  out[10] = static_cast<std::uint8_t>(v.flags);
  out[11] = static_cast<std::uint8_t>(v.flags >> 8);
  std::copy(v.object_uuid.begin(), v.object_uuid.end(), out.begin() + 16);
  for (std::size_t index = 0; index < 8; ++index) {
    out[32 + index] = static_cast<std::uint8_t>(v.catalog_epoch >> (8 * index));
    out[40 + index] = static_cast<std::uint8_t>(v.security_epoch >> (8 * index));
  }
  const auto path_size = static_cast<std::uint32_t>(v.canonical_path_utf8.size());
  for (std::size_t index = 0; index < 4; ++index) {
    out[48 + index] = static_cast<std::uint8_t>(path_size >> (8 * index));
  }
  std::copy(v.canonical_path_utf8.begin(), v.canonical_path_utf8.end(),
            out.begin() + 56);
  // CIDD and CIDO share one evidence identity.  Normalize the four-byte
  // transport magic to CIDO before hashing so the parser's only permitted
  // transformation does not mint a second descriptor authority.
  auto evidence_material = out;
  std::copy_n("CIDO", 4, evidence_material.begin());
  const auto evidence = hash(
      "ScratchBird.SblrCatalogIntrospectShowObjectDetailDescriptor.V1",
      evidence_material.data(), 408);
  if (nz(v.evidence) && evidence != v.evidence) return {};
  std::copy(evidence.begin(), evidence.end(), out.begin() + 408);
  for (std::size_t index = 0; index < 8; ++index) {
    out[440 + index] = static_cast<std::uint8_t>(v.availability >> (8 * index));
  }
  return out;
}

bool DecodeSblrCatalogIntrospectDescriptorV1(
    const uint8_t* bytes, size_t size,
    SblrCatalogIntrospectDescriptorV1* output, std::string* detail,
    bool operand) {
  if (!output || bytes == nullptr || size != 488 ||
      !std::equal(bytes, bytes + 4, operand ? "CIDO" : "CIDD") ||
      get(bytes + 4, 2) != 1 ||
      std::any_of(bytes + 12, bytes + 16, [](auto v) { return v != 0; }) ||
      std::any_of(bytes + 52, bytes + 56, [](auto v) { return v != 0; }) ||
      std::any_of(bytes + 448, bytes + 488,
                  [](auto v) { return v != 0; })) {
    if (detail) *detail = "CID header or reserved bytes are invalid";
    return false;
  }
  const auto path_size = static_cast<std::size_t>(get(bytes + 48, 4));
  if (path_size == 0 || path_size > 352 ||
      std::any_of(bytes + 56 + path_size, bytes + 408,
                  [](auto v) { return v != 0; })) {
    if (detail) *detail = "CID canonical path extent is invalid";
    return false;
  }
  SblrCatalogIntrospectDescriptorV1 value;
  value.object_kind = static_cast<std::uint16_t>(get(bytes + 6, 2));
  value.profile = static_cast<std::uint16_t>(get(bytes + 8, 2));
  value.flags = static_cast<std::uint16_t>(get(bytes + 10, 2));
  std::copy_n(bytes + 16, 16, value.object_uuid.begin());
  value.catalog_epoch = get(bytes + 32, 8);
  value.security_epoch = get(bytes + 40, 8);
  value.canonical_path_utf8.assign(
      reinterpret_cast<const char*>(bytes + 56), path_size);
  std::copy_n(bytes + 408, 32, value.evidence.begin());
  value.availability = get(bytes + 440, 8);
  const auto expected = EncodeSblrCatalogIntrospectDescriptorV1(value, operand);
  if (!valid_descriptor(value) || expected.size() != size ||
      !std::equal(expected.begin(), expected.end(), bytes)) {
    if (detail) *detail = "CID authority fields or evidence are invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

std::vector<uint8_t> EncodeSblrCatalogIntrospectResultV1(
    const SblrCatalogIntrospectResultV1& v) {
  if (!valid_result(v)) return {};
  auto out = header("CIRS", 320);
  out.insert(out.end(), v.request_uuid.begin(), v.request_uuid.end());
  out.insert(out.end(), v.readable_projection_uuid.begin(),
             v.readable_projection_uuid.end());
  out.insert(out.end(), v.row_descriptor_uuid.begin(),
             v.row_descriptor_uuid.end());
  out.insert(out.end(), v.result_set_uuid.begin(), v.result_set_uuid.end());
  out.insert(out.end(), v.object_uuid.begin(), v.object_uuid.end());
  out.insert(out.end(), v.statement_snapshot_uuid.begin(),
             v.statement_snapshot_uuid.end());
  put(out, v.catalog_epoch, 8);
  put(out, v.security_epoch, 8);
  put(out, v.row_count, 8);
  put(out, v.object_kind, 2);
  put(out, v.profile, 2);
  put(out, v.flags, 2);
  out.insert(out.end(), 2, 0);
  out.insert(out.end(), v.row_material_sha256.begin(),
             v.row_material_sha256.end());
  out.insert(out.end(), v.descriptor_evidence_sha256.begin(),
             v.descriptor_evidence_sha256.end());
  out.insert(out.end(), 48, 0);
  const auto evidence = hash(
      "ScratchBird.SblrCatalogIntrospectShowObjectDetailResult.V1",
      out.data() + 16, 240);
  if (nz(v.evidence) && evidence != v.evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  put(out, v.availability, 8);
  out.insert(out.end(), v.publication_barrier.begin(),
             v.publication_barrier.end());
  out.insert(out.end(), 8, 0);
  return out;
}

bool DecodeSblrCatalogIntrospectResultV1(
    const uint8_t* bytes, size_t size, SblrCatalogIntrospectResultV1* output,
    std::string* detail) {
  if (!output || !valid_header(bytes, size, "CIRS", 320) ||
      std::any_of(bytes + 142, bytes + 144,
                  [](auto v) { return v != 0; }) ||
      std::any_of(bytes + 208, bytes + 256,
                  [](auto v) { return v != 0; }) ||
      std::any_of(bytes + 312, bytes + 320,
                  [](auto v) { return v != 0; })) {
    if (detail) *detail = "CIRS header or reserved bytes are invalid";
    return false;
  }
  SblrCatalogIntrospectResultV1 value;
  std::copy_n(bytes + 16, 16, value.request_uuid.begin());
  std::copy_n(bytes + 32, 16, value.readable_projection_uuid.begin());
  std::copy_n(bytes + 48, 16, value.row_descriptor_uuid.begin());
  std::copy_n(bytes + 64, 16, value.result_set_uuid.begin());
  std::copy_n(bytes + 80, 16, value.object_uuid.begin());
  std::copy_n(bytes + 96, 16, value.statement_snapshot_uuid.begin());
  value.catalog_epoch = get(bytes + 112, 8);
  value.security_epoch = get(bytes + 120, 8);
  value.row_count = get(bytes + 128, 8);
  value.object_kind = static_cast<std::uint16_t>(get(bytes + 136, 2));
  value.profile = static_cast<std::uint16_t>(get(bytes + 138, 2));
  value.flags = static_cast<std::uint16_t>(get(bytes + 140, 2));
  std::copy_n(bytes + 144, 32, value.row_material_sha256.begin());
  std::copy_n(bytes + 176, 32, value.descriptor_evidence_sha256.begin());
  std::copy_n(bytes + 256, 32, value.evidence.begin());
  value.availability = get(bytes + 288, 8);
  std::copy_n(bytes + 296, 16, value.publication_barrier.begin());
  const auto expected = EncodeSblrCatalogIntrospectResultV1(value);
  if (!valid_result(value) || expected.size() != size ||
      !std::equal(expected.begin(), expected.end(), bytes)) {
    if (detail) *detail = "CIRS authority fields or evidence are invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

}  // namespace scratchbird::engine::sblr
