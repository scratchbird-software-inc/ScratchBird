#include "sblr_security_create_privilege_template_runtime.hpp"

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

PrivilegeTemplateSha Evidence(std::string_view domain,
                              const std::uint8_t* bytes,
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

bool ValidObjectKind(PrivilegeTemplateObjectKindV1 value) {
  const auto encoded = static_cast<std::uint8_t>(value);
  return encoded >= 1 && encoded <= 5;
}

void PutUuid(std::vector<std::uint8_t>* out,
             const PrivilegeTemplateUuid& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void PutSha(std::vector<std::uint8_t>* out,
            const PrivilegeTemplateSha& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void PutSlot(std::vector<std::uint8_t>* out, std::string_view value,
             std::size_t extent) {
  out->insert(out->end(), value.begin(), value.end());
  out->insert(out->end(), extent - value.size(), 0);
}

void GetUuid(const std::uint8_t* bytes, PrivilegeTemplateUuid* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

void GetSha(const std::uint8_t* bytes, PrivilegeTemplateSha* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

bool DecodeSlot(const std::uint8_t* slot, std::size_t extent,
                std::size_t length, std::string* out) {
  if (length == 0 || length > extent ||
      !AllZero(slot + length, slot + extent)) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(slot), length);
  return ValidUtf8(*out);
}

std::uint32_t DescriptorFlags(PrivilegeTemplateObjectKindV1 object_kind,
                              bool with_grant_option, bool enabled) {
  return static_cast<std::uint32_t>(object_kind) |
         (with_grant_option ? (1u << 3) : 0u) |
         (enabled ? (1u << 4) : 0u);
}

bool DecodeDescriptorFlags(std::uint32_t flags,
                           PrivilegeTemplateObjectKindV1* object_kind,
                           bool* with_grant_option, bool* enabled) {
  if ((flags & ~0x1fu) != 0) return false;
  *object_kind =
      static_cast<PrivilegeTemplateObjectKindV1>(flags & 0x7u);
  *with_grant_option = (flags & (1u << 3)) != 0;
  *enabled = (flags & (1u << 4)) != 0;
  return ValidObjectKind(*object_kind) && *enabled;
}

}  // namespace

std::vector<std::uint8_t>
EncodeSblrSecurityCreatePrivilegeTemplateRequestV1(
    const SblrSecurityCreatePrivilegeTemplateRequestV1& value) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.template_occurrence == 0 || value.command_identity != 1 ||
      !ValidObjectKind(value.object_kind) || !value.enabled ||
      value.template_name_utf8.size() > 256 ||
      value.grantee_name_utf8.size() > 256 ||
      value.privilege_utf8.size() > 128 ||
      !ValidUtf8(value.template_name_utf8) ||
      !ValidUtf8(value.grantee_name_utf8) ||
      !ValidUtf8(value.privilege_utf8)) {
    return {};
  }

  auto out = Header("PTQX",
                    kSblrSecurityCreatePrivilegeTemplateRequestV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.template_occurrence, 4);
  PutLe(&out, value.command_identity, 2);
  out.push_back(static_cast<std::uint8_t>(value.object_kind));
  std::uint8_t flags = 0;
  if (value.with_grant_option) flags |= 1u;
  if (value.enabled) flags |= 1u << 1;
  if (value.template_name_quoted) flags |= 1u << 2;
  if (value.grantee_name_quoted) flags |= 1u << 3;
  out.push_back(flags);
  PutLe(&out, value.template_name_utf8.size(), 2);
  PutLe(&out, value.grantee_name_utf8.size(), 2);
  PutLe(&out, value.privilege_utf8.size(), 2);
  out.insert(out.end(), 10, 0);
  PutSlot(&out, value.template_name_utf8, 256);
  PutSlot(&out, value.grantee_name_utf8, 256);
  PutSlot(&out, value.privilege_utf8, 128);
  const auto computed = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateBindDemand.V1",
      out.data() + 16, 688);
  if (NonZero(value.evidence) && value.evidence != computed) return {};
  PutSha(&out, computed);
  out.insert(out.end(), 16, 0);
  return out;
}

