#include "sblr_procedure_invoke_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {
void Put(std::vector<std::uint8_t>* out, std::uint64_t value, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) out->push_back(value >> (8 * i));
}
std::uint64_t Get(const std::uint8_t* bytes, std::size_t n) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < n; ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
  return value;
}
template <class T> bool Nonzero(const T& value) {
  return std::any_of(value.begin(), value.end(), [](auto byte) { return byte != 0; });
}
bool UuidV7(const ProcedureInvokeUuid& value) {
  return Nonzero(value) && (value[6] & 0xf0u) == 0x70u &&
         (value[8] & 0xc0u) == 0x80u;
}
std::vector<std::uint8_t> Header(const char* magic, std::size_t bytes) {
  std::vector<std::uint8_t> out(magic, magic + 4);
  Put(&out, 1, 2); Put(&out, bytes, 2); Put(&out, bytes, 4); Put(&out, 0, 4);
  return out;
}
bool ValidHeader(const std::uint8_t* bytes, std::size_t size,
                 const char* magic, std::size_t expected) {
  return bytes && size == expected && std::equal(bytes, bytes + 4, magic) &&
         Get(bytes + 4, 2) == 1 && Get(bytes + 6, 2) == expected &&
         Get(bytes + 8, 4) == expected &&
         std::all_of(bytes + 12, bytes + 16, [](auto byte) { return byte == 0; });
}
ProcedureInvokeSha Evidence(const char* domain, const std::uint8_t* bytes,
                            std::size_t size) {
  std::vector<std::uint8_t> material(domain, domain + std::strlen(domain));
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}
bool ValidUtf8(std::string_view value) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = bytes[index];
    if (lead == 0) return false;
    if (lead < 0x80) { ++index; continue; }
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xe0u) == 0xc0u) {
      continuation = 1; codepoint = lead & 0x1fu;
      if (codepoint < 2) return false;
    } else if ((lead & 0xf0u) == 0xe0u) {
      continuation = 2; codepoint = lead & 0x0fu;
    } else if ((lead & 0xf8u) == 0xf0u) {
      continuation = 3; codepoint = lead & 0x07u;
    } else { return false; }
    if (index + continuation >= value.size()) return false;
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto byte = bytes[index + offset];
      if ((byte & 0xc0u) != 0x80u) return false;
      codepoint = (codepoint << 6) | (byte & 0x3fu);
    }
    if ((continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    index += continuation + 1;
  }
  return !value.empty();
}
bool ValidAtom(const SblrProcedureInvokeNameAtomV2& atom) {
  return atom.raw_utf8.size() >= 1 && atom.raw_utf8.size() <= 256 &&
         atom.raw_utf8.find('.') == std::string::npos && ValidUtf8(atom.raw_utf8);
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrProcedureInvokeRequestV1(
    const SblrProcedureInvokeRequestV1& value) {
  if (!Nonzero(value.receipt) || value.occurrence == 0 ||
      value.invocation_occurrence == 0) return {};
  auto out = Header("PIRQ", 64);
  out.insert(out.end(), value.receipt.begin(), value.receipt.end());
  Put(&out, value.occurrence, 8); Put(&out, value.invocation_occurrence, 4);
  out.insert(out.end(), 20, 0);
  return out;
}
bool DecodeSblrProcedureInvokeRequestV1(const std::uint8_t* bytes,
                                        std::size_t size,
                                        SblrProcedureInvokeRequestV1* out,
                                        std::string* detail) {
  if (!out || !ValidHeader(bytes, size, "PIRQ", 64) ||
      std::any_of(bytes + 44, bytes + 64, [](auto byte) { return byte != 0; })) {
    if (detail) *detail = "PIRQ invalid"; return false;
  }
  SblrProcedureInvokeRequestV1 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = Get(bytes + 32, 8);
  value.invocation_occurrence = Get(bytes + 40, 4);
  if (EncodeSblrProcedureInvokeRequestV1(value).empty()) return false;
  *out = value; return true;
}
std::vector<std::uint8_t> EncodeSblrProcedureInvokeBindRequestV2(
    const SblrProcedureInvokeBindRequestV2& value) {
  if (!Nonzero(value.receipt) || value.occurrence == 0 ||
      value.invocation_occurrence == 0 || value.command_identity != 1 ||
      value.name_atoms.empty() || value.name_atoms.size() > 3 ||
      !std::all_of(value.name_atoms.begin(), value.name_atoms.end(), ValidAtom))
    return {};
  std::size_t total = 84;
  for (const auto& atom : value.name_atoms) total += 4 + atom.raw_utf8.size();
  if (total > 1024) return {};
  std::vector<std::uint8_t> out{'P','I','R','Q'};
  Put(&out, 2, 2); Put(&out, total, 2); Put(&out, total, 4); Put(&out, 0, 4);
  out.insert(out.end(), value.receipt.begin(), value.receipt.end());
  Put(&out, value.occurrence, 8); Put(&out, value.invocation_occurrence, 4);
  Put(&out, value.command_identity, 2); Put(&out, value.name_atoms.size(), 1);
  Put(&out, 0, 1); Put(&out, 0, 2); Put(&out, 0, 2);
  for (const auto& atom : value.name_atoms) {
    Put(&out, atom.raw_utf8.size(), 2); Put(&out, atom.quoted ? 1 : 0, 1);
    Put(&out, 0, 1);
    out.insert(out.end(), atom.raw_utf8.begin(), atom.raw_utf8.end());
  }
  const auto evidence = Evidence("ScratchBird.SblrProcedureInvokeBindRequest.V2",
                                 out.data() + 16, out.size() - 16);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  return out.size() == total ? out : std::vector<std::uint8_t>{};
}
bool DecodeSblrProcedureInvokeBindRequestV2(
    const std::uint8_t* bytes, std::size_t size,
    SblrProcedureInvokeBindRequestV2* out, std::string* detail) {
  const auto refuse = [&](const char* reason) {
    if (detail) *detail = reason;
    return false;
  };
  if (!out || !bytes || size < 89 || size > 1024 ||
      !std::equal(bytes, bytes + 4, "PIRQ") || Get(bytes + 4, 2) != 2 ||
      Get(bytes + 6, 2) != size || Get(bytes + 8, 4) != size ||
      std::any_of(bytes + 12, bytes + 16, [](auto v){return v != 0;}))
    return refuse("PIRQ v2 header invalid");
  SblrProcedureInvokeBindRequestV2 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = Get(bytes + 32, 8);
  value.invocation_occurrence = Get(bytes + 40, 4);
  value.command_identity = static_cast<std::uint16_t>(Get(bytes + 44, 2));
  const auto atom_count = Get(bytes + 46, 1);
  if (value.command_identity != 1 || atom_count < 1 || atom_count > 3 ||
      std::any_of(bytes + 47, bytes + 52, [](auto v){return v != 0;}))
    return refuse("PIRQ v2 fixed shape invalid");
  const std::size_t evidence_offset = size - 32;
  std::size_t offset = 52;
  for (std::size_t index = 0; index < atom_count; ++index) {
    if (offset + 4 > evidence_offset) return refuse("PIRQ v2 atom truncated");
    const auto length = Get(bytes + offset, 2);
    const auto quoted = bytes[offset + 2];
    if (length < 1 || length > 256 || quoted > 1 || bytes[offset + 3] != 0 ||
        offset + 4 + length > evidence_offset)
      return refuse("PIRQ v2 atom invalid");
    SblrProcedureInvokeNameAtomV2 atom;
    atom.raw_utf8.assign(reinterpret_cast<const char*>(bytes + offset + 4),
                         static_cast<std::size_t>(length));
    atom.quoted = quoted == 1;
    if (!ValidAtom(atom)) return refuse("PIRQ v2 atom text invalid");
    value.name_atoms.push_back(std::move(atom));
    offset += 4 + length;
  }
  if (offset != evidence_offset) return refuse("PIRQ v2 trailing bytes invalid");
  std::copy_n(bytes + evidence_offset, 32, value.evidence.begin());
  const auto expected = Evidence("ScratchBird.SblrProcedureInvokeBindRequest.V2",
                                 bytes + 16, evidence_offset - 16);
  if (value.evidence != expected) return refuse("PIRQ v2 evidence invalid");
  const auto canonical = EncodeSblrProcedureInvokeBindRequestV2(value);
  if (canonical.size() != size || !std::equal(canonical.begin(), canonical.end(), bytes))
    return refuse("PIRQ v2 canonical re-encoding differs");
  *out = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSblrProcedureInvokeBindRequestV3(
    const SblrProcedureInvokeBindRequestV3& value) {
  const auto valid_literal = [](std::string_view literal) {
    if (literal.empty() || literal.size() > 20) return false;
    std::size_t index = 0;
    if (literal.front() == '+' || literal.front() == '-') index = 1;
    return index < literal.size() &&
           std::all_of(literal.begin() + index, literal.end(),
                       [](char value) { return value >= '0' && value <= '9'; });
  };
  if (!Nonzero(value.receipt) || value.occurrence == 0 ||
      value.invocation_occurrence == 0 || value.command_identity != 1 ||
      value.name_atoms.empty() || value.name_atoms.size() > 3 ||
      !std::all_of(value.name_atoms.begin(), value.name_atoms.end(), ValidAtom) ||
      value.arguments.size() != 1 || value.arguments.front().ordinal != 1 ||
      value.arguments.front().lexical_kind != 1 ||
      !valid_literal(value.arguments.front().literal_utf8)) {
    return {};
  }
  std::size_t total = 92 + value.arguments.front().literal_utf8.size();
  for (const auto& atom : value.name_atoms) total += 4 + atom.raw_utf8.size();
  if (total > 1024) return {};
  std::vector<std::uint8_t> out{'P', 'I', 'R', 'Q'};
  Put(&out, 3, 2);
  Put(&out, total, 2);
  Put(&out, total, 4);
  Put(&out, 0, 4);
  out.insert(out.end(), value.receipt.begin(), value.receipt.end());
  Put(&out, value.occurrence, 8);
  Put(&out, value.invocation_occurrence, 4);
  Put(&out, value.command_identity, 2);
  Put(&out, value.name_atoms.size(), 1);
  Put(&out, 1, 1);
  Put(&out, 0, 2);
  Put(&out, 0, 2);
  for (const auto& atom : value.name_atoms) {
    Put(&out, atom.raw_utf8.size(), 2);
    Put(&out, atom.quoted ? 1 : 0, 1);
    Put(&out, 0, 1);
    out.insert(out.end(), atom.raw_utf8.begin(), atom.raw_utf8.end());
  }
  const auto& argument = value.arguments.front();
  Put(&out, argument.ordinal, 2);
  Put(&out, argument.lexical_kind, 1);
  Put(&out, 0, 1);
  Put(&out, argument.literal_utf8.size(), 2);
  Put(&out, 0, 2);
  out.insert(out.end(), argument.literal_utf8.begin(),
             argument.literal_utf8.end());
  const auto evidence = Evidence("ScratchBird.SblrProcedureInvokeBindRequest.V3",
                                 out.data() + 16, out.size() - 16);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  return out.size() == total ? out : std::vector<std::uint8_t>{};
}

bool DecodeSblrProcedureInvokeBindRequestV3(
    const std::uint8_t* bytes, std::size_t size,
    SblrProcedureInvokeBindRequestV3* out, std::string* detail) {
  const auto refuse = [&](const char* reason) {
    if (detail) *detail = reason;
    return false;
  };
  if (!out || !bytes || size < 98 || size > 1024 ||
      !std::equal(bytes, bytes + 4, "PIRQ") || Get(bytes + 4, 2) != 3 ||
      Get(bytes + 6, 2) != size || Get(bytes + 8, 4) != size ||
      std::any_of(bytes + 12, bytes + 16,
                  [](auto value) { return value != 0; })) {
    return refuse("PIRQ v3 header invalid");
  }
  SblrProcedureInvokeBindRequestV3 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = Get(bytes + 32, 8);
  value.invocation_occurrence = Get(bytes + 40, 4);
  value.command_identity = static_cast<std::uint16_t>(Get(bytes + 44, 2));
  const auto atom_count = Get(bytes + 46, 1);
  if (atom_count < 1 || atom_count > 3 || bytes[47] != 1 ||
      Get(bytes + 48, 2) != 0 || Get(bytes + 50, 2) != 0) {
    return refuse("PIRQ v3 fixed shape invalid");
  }
  const std::size_t evidence_offset = size - 32;
  std::size_t offset = 52;
  for (std::size_t index = 0; index < atom_count; ++index) {
    if (offset + 4 > evidence_offset) return refuse("PIRQ v3 atom truncated");
    const auto length = Get(bytes + offset, 2);
    const auto quoted = bytes[offset + 2];
    if (length < 1 || length > 256 || quoted > 1 || bytes[offset + 3] != 0 ||
        offset + 4 + length > evidence_offset) {
      return refuse("PIRQ v3 atom invalid");
    }
    SblrProcedureInvokeNameAtomV2 atom;
    atom.raw_utf8.assign(
        reinterpret_cast<const char*>(bytes + offset + 4), length);
    atom.quoted = quoted == 1;
    if (!ValidAtom(atom)) return refuse("PIRQ v3 atom text invalid");
    value.name_atoms.push_back(std::move(atom));
    offset += 4 + length;
  }
  if (offset + 8 > evidence_offset) {
    return refuse("PIRQ v3 argument truncated");
  }
  SblrProcedureInvokeArgumentDemandV3 argument;
  argument.ordinal = static_cast<std::uint16_t>(Get(bytes + offset, 2));
  argument.lexical_kind = bytes[offset + 2];
  const auto literal_size = Get(bytes + offset + 4, 2);
  if (bytes[offset + 3] != 0 || literal_size < 1 || literal_size > 20 ||
      Get(bytes + offset + 6, 2) != 0 ||
      offset + 8 + literal_size != evidence_offset) {
    return refuse("PIRQ v3 argument shape invalid");
  }
  argument.literal_utf8.assign(
      reinterpret_cast<const char*>(bytes + offset + 8), literal_size);
  const auto canonical_probe = [&]() {
    SblrProcedureInvokeBindRequestV3 probe = value;
    probe.arguments.push_back(argument);
    return EncodeSblrProcedureInvokeBindRequestV3(probe);
  }();
  if (argument.ordinal != 1 || argument.lexical_kind != 1 ||
      canonical_probe.empty()) {
    return refuse("PIRQ v3 argument demand invalid");
  }
  value.arguments.push_back(std::move(argument));
  std::copy_n(bytes + evidence_offset, 32, value.evidence.begin());
  const auto expected = Evidence("ScratchBird.SblrProcedureInvokeBindRequest.V3",
                                 bytes + 16, evidence_offset - 16);
  if (value.evidence != expected) return refuse("PIRQ v3 evidence invalid");
  const auto canonical = EncodeSblrProcedureInvokeBindRequestV3(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes)) {
    return refuse("PIRQ v3 canonical re-encoding differs");
  }
  *out = std::move(value);
  return true;
}
std::vector<std::uint8_t> EncodeSblrProcedureInvokeDescriptorV1(
    const SblrProcedureInvokeDescriptorV1& value, bool operand) {
  if (!Nonzero(value.body) || value.availability == 0) return {};
  auto out = Header(operand ? "PIDO" : "PIDD", 488);
  out.insert(out.end(), value.body.begin(), value.body.end());
  const auto evidence = Evidence("ScratchBird.SblrProcedureInvokeDescriptor.V1",
                                 out.data() + 16, 416);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  Put(&out, value.availability, 8); out.insert(out.end(), 16, 0);
  return out;
}
bool DecodeSblrProcedureInvokeDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrProcedureInvokeDescriptorV1* out, std::string* detail, bool operand) {
  if (!out || !ValidHeader(bytes, size, operand ? "PIDO" : "PIDD", 488) ||
      std::any_of(bytes + 472, bytes + 488, [](auto byte) { return byte != 0; })) {
    if (detail) *detail = "PID invalid"; return false;
  }
  SblrProcedureInvokeDescriptorV1 value;
  std::copy_n(bytes + 16, 416, value.body.begin());
  std::copy_n(bytes + 432, 32, value.evidence.begin());
  value.availability = Get(bytes + 464, 8);
  if (EncodeSblrProcedureInvokeDescriptorV1(value, operand).empty()) return false;
  *out = value; return true;
}
bool ValidateSblrProcedureInvokeAuthorityV1(
    const SblrProcedureInvokeAuthorityV1& value, std::string* detail) {
  const bool valid =
      UuidV7(value.invocation_uuid) && value.invocation_generation != 0 &&
      Nonzero(value.owning_transaction_uuid) &&
      value.owning_local_transaction_id != 0 &&
      Nonzero(value.statement_snapshot_uuid) &&
      Nonzero(value.catalog_epoch_uuid) && value.catalog_generation != 0 &&
      Nonzero(value.security_context_uuid) &&
      Nonzero(value.policy_snapshot_uuid) && value.policy_generation != 0 &&
      UuidV7(value.procedure_uuid) && value.procedure_generation != 0 &&
      UuidV7(value.procedure_body_uuid) &&
      value.procedure_body_generation != 0 &&
      Nonzero(value.procedure_body_sha256) &&
      UuidV7(value.procedure_abi_uuid) &&
      value.procedure_abi_generation != 0 &&
      UuidV7(value.argument_vector_uuid) && value.argument_count <= 1 &&
      value.output_parameter_count == 0 && value.invocation_flags == 0 &&
      Nonzero(value.argument_vector_sha256) &&
      UuidV7(value.output_descriptor_vector_uuid) &&
      value.output_descriptor_vector_generation != 0 &&
      UuidV7(value.result_set_shape_uuid) &&
      value.result_set_shape_generation != 0 &&
      Nonzero(value.effect_set_sha256) && UuidV7(value.recovery_uuid) &&
      value.recovery_generation != 0 && Nonzero(value.engine_snapshot_uuid) &&
      value.executor_availability_generation != 0 &&
      value.invocation_uuid != value.procedure_uuid &&
      value.invocation_uuid != value.procedure_body_uuid &&
      value.invocation_uuid != value.recovery_uuid;
  if (!valid && detail != nullptr) *detail = "PIDO authority fields are invalid";
  return valid;
}
bool DecodeSblrProcedureInvokeAuthorityV1(
    const SblrProcedureInvokeDescriptorV1& descriptor,
    SblrProcedureInvokeAuthorityV1* out, std::string* detail) {
  if (out == nullptr) return false;
  SblrProcedureInvokeAuthorityV1 value;
  const auto* body = descriptor.body.data();
  std::copy_n(body + 0, 16, value.invocation_uuid.begin());
  value.invocation_generation = Get(body + 16, 8);
  std::copy_n(body + 24, 16, value.owning_transaction_uuid.begin());
  value.owning_local_transaction_id = Get(body + 40, 8);
  std::copy_n(body + 48, 16, value.statement_snapshot_uuid.begin());
  std::copy_n(body + 64, 16, value.catalog_epoch_uuid.begin());
  value.catalog_generation = Get(body + 80, 8);
  std::copy_n(body + 88, 16, value.security_context_uuid.begin());
  std::copy_n(body + 104, 16, value.policy_snapshot_uuid.begin());
  value.policy_generation = Get(body + 120, 8);
  std::copy_n(body + 128, 16, value.procedure_uuid.begin());
  value.procedure_generation = Get(body + 144, 8);
  std::copy_n(body + 152, 16, value.procedure_body_uuid.begin());
  value.procedure_body_generation = Get(body + 168, 8);
  std::copy_n(body + 176, 32, value.procedure_body_sha256.begin());
  std::copy_n(body + 208, 16, value.procedure_abi_uuid.begin());
  value.procedure_abi_generation = Get(body + 224, 8);
  std::copy_n(body + 232, 16, value.argument_vector_uuid.begin());
  value.argument_count = static_cast<std::uint32_t>(Get(body + 248, 4));
  value.output_parameter_count =
      static_cast<std::uint32_t>(Get(body + 252, 4));
  value.invocation_flags = static_cast<std::uint32_t>(Get(body + 256, 4));
  if (Get(body + 260, 4) != 0) {
    if (detail != nullptr) *detail = "PIDO reserved field is nonzero";
    return false;
  }
  std::copy_n(body + 264, 32, value.argument_vector_sha256.begin());
  std::copy_n(body + 296, 16, value.output_descriptor_vector_uuid.begin());
  value.output_descriptor_vector_generation = Get(body + 312, 8);
  std::copy_n(body + 320, 16, value.result_set_shape_uuid.begin());
  value.result_set_shape_generation = Get(body + 336, 8);
  std::copy_n(body + 344, 32, value.effect_set_sha256.begin());
  std::copy_n(body + 376, 16, value.recovery_uuid.begin());
  value.recovery_generation = Get(body + 392, 8);
  std::copy_n(body + 400, 16, value.engine_snapshot_uuid.begin());
  value.executor_availability_generation = descriptor.availability;
  if (!ValidateSblrProcedureInvokeAuthorityV1(value, detail)) return false;
  *out = value;
  return true;
}
std::vector<std::uint8_t> EncodeSblrProcedureInvokeResultV1(
    const SblrProcedureInvokeResultV1& value) {
  if (!Nonzero(value.body) || value.body[24] < 1 || value.body[24] > 2 ||
      value.body[25] > 2 || value.availability == 0 || !Nonzero(value.barrier)) return {};
  auto out = Header("PIRS", 320);
  out.insert(out.end(), value.body.begin(), value.body.end());
  const auto evidence = Evidence("ScratchBird.SblrProcedureInvokeExecutorEvidence.V1",
                                 out.data() + 16, 240);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  Put(&out, value.availability, 8);
  out.insert(out.end(), value.barrier.begin(), value.barrier.end());
  out.insert(out.end(), 8, 0);
  return out;
}
bool DecodeSblrProcedureInvokeResultV1(const std::uint8_t* bytes,
                                       std::size_t size,
                                       SblrProcedureInvokeResultV1* out,
                                       std::string* detail) {
  if (!out || !ValidHeader(bytes, size, "PIRS", 320) ||
      std::any_of(bytes + 312, bytes + 320, [](auto byte) { return byte != 0; })) {
    if (detail) *detail = "PIRS invalid"; return false;
  }
  SblrProcedureInvokeResultV1 value;
  std::copy_n(bytes + 16, 240, value.body.begin());
  std::copy_n(bytes + 256, 32, value.evidence.begin());
  value.availability = Get(bytes + 288, 8);
  std::copy_n(bytes + 296, 16, value.barrier.begin());
  if (EncodeSblrProcedureInvokeResultV1(value).empty()) return false;
  *out = value; return true;
}
}  // namespace scratchbird::engine::sblr
