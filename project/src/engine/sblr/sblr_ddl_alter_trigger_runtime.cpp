#include "sblr_ddl_alter_trigger_runtime.hpp"

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

DdlAlterTriggerSha Evidence(std::string_view domain,
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

bool ValidAtoms(const std::vector<SblrDdlAlterTriggerNameAtomV1>& atoms) {
  if (atoms.empty() || atoms.size() > 3) return false;
  return std::all_of(atoms.begin(), atoms.end(), [](const auto& atom) {
    return atom.raw_utf8.size() >= 1 && atom.raw_utf8.size() <= 256 &&
           ValidUtf8(atom.raw_utf8);
  });
}

bool ValidActions(std::uint16_t mask, DdlAlterTriggerEnabledStateV1 enabled,
                  std::uint16_t position,
                  DdlAlterTriggerSecurityModeV1 security,
                  DdlAlterTriggerFailurePolicyV1 failure) {
  if (mask == 0 || (mask & ~kDdlAlterTriggerActionMask) != 0 ||
      position > 32767) {
    return false;
  }
  const auto has = [mask](std::uint16_t bit) { return (mask & bit) != 0; };
  if (has(kDdlAlterTriggerActionEnabled) !=
          (enabled != DdlAlterTriggerEnabledStateV1::kPreserve) ||
      static_cast<unsigned>(enabled) > 2 ||
      (!has(kDdlAlterTriggerActionPosition) && position != 0) ||
      has(kDdlAlterTriggerActionSecurity) !=
          (security != DdlAlterTriggerSecurityModeV1::kPreserve) ||
      static_cast<unsigned>(security) > 2 ||
      has(kDdlAlterTriggerActionFailurePolicy) !=
          (failure != DdlAlterTriggerFailurePolicyV1::kPreserve) ||
      static_cast<unsigned>(failure) > 1) {
    return false;
  }
  return true;
}

std::uint32_t ActionFlags(std::uint16_t mask,
                          DdlAlterTriggerEnabledStateV1 enabled,
                          std::uint16_t position,
                          DdlAlterTriggerSecurityModeV1 security,
                          DdlAlterTriggerFailurePolicyV1 failure) {
  return static_cast<std::uint32_t>(mask) |
         (static_cast<std::uint32_t>(enabled) << 8) |
         (static_cast<std::uint32_t>(security) << 10) |
         (static_cast<std::uint32_t>(failure) << 12) |
         (static_cast<std::uint32_t>(position) << 16);
}

bool DecodeActionFlags(std::uint32_t flags, std::uint16_t* mask,
                       DdlAlterTriggerEnabledStateV1* enabled,
                       std::uint16_t* position,
                       DdlAlterTriggerSecurityModeV1* security,
                       DdlAlterTriggerFailurePolicyV1* failure) {
  if ((flags & 0x8000c0c0u) != 0) return false;
  *mask = static_cast<std::uint16_t>(flags & 0x3fu);
  *enabled = static_cast<DdlAlterTriggerEnabledStateV1>((flags >> 8) & 0x3u);
  *security = static_cast<DdlAlterTriggerSecurityModeV1>((flags >> 10) & 0x3u);
  *failure = static_cast<DdlAlterTriggerFailurePolicyV1>((flags >> 12) & 0x3u);
  *position = static_cast<std::uint16_t>((flags >> 16) & 0x7fffu);
  return ValidActions(*mask, *enabled, *position, *security, *failure);
}

void PutUuid(std::vector<std::uint8_t>* out, const DdlAlterTriggerUuid& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutSha(std::vector<std::uint8_t>* out, const DdlAlterTriggerSha& sha) {
  out->insert(out->end(), sha.begin(), sha.end());
}

void GetUuid(const std::uint8_t* bytes, DdlAlterTriggerUuid* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

void GetSha(const std::uint8_t* bytes, DdlAlterTriggerSha* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

bool PairwiseDistinct(
    std::initializer_list<const DdlAlterTriggerUuid*> identities) {
  for (auto outer = identities.begin(); outer != identities.end(); ++outer) {
    if (!NonZero(**outer)) return false;
    for (auto inner = identities.begin(); inner != outer; ++inner) {
      if (**outer == **inner) return false;
    }
  }
  return true;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerRequestV1(
    const SblrDdlAlterTriggerRequestV1& value) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.trigger_occurrence == 0 || value.command_identity != 1 ||
      !ValidActions(value.action_mask, value.enabled_state, value.position,
                    value.security_mode, value.failure_policy) ||
      !ValidAtoms(value.trigger_name_atoms)) {
    return {};
  }
  std::uint8_t quoted_mask = 0;
  for (std::size_t index = 0; index < value.trigger_name_atoms.size(); ++index) {
    if (value.trigger_name_atoms[index].quoted) {
      quoted_mask |= static_cast<std::uint8_t>(1u << index);
    }
  }
  auto out = Header("TAQX", kSblrDdlAlterTriggerRequestV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.trigger_occurrence, 4);
  PutLe(&out, value.command_identity, 2);
  PutLe(&out, value.action_mask, 2);
  out.push_back(static_cast<std::uint8_t>(value.enabled_state));
  out.push_back(static_cast<std::uint8_t>(value.security_mode));
  out.push_back(static_cast<std::uint8_t>(value.failure_policy));
  out.push_back(static_cast<std::uint8_t>(value.trigger_name_atoms.size()));
  out.push_back(quoted_mask);
  out.push_back(0);
  PutLe(&out, value.position, 2);
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
      "ScratchBird.SblrDdlAlterTriggerBindDemand.V1", out.data() + 16, 814);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  out.insert(out.end(), 2, 0);
  return out.size() == kSblrDdlAlterTriggerRequestV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlAlterTriggerRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlAlterTriggerRequestV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "TAQX", kSblrDdlAlterTriggerRequestV1Bytes) ||
      bytes[53] != 0 || !AllZero(bytes + 862, bytes + 864)) {
    if (detail != nullptr) *detail = "TAQX header or reserved bytes are invalid";
    return false;
  }
  SblrDdlAlterTriggerRequestV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.trigger_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.command_identity = static_cast<std::uint16_t>(GetLe(bytes + 44, 2));
  value.action_mask = static_cast<std::uint16_t>(GetLe(bytes + 46, 2));
  value.enabled_state = static_cast<DdlAlterTriggerEnabledStateV1>(bytes[48]);
  value.security_mode = static_cast<DdlAlterTriggerSecurityModeV1>(bytes[49]);
  value.failure_policy = static_cast<DdlAlterTriggerFailurePolicyV1>(bytes[50]);
  const auto count = bytes[51];
  const auto quoted_mask = bytes[52];
  value.position = static_cast<std::uint16_t>(GetLe(bytes + 54, 2));
  if (count == 0 || count > 3 ||
      (quoted_mask & ~((1u << count) - 1u)) != 0) {
    if (detail != nullptr) *detail = "TAQX trigger-name shape is invalid";
    return false;
  }
  for (std::size_t index = 0; index < 3; ++index) {
    const auto length = static_cast<std::size_t>(GetLe(bytes + 56 + index * 2, 2));
    const auto* slot = bytes + 62 + index * 256;
    if ((index >= count && length != 0) || length > 256 ||
        !AllZero(slot + length, slot + 256)) {
      if (detail != nullptr) *detail = "TAQX trigger-name extent is invalid";
      return false;
    }
    if (index < count) {
      SblrDdlAlterTriggerNameAtomV1 atom;
      atom.raw_utf8.assign(reinterpret_cast<const char*>(slot), length);
      atom.quoted = (quoted_mask & (1u << index)) != 0;
      if (!ValidUtf8(atom.raw_utf8)) {
        if (detail != nullptr) *detail = "TAQX trigger-name UTF-8 is invalid";
        return false;
      }
      value.trigger_name_atoms.push_back(std::move(atom));
    }
  }
  GetSha(bytes + 830, &value.evidence);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlAlterTriggerBindDemand.V1", bytes + 16, 814);
  if (value.evidence != expected ||
      EncodeSblrDdlAlterTriggerRequestV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "TAQX evidence or canonical bytes are invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerDescriptorV1(
    const SblrDdlAlterTriggerDescriptorV1& value, bool operand_magic) {
  if (!ValidActions(value.action_mask, value.enabled_state, value.position,
                    value.security_mode, value.failure_policy) ||
      value.occurrence == 0 || value.trigger_occurrence == 0 ||
      value.trigger_generation == 0 || value.target_relation_generation == 0 ||
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
  auto out = Header(operand_magic ? "TADO" : "TADX",
                    kSblrDdlAlterTriggerDescriptorV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.trigger_occurrence, 4);
  PutLe(&out, ActionFlags(value.action_mask, value.enabled_state,
                          value.position, value.security_mode,
                          value.failure_policy), 4);
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
      "ScratchBird.SblrDdlAlterTriggerDescriptor.V1", out.data() + 16, 400);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), 32, 0);
  return out.size() == kSblrDdlAlterTriggerDescriptorV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlAlterTriggerDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlAlterTriggerDescriptorV1* out, std::string* detail,
    bool operand_magic) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, operand_magic ? "TADO" : "TADX",
                   kSblrDdlAlterTriggerDescriptorV1Bytes) ||
      !AllZero(bytes + 408, bytes + 416) ||
      !AllZero(bytes + 456, bytes + 488)) {
    if (detail != nullptr) *detail = "TADX/TADO header or reserved bytes are invalid";
    return false;
  }
  SblrDdlAlterTriggerDescriptorV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.trigger_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  if (!DecodeActionFlags(static_cast<std::uint32_t>(GetLe(bytes + 44, 4)),
                         &value.action_mask, &value.enabled_state,
                         &value.position, &value.security_mode,
                         &value.failure_policy)) {
    if (detail != nullptr) *detail = "TADX/TADO action flags are invalid";
    return false;
  }
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
      "ScratchBird.SblrDdlAlterTriggerDescriptor.V1", bytes + 16, 400);
  if (value.evidence != expected ||
      EncodeSblrDdlAlterTriggerDescriptorV1(value, operand_magic) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "TADX/TADO authority or evidence is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlAlterTriggerResultV1(
    const SblrDdlAlterTriggerResultV1& value) {
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
  auto out = Header("TARS", kSblrDdlAlterTriggerResultV1Bytes);
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
      "ScratchBird.SblrDdlAlterTriggerResult.V1", out.data() + 16, 240);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  PutUuid(&out, value.publication_barrier);
  out.insert(out.end(), 8, 0);
  return out.size() == kSblrDdlAlterTriggerResultV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlAlterTriggerResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlAlterTriggerResultV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "TARS", kSblrDdlAlterTriggerResultV1Bytes) ||
      !AllZero(bytes + 312, bytes + 320)) {
    if (detail != nullptr) *detail = "TARS header or reserved bytes are invalid";
    return false;
  }
  SblrDdlAlterTriggerResultV1 value;
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
      "ScratchBird.SblrDdlAlterTriggerResult.V1", bytes + 16, 240);
  if (value.evidence != expected ||
      EncodeSblrDdlAlterTriggerResultV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "TARS authority or evidence is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace scratchbird::engine::sblr