bool DecodeSblrSecurityCreatePrivilegeTemplateRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecurityCreatePrivilegeTemplateRequestV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "PTQX",
                   kSblrSecurityCreatePrivilegeTemplateRequestV1Bytes) ||
      !AllZero(bytes + 54, bytes + 64) ||
      !AllZero(bytes + 736, bytes + 752)) {
    if (detail != nullptr) *detail = "PTQX header or reserved bytes are invalid";
    return false;
  }
  SblrSecurityCreatePrivilegeTemplateRequestV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.template_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.command_identity = static_cast<std::uint16_t>(GetLe(bytes + 44, 2));
  value.object_kind =
      static_cast<PrivilegeTemplateObjectKindV1>(bytes[46]);
  const auto flags = bytes[47];
  if ((flags & 0xf0u) != 0 || (flags & (1u << 1)) == 0) {
    if (detail != nullptr) *detail = "PTQX flags are invalid";
    return false;
  }
  value.with_grant_option = (flags & 1u) != 0;
  value.enabled = true;
  value.template_name_quoted = (flags & (1u << 2)) != 0;
  value.grantee_name_quoted = (flags & (1u << 3)) != 0;
  if (!DecodeSlot(bytes + 64, 256, GetLe(bytes + 48, 2),
                  &value.template_name_utf8) ||
      !DecodeSlot(bytes + 320, 256, GetLe(bytes + 50, 2),
                  &value.grantee_name_utf8) ||
      !DecodeSlot(bytes + 576, 128, GetLe(bytes + 52, 2),
                  &value.privilege_utf8)) {
    if (detail != nullptr) *detail = "PTQX syntax slot is invalid";
    return false;
  }
  GetSha(bytes + 704, &value.evidence);
  const auto expected = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateBindDemand.V1",
      bytes + 16, 688);
  if (value.evidence != expected ||
      EncodeSblrSecurityCreatePrivilegeTemplateRequestV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "PTQX evidence or canonical form is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t>
EncodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
    const SblrSecurityCreatePrivilegeTemplateDescriptorV1& value,
    bool operand_magic) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.template_occurrence == 0 ||
      !ValidObjectKind(value.object_kind) || !value.enabled ||
      !NonZero(value.template_uuid) || value.template_generation == 0 ||
      !NonZero(value.owner_principal_uuid) || !NonZero(value.grantee_uuid) ||
      !NonZero(value.owning_transaction_uuid) ||
      value.owning_local_transaction_id == 0 ||
      !NonZero(value.statement_snapshot_uuid) ||
      !NonZero(value.catalog_epoch_uuid) || value.catalog_generation == 0 ||
      !NonZero(value.security_context_uuid) || value.security_epoch == 0 ||
      !NonZero(value.policy_snapshot_uuid) || value.policy_generation == 0 ||
      !NonZero(value.resource_grant_uuid) || value.resource_generation == 0 ||
      !NonZero(value.recovery_uuid) || value.recovery_generation == 0 ||
      !NonZero(value.syntax_demand_sha256) ||
      !NonZero(value.definition_sha256) ||
      !NonZero(value.idempotency_sha256) || value.availability == 0 ||
      value.template_uuid == value.recovery_uuid) {
    return {};
  }

  auto out = Header(operand_magic ? "PTDO" : "PTDD",
                    kSblrSecurityCreatePrivilegeTemplateDescriptorV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.template_occurrence, 4);
  PutLe(&out,
        DescriptorFlags(value.object_kind, value.with_grant_option,
                        value.enabled),
        4);
  PutUuid(&out, value.template_uuid);
  PutLe(&out, value.template_generation, 8);
  PutUuid(&out, value.owner_principal_uuid);
  PutUuid(&out, value.schema_uuid);
  PutUuid(&out, value.grantee_uuid);
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
  PutUuid(&out, value.recovery_uuid);
  PutLe(&out, value.recovery_generation, 8);
  PutSha(&out, value.syntax_demand_sha256);
  PutSha(&out, value.definition_sha256);
  PutSha(&out, value.idempotency_sha256);
  out.insert(out.end(), 40, 0);
  const auto computed = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateDescriptor.V1",
      out.data() + 16, 400);
  if (NonZero(value.evidence) && value.evidence != computed) return {};
  PutSha(&out, computed);
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), 32, 0);
  return out;
}

