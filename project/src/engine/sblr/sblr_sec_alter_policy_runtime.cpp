#include "sblr_sec_alter_policy_runtime.hpp"

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

SecAlterPolicySha Evidence(std::string_view domain, const std::uint8_t* bytes,
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

void PutUuid(std::vector<std::uint8_t>* out, const SecAlterPolicyUuid& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutSha(std::vector<std::uint8_t>* out, const SecAlterPolicySha& sha) {
  out->insert(out->end(), sha.begin(), sha.end());
}

void GetUuid(const std::uint8_t* bytes, SecAlterPolicyUuid* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

void GetSha(const std::uint8_t* bytes, SecAlterPolicySha* out) {
  std::copy_n(bytes, out->size(), out->begin());
}

bool DistinctDescriptorIdentities(
    const SblrSecAlterPolicyDescriptorV1& value) {
  return value.policy_uuid != value.binding_uuid &&
         value.policy_uuid != value.recovery_uuid &&
         value.binding_uuid != value.recovery_uuid;
}

bool DistinctResultIdentities(const SblrSecAlterPolicyResultV1& value) {
  return value.policy_uuid != value.mutation_uuid &&
         value.policy_uuid != value.publication_barrier_uuid &&
         value.policy_uuid != value.recovery_uuid &&
         value.mutation_uuid != value.publication_barrier_uuid &&
         value.mutation_uuid != value.recovery_uuid &&
         value.publication_barrier_uuid != value.recovery_uuid;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrSecAlterPolicyRequestV1(
    const SblrSecAlterPolicyRequestV1& value) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.policy_occurrence == 0 || value.command_identity != 1 ||
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

  auto out = Header("SAPQ", kRequestSize);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.policy_occurrence, 4);
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
      "ScratchBird.SblrSecAlterPolicyBindDemand.V1", out.data() + 16, 816);
  if (NonZero(value.evidence) && value.evidence != evidence) return {};
  PutSha(&out, evidence);
  out.insert(out.end(), 32, 0);
  return out.size() == kRequestSize ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrSecAlterPolicyRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecAlterPolicyRequestV1* out, std::string* detail) {
  if (out == nullptr || !ValidHeader(bytes, size, "SAPQ", kRequestSize) ||
      !AllZero(bytes + 54, bytes + 64) ||
      !AllZero(bytes + 864, bytes + 896)) {
    if (detail != nullptr) *detail = "SAPQ header or reserved bytes are invalid";
    return false;
  }
  SblrSecAlterPolicyRequestV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.policy_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.command_identity = static_cast<std::uint16_t>(GetLe(bytes + 44, 2));
  const auto count = bytes[46];
  const auto quoted_mask = bytes[47];
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.policy_occurrence == 0 || value.command_identity != 1 ||
      count == 0 || count > 3 ||
      (quoted_mask & ~((1u << count) - 1u)) != 0) {
    if (detail != nullptr) *detail = "SAPQ authority or name shape is invalid";
    return false;
  }
  for (std::size_t index = 0; index < 3; ++index) {
    const auto length = static_cast<std::size_t>(
        GetLe(bytes + 48 + index * 2, 2));
    const auto* slot = bytes + 64 + index * 256;
    if ((index >= count && length != 0) || length > 256 ||
        !AllZero(slot + length, slot + 256)) {
      if (detail != nullptr) *detail = "SAPQ name atom extent is invalid";
      return false;
    }
    if (index < count) {
      if (length == 0) {
        if (detail != nullptr) *detail = "SAPQ name atom is empty";
        return false;
      }
      SblrSecAlterPolicyNameAtomV1 atom;
      atom.raw_utf8.assign(reinterpret_cast<const char*>(slot), length);
      atom.quoted = (quoted_mask & (1u << index)) != 0;
      if (!ValidUtf8(atom.raw_utf8)) {
        if (detail != nullptr) *detail = "SAPQ name atom UTF-8 is invalid";
        return false;
      }
      value.name_atoms.push_back(std::move(atom));
    }
  }
  GetSha(bytes + 832, &value.evidence);
  const auto expected = Evidence(
      "ScratchBird.SblrSecAlterPolicyBindDemand.V1", bytes + 16, 816);
  if (value.evidence != expected ||
      EncodeSblrSecAlterPolicyRequestV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "SAPQ evidence or canonical encoding is invalid";
    return false;
  }
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrSecAlterPolicyDescriptorV1(
    const SblrSecAlterPolicyDescriptorV1& value, bool operand_magic) {
  if (!NonZero(value.receipt) || value.occurrence == 0 ||
      value.policy_occurrence == 0 || value.action != 1 || value.flags != 0 ||
      !NonZero(value.policy_uuid) || value.expected_generation == 0 ||
      !NonZero(value.database_uuid) ||
      !NonZero(value.owning_transaction_uuid) ||
      value.owning_local_transaction_id == 0 ||
      !NonZero(value.statement_snapshot_uuid) ||
      !NonZero(value.catalog_epoch_uuid) || value.catalog_generation == 0 ||
      !NonZero(value.security_context_uuid) || value.security_generation == 0 ||
      !NonZero(value.policy_snapshot_uuid) || value.policy_generation == 0 ||
      !NonZero(value.resource_grant_uuid) || value.resource_generation == 0 ||
      !NonZero(value.principal_uuid) || !NonZero(value.binding_uuid) ||
      !NonZero(value.recovery_uuid) || value.binding_generation == 0 ||
      value.recovery_generation == 0 ||
      !NonZero(value.normalized_path_sha256) ||
      !NonZero(value.syntax_demand_sha256) ||
      !NonZero(value.authorization_evidence_sha256) ||
      !NonZero(value.frozen_policy_record_sha256) || value.availability == 0 ||
      !DistinctDescriptorIdentities(value)) {
    return {};
  }
  auto out = Header(operand_magic ? "SAPO" : "SAPD", kDescriptorSize);
  PutUuid(&out, value.receipt);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.policy_occurrence, 4);
  PutLe(&out, value.action, 2);
  PutLe(&out, value.flags, 2);
  PutUuid(&out, value.policy_uuid);
  PutLe(&out, value.expected_generation, 8);
  PutUuid(&out, value.database_uuid);
  PutUuid(&out, value.owning_transaction_uuid);
  PutLe(&out, value.owning_local_transaction_id, 8);
  PutUuid(&out, value.statement_snapshot_uuid);
  PutUuid(&out, value.catalog_epoch_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutUuid(&out, value.security_context_uuid);
  PutLe(&out, value.security_generation, 8);
  PutUuid(&out, value.policy_snapshot_uuid);
  PutLe(&out, value.policy_generation, 8);
  PutUuid(&out, value.resource_grant_uuid);
  PutLe(&out, value.resource_generation, 8);
  PutUuid(&out, value.principal_uuid);
  PutUuid(&out, value.binding_uuid);
  PutUuid(&out, value.recovery_uuid);
  PutLe(&out, value.binding_generation, 8);
  PutLe(&out, value.recovery_generation, 8);
  PutSha(&out, value.normalized_path_sha256);
  PutSha(&out, value.syntax_demand_sha256);
  PutSha(&out, value.authorization_evidence_sha256);
  PutSha(&out, value.frozen_policy_record_sha256);
  const auto evidence = Evidence(
      "ScratchBird.SblrSecAlterPolicyDescriptor.V1", out.data() + 16, 400);
  if (NonZero(value.descriptor_evidence) &&
      value.descriptor_evidence != evidence) {
    return {};
  }
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  out.insert(out.end(), 32, 0);
  return out.size() == kDescriptorSize ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrSecAlterPolicyDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecAlterPolicyDescriptorV1* out, std::string* detail,
    bool operand_magic) {
  if (out == nullptr ||
      !ValidHeader(bytes, size, operand_magic ? "SAPO" : "SAPD",
                   kDescriptorSize) ||
      !AllZero(bytes + 456, bytes + 488)) {
    if (detail != nullptr) *detail = "SAPD/SAPO header or reserved bytes are invalid";
    return false;
  }
  SblrSecAlterPolicyDescriptorV1 value;
  GetUuid(bytes + 16, &value.receipt);
  value.occurrence = GetLe(bytes + 32, 8);
  value.policy_occurrence = static_cast<std::uint32_t>(GetLe(bytes + 40, 4));
  value.action = static_cast<std::uint16_t>(GetLe(bytes + 44, 2));
  value.flags = static_cast<std::uint16_t>(GetLe(bytes + 46, 2));
  GetUuid(bytes + 48, &value.policy_uuid);
  value.expected_generation = GetLe(bytes + 64, 8);
  GetUuid(bytes + 72, &value.database_uuid);
  GetUuid(bytes + 88, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 104, 8);
  GetUuid(bytes + 112, &value.statement_snapshot_uuid);
  GetUuid(bytes + 128, &value.catalog_epoch_uuid);
  value.catalog_generation = GetLe(bytes + 144, 8);
  GetUuid(bytes + 152, &value.security_context_uuid);
  value.security_generation = GetLe(bytes + 168, 8);
  GetUuid(bytes + 176, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(bytes + 192, 8);
  GetUuid(bytes + 200, &value.resource_grant_uuid);
  value.resource_generation = GetLe(bytes + 216, 8);
  GetUuid(bytes + 224, &value.principal_uuid);
  GetUuid(bytes + 240, &value.binding_uuid);
  GetUuid(bytes + 256, &value.recovery_uuid);
  value.binding_generation = GetLe(bytes + 272, 8);
  value.recovery_generation = GetLe(bytes + 280, 8);
  GetSha(bytes + 288, &value.normalized_path_sha256);
  GetSha(bytes + 320, &value.syntax_demand_sha256);
  GetSha(bytes + 352, &value.authorization_evidence_sha256);
  GetSha(bytes + 384, &value.frozen_policy_record_sha256);
  GetSha(bytes + 416, &value.descriptor_evidence);
  value.availability = GetLe(bytes + 448, 8);
  const auto expected = Evidence(
      "ScratchBird.SblrSecAlterPolicyDescriptor.V1", bytes + 16, 400);
  if (value.descriptor_evidence != expected ||
      EncodeSblrSecAlterPolicyDescriptorV1(value, operand_magic) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "SAPD/SAPO authority or evidence is invalid";
    return false;
  }
  *out = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrSecAlterPolicyResultV1(
    const SblrSecAlterPolicyResultV1& value) {
  if (!NonZero(value.receipt) || !NonZero(value.policy_uuid) ||
      value.generation == 0 || value.previous_generation == 0 ||
      value.generation != value.previous_generation + 1 ||
      !NonZero(value.database_uuid) ||
      !NonZero(value.owning_transaction_uuid) ||
      value.owning_local_transaction_id == 0 ||
      !NonZero(value.statement_snapshot_uuid) || !NonZero(value.mutation_uuid) ||
      value.cache_invalidation_generation == 0 ||
      value.security_generation == 0 || value.action != 1 ||
      value.status != 1 || value.publication_barrier != 1 ||
      !NonZero(value.descriptor_evidence) || !NonZero(value.effect_evidence) ||
      value.availability == 0 || !NonZero(value.publication_barrier_uuid) ||
      !NonZero(value.recovery_uuid) || !DistinctResultIdentities(value)) {
    return {};
  }
  auto out = Header("SAPR", kResultSize);
  PutUuid(&out, value.receipt);
  PutUuid(&out, value.policy_uuid);
  PutLe(&out, value.generation, 8);
  PutLe(&out, value.previous_generation, 8);
  PutUuid(&out, value.database_uuid);
  PutUuid(&out, value.owning_transaction_uuid);
  PutLe(&out, value.owning_local_transaction_id, 8);
  PutUuid(&out, value.statement_snapshot_uuid);
  PutUuid(&out, value.mutation_uuid);
  PutLe(&out, value.cache_invalidation_generation, 8);
  PutLe(&out, value.security_generation, 8);
  PutLe(&out, value.action, 2);
  out.push_back(value.status);
  out.push_back(value.publication_barrier);
  out.insert(out.end(), 4, 0);
  PutSha(&out, value.descriptor_evidence);
  PutSha(&out, value.effect_evidence);
  const auto evidence = Evidence(
      "ScratchBird.SblrSecAlterPolicyResult.V1", out.data() + 16, 208);
  if (NonZero(value.result_evidence) && value.result_evidence != evidence) {
    return {};
  }
  PutSha(&out, evidence);
  PutLe(&out, value.availability, 8);
  PutUuid(&out, value.publication_barrier_uuid);
  PutUuid(&out, value.recovery_uuid);
  out.insert(out.end(), 24, 0);
  return out.size() == kResultSize ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrSecAlterPolicyResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrSecAlterPolicyResultV1* out, std::string* detail) {
  if (out == nullptr || !ValidHeader(bytes, size, "SAPR", kResultSize) ||
      !AllZero(bytes + 156, bytes + 160) ||
      !AllZero(bytes + 296, bytes + 320)) {
    if (detail != nullptr) *detail = "SAPR header or reserved bytes are invalid";
    return false;
  }
  SblrSecAlterPolicyResultV1 value;
  GetUuid(bytes + 16, &value.receipt);
  GetUuid(bytes + 32, &value.policy_uuid);
  value.generation = GetLe(bytes + 48, 8);
  value.previous_generation = GetLe(bytes + 56, 8);
  GetUuid(bytes + 64, &value.database_uuid);
  GetUuid(bytes + 80, &value.owning_transaction_uuid);
  value.owning_local_transaction_id = GetLe(bytes + 96, 8);
  GetUuid(bytes + 104, &value.statement_snapshot_uuid);
  GetUuid(bytes + 120, &value.mutation_uuid);
  value.cache_invalidation_generation = GetLe(bytes + 136, 8);
  value.security_generation = GetLe(bytes + 144, 8);
  value.action = static_cast<std::uint16_t>(GetLe(bytes + 152, 2));
  value.status = bytes[154];
  value.publication_barrier = bytes[155];
  GetSha(bytes + 160, &value.descriptor_evidence);
  GetSha(bytes + 192, &value.effect_evidence);
  GetSha(bytes + 224, &value.result_evidence);
  value.availability = GetLe(bytes + 256, 8);
  GetUuid(bytes + 264, &value.publication_barrier_uuid);
  GetUuid(bytes + 280, &value.recovery_uuid);
  const auto expected = Evidence(
      "ScratchBird.SblrSecAlterPolicyResult.V1", bytes + 16, 208);
  if (value.result_evidence != expected ||
      EncodeSblrSecAlterPolicyResultV1(value) !=
          std::vector<std::uint8_t>(bytes, bytes + size)) {
    if (detail != nullptr) *detail = "SAPR authority or evidence is invalid";
    return false;
  }
  *out = value;
  return true;
}

}  // namespace scratchbird::engine::sblr
