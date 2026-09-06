#include "sblr_ddl_create_schema_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {

constexpr std::size_t kRequestSize = 896;
constexpr std::size_t kDescriptorSize = 488;
constexpr std::size_t kResultSize = 320;

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
         GetLe(bytes + 8, 4) == expected_size && AllZero(bytes + 12, bytes + 16);
}

DdlCreateSchemaSha Evidence(std::string_view domain, const std::uint8_t* bytes,
                            std::size_t size) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

bool ValidUtf8(std::string_view value) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = bytes[index];
    if (lead == 0) return false;
    if (lead < 0x80) {
      ++index;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xe0) == 0xc0) {
      continuation = 1;
      codepoint = lead & 0x1f;
      if (codepoint < 2) return false;
    } else if ((lead & 0xf0) == 0xe0) {
      continuation = 2;
      codepoint = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0) {
      continuation = 3;
      codepoint = lead & 0x07;
    } else {
      return false;
    }
    if (index + continuation >= value.size()) return false;
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto byte = bytes[index + offset];
      if ((byte & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if ((continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      return false;
    }
    index += continuation + 1;
  }
  return !value.empty();
}

void PutUuid(std::vector<std::uint8_t>* out, const DdlCreateSchemaUuid& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutSha(std::vector<std::uint8_t>* out, const DdlCreateSchemaSha& sha) {
  out->insert(out->end(), sha.begin(), sha.end());
}

void GetUuid(const std::uint8_t* bytes, DdlCreateSchemaUuid* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

void GetSha(const std::uint8_t* bytes, DdlCreateSchemaSha* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

bool DistinctGeneratedIdentities(const SblrDdlCreateSchemaDescriptorV1& value) {
  return value.schema_uuid != value.binding_uuid &&
         value.schema_uuid != value.recovery_uuid &&
         value.binding_uuid != value.recovery_uuid;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaRequestV1(
    const SblrDdlCreateSchemaRequestV1& value) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.schema_occurrence == 0 || value.command_identity != 1 ||
      value.name_atoms.empty() || value.name_atoms.size() > 3) {
    return {};
  }
  std::uint8_t quoted_mask = 0;
  for (std::size_t index = 0; index < value.name_atoms.size(); ++index) {
    const auto& atom = value.name_atoms[index];
    if (atom.raw_utf8.empty() || atom.raw_utf8.size() > 256 ||
        !ValidUtf8(atom.raw_utf8)) {
      return {};
    }
    if (atom.quoted) quoted_mask |= static_cast<std::uint8_t>(1u << index);
  }

  auto out = Header("CSQX", kRequestSize);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.schema_occurrence, 4);
  PutLe(&out, value.command_identity, 2);
  out.push_back(static_cast<std::uint8_t>(value.name_atoms.size()));
  out.push_back(quoted_mask);
  for (std::size_t index = 0; index < 3; ++index) {
    PutLe(&out, index < value.name_atoms.size()
                    ? value.name_atoms[index].raw_utf8.size()
                    : 0,
          2);
  }
  out.insert(out.end(), 10, 0);
  for (std::size_t index = 0; index < 3; ++index) {
    if (index < value.name_atoms.size()) {
      const auto& text = value.name_atoms[index].raw_utf8;
      out.insert(out.end(), text.begin(), text.end());
      out.insert(out.end(), 256 - text.size(), 0);
    } else {
      out.insert(out.end(), 256, 0);
    }
  }
  const auto evidence = Evidence(
      "ScratchBird.SblrDdlCreateSchemaBindDemand.V1", out.data() + 16, 816);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  out.insert(out.end(), 32, 0);
  return out.size() == kRequestSize ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlCreateSchemaRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlCreateSchemaRequestV1* out, std::string* detail) {
  if (out == nullptr || !ValidHeader(bytes, size, "CSQX", kRequestSize) ||
      !AllZero(bytes + 54, bytes + 64) ||
      !AllZero(bytes + 864, bytes + 896)) {
    if (detail != nullptr) *detail = "CSQX header or reserved bytes are invalid";
    return false;
  }
  SblrDdlCreateSchemaRequestV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.schema_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.command_identity = static_cast<std::uint16_t>(GetLe(bytes + 44, 2));
  const auto count = bytes[46];
  const auto quoted_mask = bytes[47];
  if (count == 0 || count > 3 || (quoted_mask & ~((1u << count) - 1u)) != 0) {
    if (detail != nullptr) *detail = "CSQX name atom count or quote mask is invalid";
    return false;
  }
  for (std::size_t index = 0; index < 3; ++index) {
    const auto length = static_cast<std::size_t>(GetLe(bytes + 48 + index * 2, 2));
    const auto* slot = bytes + 64 + index * 256;
    if ((index >= count && length != 0) || length > 256 ||
        !AllZero(slot + length, slot + 256)) {
      if (detail != nullptr) *detail = "CSQX name atom extent is invalid";
      return false;
    }
    if (index < count) {
      if (length == 0) {
        if (detail != nullptr) *detail = "CSQX name atom is empty";
        return false;
      }
      SblrDdlCreateSchemaNameAtomV1 atom;
      atom.raw_utf8.assign(reinterpret_cast<const char*>(slot), length);
      atom.quoted = (quoted_mask & (1u << index)) != 0;
      if (!ValidUtf8(atom.raw_utf8)) {
        if (detail != nullptr) *detail = "CSQX name atom UTF-8 is invalid";
        return false;
      }
      value.name_atoms.push_back(std::move(atom));
    }
  }
  GetSha(bytes + 832, &value.evidence);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlCreateSchemaBindDemand.V1", bytes + 16, 816);
  if (value.evidence != expected ||
      EncodeSblrDdlCreateSchemaRequestV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "CSQX evidence or canonical encoding is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaDescriptorV1(
    const SblrDdlCreateSchemaDescriptorV1& value, bool operand_magic) {
  const bool parent_present = (value.flags & 1u) != 0;
  if ((value.flags & ~1u) != 0 || !NonZero(value.receipt) ||
      value.occurrence == 0 || value.schema_occurrence == 0 ||
      !NonZero(value.schema_uuid) || value.schema_generation == 0 ||
      parent_present != NonZero(value.parent_schema_uuid) ||
      parent_present != (value.parent_namespace_generation != 0) ||
      !NonZero(value.database_uuid) || !NonZero(value.owning_transaction_uuid) ||
      value.owning_local_transaction_id == 0 ||
      !NonZero(value.statement_snapshot_uuid) ||
      !NonZero(value.catalog_epoch_uuid) || value.catalog_generation == 0 ||
      !NonZero(value.security_context_uuid) || value.security_epoch == 0 ||
      !NonZero(value.policy_snapshot_uuid) || value.policy_generation == 0 ||
      !NonZero(value.resource_grant_uuid) || value.resource_generation == 0 ||
      !NonZero(value.owner_principal_uuid) || !NonZero(value.binding_uuid) ||
      !NonZero(value.recovery_uuid) || value.binding_generation == 0 ||
      value.recovery_generation == 0 ||
      !NonZero(value.normalized_path_sha256) ||
      !NonZero(value.syntax_demand_sha256) ||
      !NonZero(value.authorization_evidence_sha256) ||
      value.availability == 0 || !DistinctGeneratedIdentities(value)) {
    return {};
  }
  auto out = Header(operand_magic ? "CSDO" : "CSDX", kDescriptorSize);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.schema_occurrence, 4);
  PutLe(&out, value.flags, 4);
  PutUuid(&out, value.schema_uuid);
  PutLe(&out, value.schema_generation, 8);
  PutUuid(&out, value.parent_schema_uuid);
  PutLe(&out, value.parent_namespace_generation, 8);
  PutUuid(&out, value.database_uuid);
  PutUuid(&out, value.owning_transaction_uuid);
  PutLe(&out, value.owning_local_transaction_id, 8);
  PutUuid(&out, value.statement_snapshot_uuid);
  PutUuid(&out, value.catalog_epoch_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutUuid(&out, value.security_context_uuid);
  PutLe(&out, value.security_epoch, 8);
  PutUuid(&out, value.policy_snapshot_uuid);
  PutLe(&out, value.policy_generation, 8);
  PutUuid(&out, value.resource_grant_uuid);
  PutLe(&out, value.resource_generation, 8);
  PutUuid(&out, value.owner_principal_uuid);
  PutUuid(&out, value.binding_uuid);
  PutUuid(&out, value.recovery_uuid);
  PutLe(&out, value.binding_generation, 8);
  PutLe(&out, value.recovery_generation, 8);
  PutSha(&out, value.normalized_path_sha256);
  PutSha(&out, value.syntax_demand_sha256);
  PutSha(&out, value.authorization_evidence_sha256);
  out.insert(out.end(), 8, 0);
  const auto evidence = Evidence(
      "ScratchBird.SblrDdlCreateSchemaDescriptor.V1", out.data() + 16, 400);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), 32, 0);
  return out.size() == kDescriptorSize ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlCreateSchemaDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlCreateSchemaDescriptorV1* out, std::string* detail,
    bool operand_magic) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, operand_magic ? "CSDO" : "CSDX",
                   kDescriptorSize) ||
      !AllZero(bytes + 408, bytes + 416) ||
      !AllZero(bytes + 456, bytes + 488)) {
    if (detail != nullptr) *detail = "CSDX/CSDO header or reserved bytes are invalid";
    return false;
  }
  SblrDdlCreateSchemaDescriptorV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.schema_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.flags = static_cast<std::uint32_t>(GetLe(bytes + 44, 4));
  GetUuid(bytes + 48, &value.schema_uuid);
  value.schema_generation = GetLe(bytes + 64, 8);
  GetUuid(bytes + 72, &value.parent_schema_uuid);
  value.parent_namespace_generation = GetLe(bytes + 88, 8);
  GetUuid(bytes + 96, &value.database_uuid);
  GetUuid(bytes + 112, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 128, 8);
  GetUuid(bytes + 136, &value.statement_snapshot_uuid);
  GetUuid(bytes + 152, &value.catalog_epoch_uuid);
  value.catalog_generation = GetLe(bytes + 168, 8);
  GetUuid(bytes + 176, &value.security_context_uuid);
  value.security_epoch = GetLe(bytes + 192, 8);
  GetUuid(bytes + 200, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(bytes + 216, 8);
  GetUuid(bytes + 224, &value.resource_grant_uuid);
  value.resource_generation = GetLe(bytes + 240, 8);
  GetUuid(bytes + 248, &value.owner_principal_uuid);
  GetUuid(bytes + 264, &value.binding_uuid);
  GetUuid(bytes + 280, &value.recovery_uuid);
  value.binding_generation = GetLe(bytes + 296, 8);
  value.recovery_generation = GetLe(bytes + 304, 8);
  GetSha(bytes + 312, &value.normalized_path_sha256);
  GetSha(bytes + 344, &value.syntax_demand_sha256);
  GetSha(bytes + 376, &value.authorization_evidence_sha256);
  GetSha(bytes + 416, &value.evidence);
  value.availability = GetLe(bytes + 448, 8);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlCreateSchemaDescriptor.V1", bytes + 16, 400);
  if (value.evidence != expected ||
      EncodeSblrDdlCreateSchemaDescriptorV1(value, operand_magic) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "CSDX/CSDO authority or evidence is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlCreateSchemaResultV1(
    const SblrDdlCreateSchemaResultV1& value) {
  const bool parent_present = NonZero(value.parent_schema_uuid);
  if (!NonZero(value.receipt) || !NonZero(value.schema_uuid) ||
      value.schema_generation == 0 ||
      parent_present != (value.parent_namespace_generation != 0) ||
      !NonZero(value.database_uuid) || !NonZero(value.owning_transaction_uuid) ||
      value.owning_local_transaction_id == 0 ||
      !NonZero(value.statement_snapshot_uuid) ||
      !NonZero(value.catalog_row_uuid) || !NonZero(value.mutation_uuid) ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_generation == 0 ||
      !NonZero(value.normalized_path_sha256) ||
      !NonZero(value.descriptor_evidence_sha256) || value.availability == 0 ||
      !NonZero(value.publication_barrier) ||
      value.schema_uuid == value.catalog_row_uuid ||
      value.schema_uuid == value.mutation_uuid ||
      value.schema_uuid == value.publication_barrier ||
      value.catalog_row_uuid == value.mutation_uuid ||
      value.catalog_row_uuid == value.publication_barrier ||
      value.mutation_uuid == value.publication_barrier) {
    return {};
  }
  auto out = Header("CSRS", kResultSize);
  PutUuid(&out, value.receipt);
  PutUuid(&out, value.schema_uuid);
  PutLe(&out, value.schema_generation, 8);
  PutUuid(&out, value.parent_schema_uuid);
  PutLe(&out, value.parent_namespace_generation, 8);
  PutUuid(&out, value.database_uuid);
  PutUuid(&out, value.owning_transaction_uuid);
  PutLe(&out, value.owning_local_transaction_id, 8);
  PutUuid(&out, value.statement_snapshot_uuid);
  PutUuid(&out, value.catalog_row_uuid);
  PutUuid(&out, value.mutation_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_generation, 8);
  PutSha(&out, value.normalized_path_sha256);
  PutSha(&out, value.descriptor_evidence_sha256);
  const auto evidence = Evidence(
      "ScratchBird.SblrDdlCreateSchemaResult.V1", out.data() + 16, 240);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  PutUuid(&out, value.publication_barrier);
  out.insert(out.end(), 8, 0);
  return out.size() == kResultSize ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlCreateSchemaResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlCreateSchemaResultV1* out, std::string* detail) {
  if (out == nullptr || !ValidHeader(bytes, size, "CSRS", kResultSize) ||
      !AllZero(bytes + 312, bytes + 320)) {
    if (detail != nullptr) *detail = "CSRS header or reserved bytes are invalid";
    return false;
  }
  SblrDdlCreateSchemaResultV1 value;
  GetUuid(bytes + 16, &value.receipt);
  GetUuid(bytes + 32, &value.schema_uuid);
  value.schema_generation = GetLe(bytes + 48, 8);
  GetUuid(bytes + 56, &value.parent_schema_uuid);
  value.parent_namespace_generation = GetLe(bytes + 72, 8);
  GetUuid(bytes + 80, &value.database_uuid);
  GetUuid(bytes + 96, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 112, 8);
  GetUuid(bytes + 120, &value.statement_snapshot_uuid);
  GetUuid(bytes + 136, &value.catalog_row_uuid);
  GetUuid(bytes + 152, &value.mutation_uuid);
  value.catalog_generation = GetLe(bytes + 168, 8);
  value.security_epoch = GetLe(bytes + 176, 8);
  value.resource_generation = GetLe(bytes + 184, 8);
  GetSha(bytes + 192, &value.normalized_path_sha256);
  GetSha(bytes + 224, &value.descriptor_evidence_sha256);
  GetSha(bytes + 256, &value.evidence);
  value.availability = GetLe(bytes + 288, 8);
  GetUuid(bytes + 296, &value.publication_barrier);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlCreateSchemaResult.V1", bytes + 16, 240);
  if (value.evidence != expected ||
      EncodeSblrDdlCreateSchemaResultV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "CSRS authority or evidence is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace scratchbird::engine::sblr