bool DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecurityCreatePrivilegeTemplateDescriptorV1* out,
    std::string* detail, bool operand_magic) {
  const auto expected_magic = operand_magic ? "PTDO" : "PTDD";
  if (out == nullptr ||
      !ValidHeader(bytes, size, expected_magic,
                   kSblrSecurityCreatePrivilegeTemplateDescriptorV1Bytes) ||
      !AllZero(bytes + 376, bytes + 416) ||
      !AllZero(bytes + 456, bytes + 488)) {
    if (detail != nullptr) *detail = "privilege-template descriptor framing is invalid";
    return false;
  }
  SblrSecurityCreatePrivilegeTemplateDescriptorV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.template_occurrence =
      static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  if (!DecodeDescriptorFlags(static_cast<std::uint32_t>(GetLe(bytes + 44, 4)),
                             &value.object_kind,
                             &value.with_grant_option, &value.enabled)) {
    if (detail != nullptr) *detail = "privilege-template descriptor flags are invalid";
    return false;
  }
  GetUuid(bytes + 48, &value.template_uuid);
  value.template_generation = GetLe(bytes + 64, 8);
  GetUuid(bytes + 72, &value.owner_principal_uuid);
  GetUuid(bytes + 88, &value.schema_uuid);
  GetUuid(bytes + 104, &value.grantee_uuid);
  GetUuid(bytes + 120, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 136, 8);
  GetUuid(bytes + 144, &value.statement_snapshot_uuid);
  GetUuid(bytes + 160, &value.catalog_epoch_uuid);
  value.catalog_generation = GetLe(bytes + 176, 8);
  GetUuid(bytes + 184, &value.security_context_uuid);
  value.security_epoch = GetLe(bytes + 200, 8);
  GetUuid(bytes + 208, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(bytes + 224, 8);
  GetUuid(bytes + 232, &value.resource_grant_uuid);
  value.resource_generation = GetLe(bytes + 248, 8);
  GetUuid(bytes + 256, &value.recovery_uuid);
  value.recovery_generation = GetLe(bytes + 272, 8);
  GetSha(bytes + 280, &value.syntax_demand_sha256);
  GetSha(bytes + 312, &value.definition_sha256);
  GetSha(bytes + 344, &value.idempotency_sha256);
  GetSha(bytes + 416, &value.evidence);
  value.availability = GetLe(bytes + 448, 8);
  const auto expected = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateDescriptor.V1",
      bytes + 16, 400);
  if (value.evidence != expected ||
      EncodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
          value, operand_magic) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "privilege-template descriptor evidence is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t>
EncodeSblrSecurityCreatePrivilegeTemplateResultV1(
    const SblrSecurityCreatePrivilegeTemplateResultV1& value) {
  if (!NonZero(value.receipt) || !NonZero(value.template_uuid) ||
      value.template_generation == 0 ||
      !NonZero(value.owner_principal_uuid) || !NonZero(value.grantee_uuid) ||
      !NonZero(value.owning_transaction_uuid) ||
      value.owning_local_transaction_id == 0 ||
      !NonZero(value.statement_snapshot_uuid) || value.catalog_generation == 0 ||
      value.security_generation == 0 || value.resource_generation == 0 ||
      !NonZero(value.definition_sha256) ||
      !NonZero(value.descriptor_evidence_sha256) ||
      !NonZero(value.mutation_uuid) || value.cache_invalidation_epoch == 0 ||
      !value.template_created || value.availability == 0 ||
      !NonZero(value.publication_barrier) ||
      value.template_uuid == value.mutation_uuid ||
      value.template_uuid == value.publication_barrier ||
      value.mutation_uuid == value.publication_barrier) {
    return {};
  }

  auto out = Header("PTRS",
                    kSblrSecurityCreatePrivilegeTemplateResultV1Bytes);
  PutUuid(&out, value.receipt);
  PutUuid(&out, value.template_uuid);
  PutLe(&out, value.template_generation, 8);
  PutUuid(&out, value.owner_principal_uuid);
  PutUuid(&out, value.grantee_uuid);
  PutUuid(&out, value.owning_transaction_uuid);
  PutLe(&out, value.owning_local_transaction_id, 8);
  PutUuid(&out, value.statement_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_generation, 8);
  PutLe(&out, value.resource_generation, 8);
  PutSha(&out, value.definition_sha256);
  PutSha(&out, value.descriptor_evidence_sha256);
  PutUuid(&out, value.mutation_uuid);
  PutLe(&out, value.cache_invalidation_epoch, 8);
  std::uint64_t flags = 1;
  if (value.exact_idempotent_replay) flags |= 1u << 1;
  PutLe(&out, flags, 8);
  out.insert(out.end(), 8, 0);
  const auto computed = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateResult.V1",
      out.data() + 16, 240);
  if (NonZero(value.evidence) && value.evidence != computed) return {};
  PutSha(&out, computed);
  PutLe(&out, value.availability, 8);
  PutUuid(&out, value.publication_barrier);
  out.insert(out.end(), 8, 0);
  return out;
}

