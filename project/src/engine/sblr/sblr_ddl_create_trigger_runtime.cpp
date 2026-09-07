#include "sblr_ddl_create_trigger_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>
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

DdlCreateTriggerSha Evidence(std::string_view domain,
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

void PutUuid(std::vector<std::uint8_t>* out,
             const DdlCreateTriggerUuid& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutSha(std::vector<std::uint8_t>* out, const DdlCreateTriggerSha& sha) {
  out->insert(out->end(), sha.begin(), sha.end());
}

void GetUuid(const std::uint8_t* bytes, DdlCreateTriggerUuid* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

void GetSha(const std::uint8_t* bytes, DdlCreateTriggerSha* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

template <typename Enum>
std::uint32_t EnumValue(Enum value) {
  return static_cast<std::uint32_t>(value);
}

bool ValidDefinition(DdlCreateTriggerTimingV1 timing,
                     DdlCreateTriggerEventV1 event,
                     DdlCreateTriggerScopeV1 scope,
                     DdlCreateTriggerTargetKindV1 target_kind,
                     DdlCreateTriggerBodyProfileV1 body_profile,
                     std::uint16_t position,
                     DdlCreateTriggerSecurityModeV1 security_mode,
                     DdlCreateTriggerImageModeV1 image_mode,
                     DdlCreateTriggerExecutionModeV1 execution_mode,
                     DdlCreateTriggerEnabledStateV1 enabled_state,
                     DdlCreateTriggerRecursionModeV1 recursion_mode) {
  if (EnumValue(timing) < 1 || EnumValue(timing) > 3 ||
      EnumValue(event) < 1 || EnumValue(event) > 3 ||
      EnumValue(scope) < 1 || EnumValue(scope) > 2 ||
      target_kind != DdlCreateTriggerTargetKindV1::kTable ||
      EnumValue(body_profile) < 1 || EnumValue(body_profile) > 4 ||
      position > 32767 || EnumValue(security_mode) < 1 ||
      EnumValue(security_mode) > 2 || EnumValue(image_mode) < 1 ||
      EnumValue(image_mode) > 2 || EnumValue(execution_mode) < 1 ||
      EnumValue(execution_mode) > 2 || EnumValue(enabled_state) < 1 ||
      EnumValue(enabled_state) > 2 || EnumValue(recursion_mode) < 1 ||
      EnumValue(recursion_mode) > 3) {
    return false;
  }
  if (timing != DdlCreateTriggerTimingV1::kAfter) return false;
  switch (body_profile) {
    case DdlCreateTriggerBodyProfileV1::kAuditAfterInsertRow:
      return event == DdlCreateTriggerEventV1::kInsert &&
             scope == DdlCreateTriggerScopeV1::kRow;
    case DdlCreateTriggerBodyProfileV1::kAuditAfterUpdateRow:
      return event == DdlCreateTriggerEventV1::kUpdate &&
             scope == DdlCreateTriggerScopeV1::kRow;
    case DdlCreateTriggerBodyProfileV1::kAuditAfterDeleteRow:
      return event == DdlCreateTriggerEventV1::kDelete &&
             scope == DdlCreateTriggerScopeV1::kRow;
    case DdlCreateTriggerBodyProfileV1::kAuditAfterUpdateStatement:
      return event == DdlCreateTriggerEventV1::kUpdate &&
             scope == DdlCreateTriggerScopeV1::kStatement;
  }
  return false;
}

std::uint32_t DefinitionFlags(
    DdlCreateTriggerTimingV1 timing, DdlCreateTriggerEventV1 event,
    DdlCreateTriggerScopeV1 scope,
    DdlCreateTriggerTargetKindV1 target_kind,
    DdlCreateTriggerBodyProfileV1 body_profile, std::uint16_t position,
    DdlCreateTriggerSecurityModeV1 security_mode,
    DdlCreateTriggerImageModeV1 image_mode,
    DdlCreateTriggerExecutionModeV1 execution_mode,
    DdlCreateTriggerEnabledStateV1 enabled_state,
    DdlCreateTriggerRecursionModeV1 recursion_mode) {
  return EnumValue(timing) | (EnumValue(event) << 2) |
         ((EnumValue(scope) - 1) << 4) |
         ((EnumValue(target_kind) - 1) << 5) |
         (EnumValue(body_profile) << 6) |
         (static_cast<std::uint32_t>(position) << 9) |
         ((EnumValue(security_mode) - 1) << 24) |
         ((EnumValue(image_mode) - 1) << 25) |
         ((EnumValue(execution_mode) - 1) << 26) |
         ((EnumValue(enabled_state) - 1) << 27) |
         ((EnumValue(recursion_mode) - 1) << 28);
}

bool DecodeDefinitionFlags(
    std::uint32_t flags, DdlCreateTriggerTimingV1* timing,
    DdlCreateTriggerEventV1* event, DdlCreateTriggerScopeV1* scope,
    DdlCreateTriggerTargetKindV1* target_kind,
    DdlCreateTriggerBodyProfileV1* body_profile, std::uint16_t* position,
    DdlCreateTriggerSecurityModeV1* security_mode,
    DdlCreateTriggerImageModeV1* image_mode,
    DdlCreateTriggerExecutionModeV1* execution_mode,
    DdlCreateTriggerEnabledStateV1* enabled_state,
    DdlCreateTriggerRecursionModeV1* recursion_mode) {
  if ((flags & 0xc0000000u) != 0) return false;
  *timing = static_cast<DdlCreateTriggerTimingV1>(flags & 0x3u);
  *event = static_cast<DdlCreateTriggerEventV1>((flags >> 2) & 0x3u);
  *scope = static_cast<DdlCreateTriggerScopeV1>(((flags >> 4) & 0x1u) + 1);
  *target_kind =
      static_cast<DdlCreateTriggerTargetKindV1>(((flags >> 5) & 0x1u) + 1);
  *body_profile =
      static_cast<DdlCreateTriggerBodyProfileV1>((flags >> 6) & 0x7u);
  *position = static_cast<std::uint16_t>((flags >> 9) & 0x7fffu);
  *security_mode = static_cast<DdlCreateTriggerSecurityModeV1>(
      ((flags >> 24) & 0x1u) + 1);
  *image_mode = static_cast<DdlCreateTriggerImageModeV1>(
      ((flags >> 25) & 0x1u) + 1);
  *execution_mode = static_cast<DdlCreateTriggerExecutionModeV1>(
      ((flags >> 26) & 0x1u) + 1);
  *enabled_state = static_cast<DdlCreateTriggerEnabledStateV1>(
      ((flags >> 27) & 0x1u) + 1);
  *recursion_mode = static_cast<DdlCreateTriggerRecursionModeV1>(
      ((flags >> 28) & 0x3u) + 1);
  return ValidDefinition(*timing, *event, *scope, *target_kind, *body_profile,
                         *position, *security_mode, *image_mode,
                         *execution_mode, *enabled_state, *recursion_mode);
}

bool ValidAtoms(const std::vector<SblrDdlCreateTriggerNameAtomV1>& atoms) {
  if (atoms.empty() || atoms.size() > 3) return false;
  return std::all_of(atoms.begin(), atoms.end(), [](const auto& atom) {
    return atom.raw_utf8.size() >= 1 && atom.raw_utf8.size() <= 256 &&
           ValidUtf8(atom.raw_utf8);
  });
}

bool PairwiseDistinct(
    std::initializer_list<const DdlCreateTriggerUuid*> identities) {
  for (auto outer = identities.begin(); outer != identities.end(); ++outer) {
    if (!NonZero(**outer)) return false;
    for (auto inner = identities.begin(); inner != outer; ++inner) {
      if (**outer == **inner) return false;
    }
  }
  return true;
}

void PutAtomGroup(
    std::vector<std::uint8_t>* out,
    const std::vector<SblrDdlCreateTriggerNameAtomV1>& atoms) {
  for (std::size_t index = 0; index < 3; ++index) {
    if (index < atoms.size()) {
      out->insert(out->end(), atoms[index].raw_utf8.begin(),
                  atoms[index].raw_utf8.end());
      out->insert(out->end(), 256 - atoms[index].raw_utf8.size(), 0);
    } else {
      out->insert(out->end(), 256, 0);
    }
  }
}

bool DecodeAtomGroup(
    const std::uint8_t* slots, const std::uint16_t* lengths,
    std::uint8_t count, std::uint8_t quoted_mask,
    std::vector<SblrDdlCreateTriggerNameAtomV1>* atoms,
    std::string* detail) {
  if (count == 0 || count > 3 ||
      (quoted_mask & ~((1u << count) - 1u)) != 0) {
    if (detail != nullptr) *detail = "TVQX name atom count or quote mask is invalid";
    return false;
  }
  for (std::size_t index = 0; index < 3; ++index) {
    const auto length = static_cast<std::size_t>(lengths[index]);
    const auto* slot = slots + index * 256;
    if ((index >= count && length != 0) || length > 256 ||
        !AllZero(slot + length, slot + 256)) {
      if (detail != nullptr) *detail = "TVQX name atom extent is invalid";
      return false;
    }
    if (index < count) {
      if (length == 0) {
        if (detail != nullptr) *detail = "TVQX name atom is empty";
        return false;
      }
      SblrDdlCreateTriggerNameAtomV1 atom;
      atom.raw_utf8.assign(reinterpret_cast<const char*>(slot), length);
      atom.quoted = (quoted_mask & (1u << index)) != 0;
      if (!ValidUtf8(atom.raw_utf8)) {
        if (detail != nullptr) *detail = "TVQX name atom UTF-8 is invalid";
        return false;
      }
      atoms->push_back(std::move(atom));
    }
  }
  return true;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerRequestV1(
    const SblrDdlCreateTriggerRequestV1& value) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.trigger_occurrence == 0 || value.command_identity != 1 ||
      !ValidDefinition(value.timing, value.event, value.scope,
                       value.target_kind, value.body_profile, value.position,
                       value.security_mode, value.image_mode,
                       value.execution_mode, value.enabled_state,
                       value.recursion_mode) ||
      !ValidAtoms(value.trigger_name_atoms) ||
      !ValidAtoms(value.target_name_atoms)) {
    return {};
  }
  std::uint8_t trigger_quoted_mask = 0;
  std::uint8_t target_quoted_mask = 0;
  for (std::size_t index = 0; index < value.trigger_name_atoms.size(); ++index) {
    if (value.trigger_name_atoms[index].quoted) {
      trigger_quoted_mask |= static_cast<std::uint8_t>(1u << index);
    }
  }
  for (std::size_t index = 0; index < value.target_name_atoms.size(); ++index) {
    if (value.target_name_atoms[index].quoted) {
      target_quoted_mask |= static_cast<std::uint8_t>(1u << index);
    }
  }

  auto out = Header("TVQX", kSblrDdlCreateTriggerRequestV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.trigger_occurrence, 4);
  PutLe(&out, value.command_identity, 2);
  out.push_back(static_cast<std::uint8_t>(value.timing));
  out.push_back(static_cast<std::uint8_t>(value.event));
  out.push_back(static_cast<std::uint8_t>(value.scope));
  out.push_back(static_cast<std::uint8_t>(value.target_kind));
  PutLe(&out, static_cast<std::uint16_t>(value.body_profile), 2);
  out.push_back(static_cast<std::uint8_t>(value.trigger_name_atoms.size()));
  out.push_back(trigger_quoted_mask);
  out.push_back(static_cast<std::uint8_t>(value.target_name_atoms.size()));
  out.push_back(target_quoted_mask);
  for (std::size_t index = 0; index < 3; ++index) {
    PutLe(&out, index < value.trigger_name_atoms.size()
                    ? value.trigger_name_atoms[index].raw_utf8.size()
                    : 0,
          2);
  }
  for (std::size_t index = 0; index < 3; ++index) {
    PutLe(&out, index < value.target_name_atoms.size()
                    ? value.target_name_atoms[index].raw_utf8.size()
                    : 0,
          2);
  }
  PutLe(&out, value.position, 2);
  out.push_back(static_cast<std::uint8_t>(value.security_mode));
  out.push_back(static_cast<std::uint8_t>(value.image_mode));
  out.push_back(static_cast<std::uint8_t>(value.execution_mode));
  out.push_back(static_cast<std::uint8_t>(value.enabled_state));
  out.push_back(static_cast<std::uint8_t>(value.recursion_mode));
  out.insert(out.end(), 5, 0);
  PutAtomGroup(&out, value.trigger_name_atoms);
  PutAtomGroup(&out, value.target_name_atoms);
  const auto evidence = Evidence(
      "ScratchBird.SblrDdlCreateTriggerBindDemand.V1", out.data() + 16,
      1600);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  out.insert(out.end(), 16, 0);
  return out.size() == kSblrDdlCreateTriggerRequestV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlCreateTriggerRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlCreateTriggerRequestV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "TVQX", kSblrDdlCreateTriggerRequestV1Bytes) ||
      !AllZero(bytes + 75, bytes + 80) ||
      !AllZero(bytes + 1648, bytes + 1664)) {
    if (detail != nullptr) *detail = "TVQX header or reserved bytes are invalid";
    return false;
  }
  SblrDdlCreateTriggerRequestV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.trigger_occurrence =
      static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.command_identity = static_cast<std::uint16_t>(GetLe(bytes + 44, 2));
  value.timing = static_cast<DdlCreateTriggerTimingV1>(bytes[46]);
  value.event = static_cast<DdlCreateTriggerEventV1>(bytes[47]);
  value.scope = static_cast<DdlCreateTriggerScopeV1>(bytes[48]);
  value.target_kind = static_cast<DdlCreateTriggerTargetKindV1>(bytes[49]);
  value.body_profile =
      static_cast<DdlCreateTriggerBodyProfileV1>(GetLe(bytes + 50, 2));
  value.position = static_cast<std::uint16_t>(GetLe(bytes + 68, 2));
  value.security_mode = static_cast<DdlCreateTriggerSecurityModeV1>(bytes[70]);
  value.image_mode = static_cast<DdlCreateTriggerImageModeV1>(bytes[71]);
  value.execution_mode =
      static_cast<DdlCreateTriggerExecutionModeV1>(bytes[72]);
  value.enabled_state =
      static_cast<DdlCreateTriggerEnabledStateV1>(bytes[73]);
  value.recursion_mode =
      static_cast<DdlCreateTriggerRecursionModeV1>(bytes[74]);
  if (!ValidDefinition(value.timing, value.event, value.scope,
                       value.target_kind, value.body_profile, value.position,
                       value.security_mode, value.image_mode,
                       value.execution_mode, value.enabled_state,
                       value.recursion_mode)) {
    if (detail != nullptr) *detail = "TVQX trigger definition profile is invalid";
    return false;
  }
  std::uint16_t lengths[6]{};
  for (std::size_t index = 0; index < 6; ++index) {
    lengths[index] =
        static_cast<std::uint16_t>(GetLe(bytes + 56 + index * 2, 2));
  }
  if (!DecodeAtomGroup(bytes + 80, lengths, bytes[52], bytes[53],
                       &value.trigger_name_atoms, detail) ||
      !DecodeAtomGroup(bytes + 848, lengths + 3, bytes[54], bytes[55],
                       &value.target_name_atoms, detail)) {
    return false;
  }
  GetSha(bytes + 1616, &value.evidence);
  const auto expected = Evidence(
      "ScratchBird.SblrDdlCreateTriggerBindDemand.V1", bytes + 16, 1600);
  if (value.evidence != expected ||
      EncodeSblrDdlCreateTriggerRequestV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "TVQX evidence or canonical bytes are invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerDescriptorV1(
    const SblrDdlCreateTriggerDescriptorV1& value, bool operand_magic) {
  if (!ValidDefinition(value.timing, value.event, value.scope,
                       value.target_kind, value.body_profile, value.position,
                       value.security_mode, value.image_mode,
                       value.execution_mode, value.enabled_state,
                       value.recursion_mode) ||
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

  auto out = Header(operand_magic ? "TVDO" : "TVDX",
                    kSblrDdlCreateTriggerDescriptorV1Bytes);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.trigger_occurrence, 4);
  PutLe(&out,
        DefinitionFlags(value.timing, value.event, value.scope,
                        value.target_kind, value.body_profile, value.position,
                        value.security_mode, value.image_mode,
                        value.execution_mode, value.enabled_state,
                        value.recursion_mode),
        4);
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
      "ScratchBird.SblrDdlCreateTriggerDescriptor.V1", out.data() + 16, 400);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), 32, 0);
  return out.size() == kSblrDdlCreateTriggerDescriptorV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlCreateTriggerDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlCreateTriggerDescriptorV1* out, std::string* detail,
    bool operand_magic) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, operand_magic ? "TVDO" : "TVDX",
                   kSblrDdlCreateTriggerDescriptorV1Bytes) ||
      !AllZero(bytes + 408, bytes + 416) ||
      !AllZero(bytes + 456, bytes + 488)) {
    if (detail != nullptr) {
      *detail = "TVDX/TVDO header or reserved bytes are invalid";
    }
    return false;
  }
  SblrDdlCreateTriggerDescriptorV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.trigger_occurrence =
      static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  if (!DecodeDefinitionFlags(
          static_cast<std::uint32_t>(GetLe(bytes + 44, 4)), &value.timing,
          &value.event, &value.scope, &value.target_kind, &value.body_profile,
          &value.position, &value.security_mode, &value.image_mode,
          &value.execution_mode, &value.enabled_state,
          &value.recursion_mode)) {
    if (detail != nullptr) *detail = "TVDX/TVDO definition flags are invalid";
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
      "ScratchBird.SblrDdlCreateTriggerDescriptor.V1", bytes + 16, 400);
  if (value.evidence != expected ||
      EncodeSblrDdlCreateTriggerDescriptorV1(value, operand_magic) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) {
      *detail = "TVDX/TVDO authority, evidence, or canonical bytes are invalid";
    }
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDdlCreateTriggerResultV1(
    const SblrDdlCreateTriggerResultV1& value) {
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
  auto out = Header("TVRS", kSblrDdlCreateTriggerResultV1Bytes);
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
      "ScratchBird.SblrDdlCreateTriggerResult.V1", out.data() + 16, 240);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  PutUuid(&out, value.publication_barrier);
  out.insert(out.end(), 8, 0);
  return out.size() == kSblrDdlCreateTriggerResultV1Bytes
             ? out
             : std::vector<std::uint8_t>{};
}

bool DecodeSblrDdlCreateTriggerResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDdlCreateTriggerResultV1* out, std::string* detail) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, "TVRS", kSblrDdlCreateTriggerResultV1Bytes) ||
      !AllZero(bytes + 312, bytes + 320)) {
    if (detail != nullptr) *detail = "TVRS header or reserved bytes are invalid";
    return false;
  }
  SblrDdlCreateTriggerResultV1 value;
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
      "ScratchBird.SblrDdlCreateTriggerResult.V1", bytes + 16, 240);
  if (value.evidence != expected ||
      EncodeSblrDdlCreateTriggerResultV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) {
      *detail = "TVRS authority, evidence, or canonical bytes are invalid";
    }
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace scratchbird::engine::sblr
