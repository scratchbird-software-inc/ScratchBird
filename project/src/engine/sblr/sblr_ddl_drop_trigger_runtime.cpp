#include "sblr_ddl_drop_trigger_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <initializer_list>
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

DdlDropTriggerSha Evidence(std::string_view domain,
                           const std::uint8_t* bytes, std::size_t size) {
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

bool ValidAtoms(const std::vector<SblrDdlDropTriggerNameAtomV1>& atoms) {
  if (atoms.empty() || atoms.size() > 3) return false;
  return std::all_of(atoms.begin(), atoms.end(), [](const auto& atom) {
    return atom.raw_utf8.size() >= 1 && atom.raw_utf8.size() <= 256 &&
           ValidUtf8(atom.raw_utf8);
  });
}

bool ValidDependencyMode(DdlDropTriggerDependencyModeV1 mode) {
  return mode == DdlDropTriggerDependencyModeV1::kRestrict ||
         mode == DdlDropTriggerDependencyModeV1::kCascade;
}

void PutUuid(std::vector<std::uint8_t>* out, const DdlDropTriggerUuid& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutSha(std::vector<std::uint8_t>* out, const DdlDropTriggerSha& sha) {
  out->insert(out->end(), sha.begin(), sha.end());
}

void GetUuid(const std::uint8_t* bytes, DdlDropTriggerUuid* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

void GetSha(const std::uint8_t* bytes, DdlDropTriggerSha* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

bool PairwiseDistinct(
    std::initializer_list<const DdlDropTriggerUuid*> identities) {
  for (auto outer = identities.begin(); outer != identities.end(); ++outer) {
    if (!NonZero(**outer)) return false;
    for (auto inner = identities.begin(); inner != outer; ++inner) {
      if (**outer == **inner) return false;
    }
  }
  return true;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrDdlDropTriggerRequestV1(
    const SblrDdlDropTriggerRequestV1& value) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.trigger_occurrence == 0 || value.command_identity != 1 ||
      !ValidDependencyMode(value.dependency_mode) ||
      !ValidAtoms(value.trigger_name_atoms)) {
    return {};
  }
  std::uint8_t quoted_mask = 0;
  for (std::size_t index = 0; index < value.trigger_name_atoms.size(); ++index) {
    if (value.trigger_name_atoms[index].quoted) {
      quoted_mask |= static_cast<std::uint8_t>(1u << index);
    }
  }
  auto out = Header("TDQX", kSblrDdlDropTriggerRequestV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.trigger_occurrence, 4);
  PutLe(&out, value.command_identity, 2);
  out.push_back(static_cast<std::uint8_t>(value.dependency_mode));
  out.push_back(static_cast<std::uint8_t>(value.trigger_name_atoms.size()));
  out.push_back(quoted_mask);
  out.push_back(0);
  for (std::size_t index = 0; index < 3; ++index) {
    PutLe(&out, index < value.trigger_name_atoms.size()
                    ? value.trigger_name_atoms[index].raw_utf8.size()
                    : 0,
          2);
  }
  for (std::size_t index = 0; index < 3; ++index) {
    if (index < value.trigger_name_atoms.size()) {
      const auto& atom = value.trigger_name_atoms[index].raw_utf8;
      out.insert(out.end(), atom.begin(), atom.end());
      out.insert(out.end(), 256 - atom.size(), 0);
    } else {
      out.insert(out.end(), 256, 0);
    }
  }
  const auto evidence = Evidence(
      "ScratchBird.SblrDdlDropTriggerBindDemand.V1", out.data() + 16, 808);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  out.insert(out.end(), 8, 0);
  return out.size() == kSblrDdlDropTriggerRequestV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlDropTriggerRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropTriggerRequestV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "TDQX", kSblrDdlDropTriggerRequestV1Bytes) ||
      bytes[49] != 0 || !AllZero(bytes + 856, bytes + 864)) {
    if (detail != nullptr) *detail = "TDQX header or reserved bytes are invalid";
    return false;
  }
  SblrDdlDropTriggerRequestV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.trigger_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.command_identity = static_cast<std::uint16_t>(GetLe(bytes + 44, 2));
  value.dependency_mode =
      static_cast<DdlDropTriggerDependencyModeV1>(bytes[46]);
  const auto count = bytes[47];
  const auto quoted_mask = bytes[48];
  if (count == 0 || count > 3 ||
      (quoted_mask & ~((1u << count) - 1u)) != 0) {
    if (detail != nullptr) *detail = "TDQX trigger-name shape is invalid";
    return false;
  }
  for (std::size_t index = 0; index < 3; ++index) {
    const auto length = static_cast<std::size_t>(GetLe(bytes + 50 + index * 2, 2));
    const auto* slot = bytes + 56 + index * 256;
    if ((index >= count && length != 0) || length > 256 ||
        !AllZero(slot + length, slot + 256)) {
      if (detail != nullptr) *detail = "TDQX trigger-name extent is invalid";
      return false;
    }
    if (index < count) {
      SblrDdlDropTriggerNameAtomV1 atom;
      atom.raw_utf8.assign(reinterpret_cast<const char*>(slot), length);
      atom.quoted = (quoted_mask & (1u << index)) != 0;
      if (!ValidUtf8(atom.raw_utf8)) {
        if (detail != nullptr) *detail = "TDQX trigger-name UTF-8 is invalid";
        return false;
      }
      value.trigger_name_atoms.push_back(std::move(atom));
    }
  }
  GetSha(bytes + 824, &value.evidence);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlDropTriggerBindDemand.V1", bytes + 16, 808);
  if (value.evidence != expected ||
      EncodeSblrDdlDropTriggerRequestV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "TDQX evidence or canonical bytes are invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlDropTriggerDescriptorV1(
    const SblrDdlDropTriggerDescriptorV1& value, bool operand_magic) {
  if (!ValidDependencyMode(value.dependency_mode) || value.occurrence == 0 ||
      value.trigger_occurrence == 0 || value.trigger_generation == 0 ||
      value.target_relation_generation == 0 ||
      value.target_relation_descriptor_generation == 0 ||
      value.schema_generation == 0 || value.owning_local_transaction_id == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.policy_generation == 0 || value.resource_generation == 0 ||
      value.body_sblr_generation == 0 || value.recovery_generation == 0 ||
      value.availability == 0 || !NonZero(value.syntax_demand_sha256) ||
      !NonZero(value.authority_bundle_sha256) ||
      !PairwiseDistinct(
          {&value.receipt, &value.trigger_uuid, &value.target_relation_uuid,
           &value.target_relation_descriptor_uuid, &value.schema_uuid,
           &value.owning_transaction_uuid, &value.statement_snapshot_uuid,
           &value.catalog_epoch_uuid, &value.security_context_uuid,
           &value.policy_snapshot_uuid, &value.resource_grant_uuid,
           &value.owner_principal_uuid, &value.body_sblr_uuid,
           &value.recovery_uuid})) {
    return {};
  }
  auto out = Header(operand_magic ? "TDDO" : "TDDX",
                    kSblrDdlDropTriggerDescriptorV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.trigger_occurrence, 4);
  PutLe(&out, static_cast<std::uint8_t>(value.dependency_mode), 4);
  PutUuid(&out, value.trigger_uuid);
  PutLe(&out, value.trigger_generation, 8);
  PutUuid(&out, value.target_relation_uuid);
  PutLe(&out, value.target_relation_generation, 8);
  PutUuid(&out, value.target_relation_descriptor_uuid);
  PutLe(&out, value.target_relation_descriptor_generation, 8);
  PutUuid(&out, value.schema_uuid);
  PutLe(&out, value.schema_generation, 8);
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
  PutUuid(&out, value.body_sblr_uuid);
  PutLe(&out, value.body_sblr_generation, 8);
  PutUuid(&out, value.recovery_uuid);
  PutLe(&out, value.recovery_generation, 8);
  PutSha(&out, value.syntax_demand_sha256);
  PutSha(&out, value.authority_bundle_sha256);
  out.insert(out.end(), 8, 0);
  const auto evidence = Evidence(
      "ScratchBird.SblrDdlDropTriggerDescriptor.V1", out.data() + 16, 400);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), 32, 0);
  return out.size() == kSblrDdlDropTriggerDescriptorV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlDropTriggerDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropTriggerDescriptorV1* out, std::string* detail,
    bool operand_magic) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, operand_magic ? "TDDO" : "TDDX",
                   kSblrDdlDropTriggerDescriptorV1Bytes) ||
      !AllZero(bytes + 408, bytes + 416) ||
      !AllZero(bytes + 456, bytes + 488)) {
    if (detail != nullptr) *detail = "TDDX/TDDO header or reserved bytes are invalid";
    return false;
  }
  SblrDdlDropTriggerDescriptorV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.trigger_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  const auto flags = static_cast<std::uint32_t>(GetLe(bytes + 44, 4));
  if ((flags & ~0x3u) != 0) {
    if (detail != nullptr) *detail = "TDDX/TDDO dependency flags are invalid";
    return false;
  }
  value.dependency_mode = static_cast<DdlDropTriggerDependencyModeV1>(flags);
  GetUuid(bytes + 48, &value.trigger_uuid);
  value.trigger_generation = GetLe(bytes + 64, 8);
  GetUuid(bytes + 72, &value.target_relation_uuid);
  value.target_relation_generation = GetLe(bytes + 88, 8);
  GetUuid(bytes + 96, &value.target_relation_descriptor_uuid);
  value.target_relation_descriptor_generation = GetLe(bytes + 112, 8);
  GetUuid(bytes + 120, &value.schema_uuid);
  value.schema_generation = GetLe(bytes + 136, 8);
  GetUuid(bytes + 144, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 160, 8);
  GetUuid(bytes + 168, &value.statement_snapshot_uuid);
  GetUuid(bytes + 184, &value.catalog_epoch_uuid);
  value.catalog_generation = GetLe(bytes + 200, 8);
  GetUuid(bytes + 208, &value.security_context_uuid);
  value.security_epoch = GetLe(bytes + 224, 8);
  GetUuid(bytes + 232, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(bytes + 248, 8);
  GetUuid(bytes + 256, &value.resource_grant_uuid);
  value.resource_generation = GetLe(bytes + 272, 8);
  GetUuid(bytes + 280, &value.owner_principal_uuid);
  GetUuid(bytes + 296, &value.body_sblr_uuid);
  value.body_sblr_generation = GetLe(bytes + 312, 8);
  GetUuid(bytes + 320, &value.recovery_uuid);
  value.recovery_generation = GetLe(bytes + 336, 8);
  GetSha(bytes + 344, &value.syntax_demand_sha256);
  GetSha(bytes + 376, &value.authority_bundle_sha256);
  GetSha(bytes + 416, &value.evidence);
  value.availability = GetLe(bytes + 448, 8);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlDropTriggerDescriptor.V1", bytes + 16, 400);
  if (value.evidence != expected ||
      EncodeSblrDdlDropTriggerDescriptorV1(value, operand_magic) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "TDDX/TDDO authority or evidence is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlDropTriggerResultV1(
    const SblrDdlDropTriggerResultV1& value) {
  if (value.trigger_generation == 0 || value.target_relation_generation == 0 ||
      value.schema_generation == 0 || value.owning_local_transaction_id == 0 ||
      value.body_sblr_generation == 0 || value.catalog_generation == 0 ||
      value.security_epoch == 0 || value.resource_generation == 0 ||
      value.availability == 0 ||
      !NonZero(value.descriptor_evidence_sha256) ||
      !PairwiseDistinct(
          {&value.receipt, &value.trigger_uuid, &value.target_relation_uuid,
           &value.schema_uuid, &value.owning_transaction_uuid,
           &value.statement_snapshot_uuid, &value.catalog_row_uuid,
           &value.mutation_uuid, &value.body_sblr_uuid,
           &value.publication_barrier})) {
    return {};
  }
  auto out = Header("TDRS", kSblrDdlDropTriggerResultV1Bytes);
  PutUuid(&out, value.receipt);
  PutUuid(&out, value.trigger_uuid);
  PutLe(&out, value.trigger_generation, 8);
  PutUuid(&out, value.target_relation_uuid);
  PutLe(&out, value.target_relation_generation, 8);
  PutUuid(&out, value.schema_uuid);
  PutLe(&out, value.schema_generation, 8);
  PutUuid(&out, value.owning_transaction_uuid);
  PutLe(&out, value.owning_local_transaction_id, 8);
  PutUuid(&out, value.statement_snapshot_uuid);
  PutUuid(&out, value.catalog_row_uuid);
  PutUuid(&out, value.mutation_uuid);
  PutUuid(&out, value.body_sblr_uuid);
  PutLe(&out, value.body_sblr_generation, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_generation, 8);
  PutSha(&out, value.descriptor_evidence_sha256);
  const auto evidence = Evidence(
      "ScratchBird.SblrDdlDropTriggerResult.V1", out.data() + 16, 240);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  PutUuid(&out, value.publication_barrier);
  out.insert(out.end(), 8, 0);
  return out.size() == kSblrDdlDropTriggerResultV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlDropTriggerResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlDropTriggerResultV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "TDRS", kSblrDdlDropTriggerResultV1Bytes) ||
      !AllZero(bytes + 312, bytes + 320)) {
    if (detail != nullptr) *detail = "TDRS header or reserved bytes are invalid";
    return false;
  }
  SblrDdlDropTriggerResultV1 value;
  GetUuid(bytes + 16, &value.receipt);
  GetUuid(bytes + 32, &value.trigger_uuid);
  value.trigger_generation = GetLe(bytes + 48, 8);
  GetUuid(bytes + 56, &value.target_relation_uuid);
  value.target_relation_generation = GetLe(bytes + 72, 8);
  GetUuid(bytes + 80, &value.schema_uuid);
  value.schema_generation = GetLe(bytes + 96, 8);
  GetUuid(bytes + 104, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 120, 8);
  GetUuid(bytes + 128, &value.statement_snapshot_uuid);
  GetUuid(bytes + 144, &value.catalog_row_uuid);
  GetUuid(bytes + 160, &value.mutation_uuid);
  GetUuid(bytes + 176, &value.body_sblr_uuid);
  value.body_sblr_generation = GetLe(bytes + 192, 8);
  value.catalog_generation = GetLe(bytes + 200, 8);
  value.security_epoch = GetLe(bytes + 208, 8);
  value.resource_generation = GetLe(bytes + 216, 8);
  GetSha(bytes + 224, &value.descriptor_evidence_sha256);
  GetSha(bytes + 256, &value.evidence);
  value.availability = GetLe(bytes + 288, 8);
  GetUuid(bytes + 296, &value.publication_barrier);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlDropTriggerResult.V1", bytes + 16, 240);
  if (value.evidence != expected ||
      EncodeSblrDdlDropTriggerResultV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "TDRS authority or evidence is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace scratchbird::engine::sblr