bool DecodeSblrSecurityCreatePrivilegeTemplateResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecurityCreatePrivilegeTemplateResultV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "PTRS",
                   kSblrSecurityCreatePrivilegeTemplateResultV1Bytes) ||
      !AllZero(bytes + 248, bytes + 256) ||
      !AllZero(bytes + 312, bytes + 320)) {
    if (detail != nullptr) *detail = "PTRS framing is invalid";
    return false;
  }
  SblrSecurityCreatePrivilegeTemplateResultV1 value;
  GetUuid(bytes + 16, &value.receipt);
  GetUuid(bytes + 32, &value.template_uuid);
  value.template_generation = GetLe(bytes + 48, 8);
  GetUuid(bytes + 56, &value.owner_principal_uuid);
  GetUuid(bytes + 72, &value.grantee_uuid);
  GetUuid(bytes + 88, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 104, 8);
  GetUuid(bytes + 112, &value.statement_snapshot_uuid);
  value.catalog_generation = GetLe(bytes + 128, 8);
  value.security_generation = GetLe(bytes + 136, 8);
  value.resource_generation = GetLe(bytes + 144, 8);
  GetSha(bytes + 152, &value.definition_sha256);
  GetSha(bytes + 184, &value.descriptor_evidence_sha256);
  GetUuid(bytes + 216, &value.mutation_uuid);
  value.cache_invalidation_epoch = GetLe(bytes + 232, 8);
  const auto flags = GetLe(bytes + 240, 8);
  if ((flags & ~0x3ull) != 0 || (flags & 1u) == 0) {
    if (detail != nullptr) *detail = "PTRS status flags are invalid";
    return false;
  }
  value.template_created = true;
  value.exact_idempotent_replay = (flags & (1u << 1)) != 0;
  GetSha(bytes + 256, &value.evidence);
  value.availability = GetLe(bytes + 288, 8);
  GetUuid(bytes + 296, &value.publication_barrier);
  const auto expected = Evidence(
      "ScratchBird.SblrSecurityCreatePrivilegeTemplateResult.V1",
      bytes + 16, 240);
  if (value.evidence != expected ||
      EncodeSblrSecurityCreatePrivilegeTemplateResultV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "PTRS evidence or canonical form is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace scratchbird::engine::sblr
