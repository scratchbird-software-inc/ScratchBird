#include "sbps_bulk_import_stream_codec.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace scratchbird::wire::sbps_bulk_import {
namespace {

constexpr std::uint64_t kMaximumStreamBytes = 17'179'869'184ULL;
constexpr std::uint64_t kMaximumChunkCount = 262'144ULL;
constexpr std::size_t kMaximumChunkBytes = 8'388'608U;

using Fields = std::vector<std::vector<std::uint8_t>>;

bool Fail(std::string* detail, const char* reason) {
  if (detail != nullptr) {
    *detail = reason;
  }
  return false;
}

template <typename T>
bool IsNonZero(const T& value) {
  return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) {
    return byte != 0;
  });
}

Hash256 Hash(const std::vector<std::uint8_t>& bytes) {
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

void AppendDomain(std::vector<std::uint8_t>* bytes, const char* domain) {
  bytes->insert(bytes->end(), domain, domain + std::strlen(domain));
}

void AppendU16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8U));
}

void AppendU32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendU64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendField(std::vector<std::uint8_t>* bytes,
                 std::uint16_t field_id,
                 const std::uint8_t* value,
                 std::size_t value_size) {
  AppendU16(bytes, field_id);
  AppendU32(bytes, static_cast<std::uint32_t>(value_size));
  bytes->insert(bytes->end(), value, value + value_size);
}

template <typename T>
void AppendArrayField(std::vector<std::uint8_t>* bytes,
                      std::uint16_t field_id,
                      const T& value) {
  AppendField(bytes, field_id, value.data(), value.size());
}

void AppendU8Field(std::vector<std::uint8_t>* bytes,
                   std::uint16_t field_id,
                   std::uint8_t value) {
  AppendField(bytes, field_id, &value, 1);
}

void AppendU32Field(std::vector<std::uint8_t>* bytes,
                    std::uint16_t field_id,
                    std::uint32_t value) {
  std::uint8_t encoded[4];
  for (unsigned i = 0; i != 4; ++i) {
    encoded[i] = static_cast<std::uint8_t>(value >> (8U * i));
  }
  AppendField(bytes, field_id, encoded, sizeof(encoded));
}

void AppendU64Field(std::vector<std::uint8_t>* bytes,
                    std::uint16_t field_id,
                    std::uint64_t value) {
  std::uint8_t encoded[8];
  for (unsigned i = 0; i != 8; ++i) {
    encoded[i] = static_cast<std::uint8_t>(value >> (8U * i));
  }
  AppendField(bytes, field_id, encoded, sizeof(encoded));
}

bool ParseExactTlv(const std::uint8_t* data,
                   std::size_t size,
                   std::size_t expected_fields,
                   Fields* fields,
                   std::string* detail) {
  if (fields == nullptr) {
    return Fail(detail, "bulk_import.tlv.output_missing");
  }
  fields->clear();
  if (data == nullptr || size < 2 || data[0] != 1 || data[1] != 0) {
    return Fail(detail, "bulk_import.tlv.revision_invalid");
  }

  std::size_t offset = 2;
  while (offset < size) {
    if (fields->size() >= expected_fields || size - offset < 6) {
      return Fail(detail, "bulk_import.tlv.field_header_invalid");
    }
    const auto field_id = static_cast<std::uint16_t>(data[offset]) |
                          (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
    const auto field_size = static_cast<std::uint32_t>(data[offset + 2]) |
                            (static_cast<std::uint32_t>(data[offset + 3]) << 8U) |
                            (static_cast<std::uint32_t>(data[offset + 4]) << 16U) |
                            (static_cast<std::uint32_t>(data[offset + 5]) << 24U);
    const auto expected_id = static_cast<std::uint16_t>(fields->size() + 1U);
    if (field_id != expected_id) {
      return Fail(detail, "bulk_import.tlv.field_identity_invalid");
    }
    offset += 6;
    if (field_size > size - offset) {
      return Fail(detail, "bulk_import.tlv.field_extent_invalid");
    }
    fields->emplace_back(data + offset, data + offset + field_size);
    offset += field_size;
  }
  if (offset != size || fields->size() != expected_fields) {
    return Fail(detail, "bulk_import.tlv.field_count_invalid");
  }
  return true;
}

template <typename T>
bool CopyArray(const std::vector<std::uint8_t>& bytes, T* value) {
  if (value == nullptr || bytes.size() != value->size()) {
    return false;
  }
  std::copy(bytes.begin(), bytes.end(), value->begin());
  return true;
}

bool ReadU8(const std::vector<std::uint8_t>& bytes, std::uint8_t* value) {
  if (value == nullptr || bytes.size() != 1) {
    return false;
  }
  *value = bytes[0];
  return true;
}

bool ReadU32(const std::vector<std::uint8_t>& bytes, std::uint32_t* value) {
  if (value == nullptr || bytes.size() != 4) {
    return false;
  }
  *value = 0;
  for (unsigned i = 0; i != 4; ++i) {
    *value |= static_cast<std::uint32_t>(bytes[i]) << (8U * i);
  }
  return true;
}

bool ReadU64(const std::vector<std::uint8_t>& bytes, std::uint64_t* value) {
  if (value == nullptr || bytes.size() != 8) {
    return false;
  }
  *value = 0;
  for (unsigned i = 0; i != 8; ++i) {
    *value |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
  }
  return true;
}

bool IsValidUtf8Atom(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0 || size > 256) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < size) {
    const auto lead = data[offset++];
    if (lead == 0) {
      return false;
    }
    if (lead < 0x80) {
      continue;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
      if (offset >= size || (data[offset] & 0xc0U) != 0x80U) {
        return false;
      }
      ++offset;
      continue;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
      if (offset + 1 >= size ||
          (data[offset] & 0xc0U) != 0x80U ||
          (data[offset + 1] & 0xc0U) != 0x80U ||
          (lead == 0xe0 && data[offset] < 0xa0) ||
          (lead == 0xed && data[offset] >= 0xa0)) {
        return false;
      }
      offset += 2;
      continue;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
      if (offset + 2 >= size ||
          (data[offset] & 0xc0U) != 0x80U ||
          (data[offset + 1] & 0xc0U) != 0x80U ||
          (data[offset + 2] & 0xc0U) != 0x80U ||
          (lead == 0xf0 && data[offset] < 0x90) ||
          (lead == 0xf4 && data[offset] >= 0x90)) {
        return false;
      }
      offset += 3;
      continue;
    }
    return false;
  }
  return true;
}

bool DecodeBindTargetNameAtomsStrict(
    const std::vector<std::uint8_t>& value,
    std::vector<BindTargetNameAtom>* atoms) {
  if (atoms == nullptr) {
    return false;
  }
  atoms->clear();
  if (value.empty() || value[0] < 1 || value[0] > 3) {
    return false;
  }
  atoms->reserve(value[0]);
  std::size_t offset = 1;
  for (std::uint8_t atom = 0; atom < value[0]; ++atom) {
    if (value.size() - offset < 2) {
      atoms->clear();
      return false;
    }
    const auto atom_size = static_cast<std::size_t>(value[offset]) |
                           (static_cast<std::size_t>(value[offset + 1]) << 8U);
    offset += 2;
    if (atom_size == 0 || atom_size > 256 || atom_size > value.size() - offset) {
      atoms->clear();
      return false;
    }
    if (!IsValidUtf8Atom(value.data() + offset, atom_size)) {
      atoms->clear();
      return false;
    }
    BindTargetNameAtom decoded;
    decoded.raw_utf8.assign(
        reinterpret_cast<const char*>(value.data() + offset), atom_size);
    offset += atom_size;
    if (offset >= value.size() || value[offset] > 1) {
      atoms->clear();
      return false;
    }
    decoded.quoted = value[offset] == 1;
    atoms->push_back(std::move(decoded));
    ++offset;
  }
  if (offset != value.size()) {
    atoms->clear();
    return false;
  }
  return true;
}

bool EncodeBindTargetNameAtomsStrict(
    const std::vector<BindTargetNameAtom>& atoms,
    std::vector<std::uint8_t>* value) {
  if (value == nullptr) {
    return false;
  }
  value->clear();
  if (atoms.empty() || atoms.size() > 3) {
    return false;
  }
  value->push_back(static_cast<std::uint8_t>(atoms.size()));
  for (const auto& atom : atoms) {
    if (atom.raw_utf8.empty() || atom.raw_utf8.size() > 256 ||
        !IsValidUtf8Atom(
            reinterpret_cast<const std::uint8_t*>(atom.raw_utf8.data()),
            atom.raw_utf8.size())) {
      value->clear();
      return false;
    }
    const auto size = static_cast<std::uint16_t>(atom.raw_utf8.size());
    value->push_back(static_cast<std::uint8_t>(size));
    value->push_back(static_cast<std::uint8_t>(size >> 8U));
    value->insert(value->end(), atom.raw_utf8.begin(), atom.raw_utf8.end());
    value->push_back(atom.quoted ? 1U : 0U);
  }
  std::vector<BindTargetNameAtom> decoded;
  if (!DecodeBindTargetNameAtomsStrict(*value, &decoded) ||
      decoded.size() != atoms.size()) {
    value->clear();
    return false;
  }
  for (std::size_t index = 0; index < atoms.size(); ++index) {
    if (decoded[index].raw_utf8 != atoms[index].raw_utf8 ||
        decoded[index].quoted != atoms[index].quoted) {
      value->clear();
      return false;
    }
  }
  return true;
}

bool IsAdmittedCommandSurface(const std::string& command_surface_id) {
  return command_surface_id == "SBSQL-465931ED7427" ||
         command_surface_id == "SBSQL-4F912014EA85";
}

bool CheckedStreamExtent(std::uint64_t offset, std::size_t payload_size) {
  if (offset > kMaximumStreamBytes ||
      payload_size > kMaximumStreamBytes - offset) {
    return false;
  }
  return offset + payload_size <= kMaximumStreamBytes;
}

bool IsValidChunk(const Chunk& value) {
  if (!IsNonZero(value.authenticated_receipt_uuid) ||
      !IsNonZero(value.stream_uuid) || value.stream_generation == 0 ||
      value.structural_occurrence == 0 || value.import_occurrence == 0 ||
      !IsNonZero(value.descriptor_evidence) || value.chunk_sequence == 0 ||
      value.chunk_sequence > kMaximumChunkCount ||
      value.chunk_payload.empty() || value.chunk_payload.size() > kMaximumChunkBytes ||
      !IsNonZero(value.previous_chain_sha256) ||
      !CheckedStreamExtent(value.byte_offset, value.chunk_payload.size()) ||
      !ValidateChunkEvidence(value)) {
    return false;
  }
  if (value.chunk_sequence == 1) {
    return value.byte_offset == 0 &&
           value.previous_chain_sha256 ==
               ChainStart(value.stream_uuid, value.stream_generation,
                          value.descriptor_evidence);
  }
  return value.byte_offset != 0;
}

bool IsValidChunkAck(const ChunkAck& value) {
  return IsNonZero(value.stream_uuid) && value.stream_generation != 0 &&
         value.accepted_sequence != 0 &&
         value.accepted_sequence <= kMaximumChunkCount &&
         value.accepted_total_bytes != 0 &&
         value.accepted_total_bytes <= kMaximumStreamBytes &&
         IsNonZero(value.accepted_chain_sha256) &&
         value.durable_spool_generation != 0 &&
         ValidateChunkAckEvidence(value);
}

bool IsValidSeal(const Seal& value) {
  return IsNonZero(value.authenticated_receipt_uuid) &&
         IsNonZero(value.stream_uuid) && value.stream_generation != 0 &&
         IsNonZero(value.descriptor_evidence) &&
         value.final_chunk_count != 0 &&
         value.final_chunk_count <= kMaximumChunkCount &&
         value.total_stream_bytes != 0 &&
         value.total_stream_bytes <= kMaximumStreamBytes &&
         IsNonZero(value.final_chain_sha256) &&
         IsNonZero(value.content_sha256) && ValidateSealEvidence(value);
}

bool IsValidSealAck(const SealAck& value) {
  return IsNonZero(value.stream_uuid) && value.stream_generation != 0 &&
         IsNonZero(value.durable_spool_uuid) &&
         value.durable_spool_generation != 0 && value.chunk_count != 0 &&
         value.chunk_count <= kMaximumChunkCount &&
         value.total_stream_bytes != 0 &&
         value.total_stream_bytes <= kMaximumStreamBytes &&
         IsNonZero(value.final_chain_sha256) &&
         IsNonZero(value.content_sha256) && ValidateSealAckEvidence(value);
}

bool IsValidBind(const Bind& value) {
  std::vector<BindTargetNameAtom> target_name_atoms;
  return ValidateBindEvidence(value) &&
         DecodeBindTargetNameAtomsStrict(value.target_name_atom_vector,
                                         &target_name_atoms) &&
         value.input_format_demand == 1 &&
         value.character_encoding_demand == 1 &&
         value.conversion_policy_demand == 1 &&
         value.null_default_policy_demand == 1 &&
         value.reject_policy_demand == 1 && value.maximum_rejected_rows == 0;
}

bool IsValidBindAck(const BindAck& value) {
  return ValidateBindAckEvidence(value);
}

void BeginTlv(std::vector<std::uint8_t>* bytes) {
  bytes->clear();
  bytes->push_back(1);
  bytes->push_back(0);
}

void EncodeChunkCanonical(const Chunk& value, std::vector<std::uint8_t>* bytes) {
  BeginTlv(bytes);
  AppendArrayField(bytes, 1, value.authenticated_receipt_uuid);
  AppendArrayField(bytes, 2, value.stream_uuid);
  AppendU64Field(bytes, 3, value.stream_generation);
  AppendU64Field(bytes, 4, value.structural_occurrence);
  AppendU32Field(bytes, 5, value.import_occurrence);
  AppendArrayField(bytes, 6, value.descriptor_evidence);
  AppendU64Field(bytes, 7, value.chunk_sequence);
  AppendU64Field(bytes, 8, value.byte_offset);
  AppendArrayField(bytes, 9, value.previous_chain_sha256);
  AppendArrayField(bytes, 10, value.payload_sha256);
  AppendField(bytes, 11, value.chunk_payload.data(), value.chunk_payload.size());
  AppendArrayField(bytes, 12, value.chunk_chain_sha256);
}

void EncodeChunkAckCanonical(const ChunkAck& value,
                             std::vector<std::uint8_t>* bytes) {
  BeginTlv(bytes);
  AppendArrayField(bytes, 1, value.stream_uuid);
  AppendU64Field(bytes, 2, value.stream_generation);
  AppendU64Field(bytes, 3, value.accepted_sequence);
  AppendU64Field(bytes, 4, value.accepted_total_bytes);
  AppendArrayField(bytes, 5, value.accepted_chain_sha256);
  AppendU64Field(bytes, 6, value.durable_spool_generation);
  AppendArrayField(bytes, 7, value.ack_evidence_sha256);
}

void EncodeSealCanonical(const Seal& value, std::vector<std::uint8_t>* bytes) {
  BeginTlv(bytes);
  AppendArrayField(bytes, 1, value.authenticated_receipt_uuid);
  AppendArrayField(bytes, 2, value.stream_uuid);
  AppendU64Field(bytes, 3, value.stream_generation);
  AppendArrayField(bytes, 4, value.descriptor_evidence);
  AppendU64Field(bytes, 5, value.final_chunk_count);
  AppendU64Field(bytes, 6, value.total_stream_bytes);
  AppendArrayField(bytes, 7, value.final_chain_sha256);
  AppendArrayField(bytes, 8, value.content_sha256);
  AppendArrayField(bytes, 9, value.seal_request_evidence_sha256);
}

void EncodeSealAckCanonical(const SealAck& value,
                            std::vector<std::uint8_t>* bytes) {
  BeginTlv(bytes);
  AppendArrayField(bytes, 1, value.stream_uuid);
  AppendU64Field(bytes, 2, value.stream_generation);
  AppendArrayField(bytes, 3, value.durable_spool_uuid);
  AppendU64Field(bytes, 4, value.durable_spool_generation);
  AppendU64Field(bytes, 5, value.chunk_count);
  AppendU64Field(bytes, 6, value.total_stream_bytes);
  AppendArrayField(bytes, 7, value.final_chain_sha256);
  AppendArrayField(bytes, 8, value.content_sha256);
  AppendArrayField(bytes, 9, value.seal_evidence_sha256);
}

void EncodeBindCanonical(const Bind& value, std::vector<std::uint8_t>* bytes) {
  BeginTlv(bytes);
  AppendArrayField(bytes, 1, value.authenticated_receipt_uuid);
  AppendField(bytes, 2,
              reinterpret_cast<const std::uint8_t*>(value.command_surface_id.data()),
              value.command_surface_id.size());
  AppendU64Field(bytes, 3, value.structural_occurrence);
  AppendU32Field(bytes, 4, value.import_occurrence);
  AppendField(bytes, 5, value.target_name_atom_vector.data(),
              value.target_name_atom_vector.size());
  AppendU8Field(bytes, 6, value.input_format_demand);
  AppendU8Field(bytes, 7, value.character_encoding_demand);
  AppendU8Field(bytes, 8, value.conversion_policy_demand);
  AppendU8Field(bytes, 9, value.null_default_policy_demand);
  AppendU8Field(bytes, 10, value.reject_policy_demand);
  AppendU64Field(bytes, 11, value.maximum_rejected_rows);
  AppendArrayField(bytes, 12, value.syntax_demand_sha256);
}

void EncodeBindAckCanonical(const BindAck& value,
                            std::vector<std::uint8_t>* bytes) {
  BeginTlv(bytes);
  AppendArrayField(bytes, 1, value.authenticated_receipt_uuid);
  AppendArrayField(bytes, 2, value.binding_uuid);
  AppendU64Field(bytes, 3, value.binding_generation);
  AppendU64Field(bytes, 4, value.structural_occurrence);
  AppendU32Field(bytes, 5, value.import_occurrence);
  AppendArrayField(bytes, 6, value.syntax_demand_sha256);
  AppendArrayField(bytes, 7, value.binding_evidence_sha256);
}

template <typename T, typename Encoder>
bool IsCanonicalEncoding(const std::uint8_t* data,
                         std::size_t size,
                         const T& value,
                         Encoder encoder) {
  std::vector<std::uint8_t> canonical;
  encoder(value, &canonical);
  return canonical.size() == size &&
         std::equal(canonical.begin(), canonical.end(), data);
}

}  // namespace

Hash256 PayloadSha256(const std::vector<std::uint8_t>& payload) {
  return Hash(payload);
}

Hash256 ContentSha256(const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> preimage;
  static constexpr char kDomain[] =
      "ScratchBird.BulkImportStreamContent.V1";
  preimage.reserve(sizeof(kDomain) - 1 + 8 + payload.size());
  AppendDomain(&preimage, kDomain);
  AppendU64(&preimage, payload.size());
  preimage.insert(preimage.end(), payload.begin(), payload.end());
  return Hash(preimage);
}

Hash256 ChainStart(const Uuid& stream_uuid,
                   std::uint64_t stream_generation,
                   const Hash256& descriptor_evidence) {
  std::vector<std::uint8_t> preimage;
  AppendDomain(&preimage, "ScratchBird.BulkImportStreamChainStart.V1");
  preimage.insert(preimage.end(), stream_uuid.begin(), stream_uuid.end());
  AppendU64(&preimage, stream_generation);
  preimage.insert(preimage.end(), descriptor_evidence.begin(),
                  descriptor_evidence.end());
  return Hash(preimage);
}

Hash256 ChainStep(const Hash256& previous_chain_sha256,
                  std::uint64_t chunk_sequence,
                  std::uint64_t byte_offset,
                  const Hash256& payload_sha256,
                  std::uint32_t payload_size) {
  std::vector<std::uint8_t> preimage;
  AppendDomain(&preimage, "ScratchBird.BulkImportStreamChainStep.V1");
  preimage.insert(preimage.end(), previous_chain_sha256.begin(),
                  previous_chain_sha256.end());
  AppendU64(&preimage, chunk_sequence);
  AppendU64(&preimage, byte_offset);
  AppendU32(&preimage, payload_size);
  preimage.insert(preimage.end(), payload_sha256.begin(), payload_sha256.end());
  return Hash(preimage);
}

Hash256 ChunkAckEvidence(const ChunkAck& value) {
  std::vector<std::uint8_t> preimage;
  AppendDomain(&preimage, "ScratchBird.BulkImportStreamChunkAck.V1");
  preimage.insert(preimage.end(), value.stream_uuid.begin(), value.stream_uuid.end());
  AppendU64(&preimage, value.stream_generation);
  AppendU64(&preimage, value.accepted_sequence);
  AppendU64(&preimage, value.accepted_total_bytes);
  preimage.insert(preimage.end(), value.accepted_chain_sha256.begin(),
                  value.accepted_chain_sha256.end());
  AppendU64(&preimage, value.durable_spool_generation);
  return Hash(preimage);
}

Hash256 SealRequestEvidence(const Seal& value) {
  std::vector<std::uint8_t> preimage;
  AppendDomain(&preimage, "ScratchBird.BulkImportStreamSealRequest.V1");
  preimage.insert(preimage.end(), value.authenticated_receipt_uuid.begin(),
                  value.authenticated_receipt_uuid.end());
  preimage.insert(preimage.end(), value.stream_uuid.begin(), value.stream_uuid.end());
  AppendU64(&preimage, value.stream_generation);
  preimage.insert(preimage.end(), value.descriptor_evidence.begin(),
                  value.descriptor_evidence.end());
  AppendU64(&preimage, value.final_chunk_count);
  AppendU64(&preimage, value.total_stream_bytes);
  preimage.insert(preimage.end(), value.final_chain_sha256.begin(),
                  value.final_chain_sha256.end());
  preimage.insert(preimage.end(), value.content_sha256.begin(),
                  value.content_sha256.end());
  return Hash(preimage);
}

Hash256 SealAckEvidence(const SealAck& value) {
  std::vector<std::uint8_t> preimage;
  AppendDomain(&preimage, "ScratchBird.BulkImportStreamSeal.V1");
  preimage.insert(preimage.end(), value.stream_uuid.begin(), value.stream_uuid.end());
  AppendU64(&preimage, value.stream_generation);
  preimage.insert(preimage.end(), value.durable_spool_uuid.begin(),
                  value.durable_spool_uuid.end());
  AppendU64(&preimage, value.durable_spool_generation);
  AppendU64(&preimage, value.chunk_count);
  AppendU64(&preimage, value.total_stream_bytes);
  preimage.insert(preimage.end(), value.final_chain_sha256.begin(),
                  value.final_chain_sha256.end());
  preimage.insert(preimage.end(), value.content_sha256.begin(),
                  value.content_sha256.end());
  return Hash(preimage);
}

Hash256 BindDemandEvidence(const Bind& value) {
  std::vector<std::uint8_t> preimage;
  AppendDomain(&preimage, "ScratchBird.BulkImportStreamBindDemand.V1");
  preimage.insert(preimage.end(), value.authenticated_receipt_uuid.begin(),
                  value.authenticated_receipt_uuid.end());
  preimage.insert(preimage.end(), value.command_surface_id.begin(),
                  value.command_surface_id.end());
  AppendU64(&preimage, value.structural_occurrence);
  AppendU32(&preimage, value.import_occurrence);
  preimage.insert(preimage.end(), value.target_name_atom_vector.begin(),
                  value.target_name_atom_vector.end());
  preimage.push_back(value.input_format_demand);
  preimage.push_back(value.character_encoding_demand);
  preimage.push_back(value.conversion_policy_demand);
  preimage.push_back(value.null_default_policy_demand);
  preimage.push_back(value.reject_policy_demand);
  AppendU64(&preimage, value.maximum_rejected_rows);
  return Hash(preimage);
}

Hash256 BindAckEvidence(const BindAck& value) {
  std::vector<std::uint8_t> preimage;
  AppendDomain(&preimage, "ScratchBird.BulkImportStreamBindAck.V1");
  preimage.insert(preimage.end(), value.authenticated_receipt_uuid.begin(),
                  value.authenticated_receipt_uuid.end());
  preimage.insert(preimage.end(), value.binding_uuid.begin(), value.binding_uuid.end());
  AppendU64(&preimage, value.binding_generation);
  AppendU64(&preimage, value.structural_occurrence);
  AppendU32(&preimage, value.import_occurrence);
  preimage.insert(preimage.end(), value.syntax_demand_sha256.begin(),
                  value.syntax_demand_sha256.end());
  return Hash(preimage);
}

bool ValidateChunkEvidence(const Chunk& value) {
  return IsNonZero(value.payload_sha256) &&
         value.payload_sha256 == PayloadSha256(value.chunk_payload) &&
         IsNonZero(value.chunk_chain_sha256) &&
         value.chunk_chain_sha256 ==
             ChainStep(value.previous_chain_sha256, value.chunk_sequence,
                       value.byte_offset, value.payload_sha256,
                       static_cast<std::uint32_t>(value.chunk_payload.size()));
}

bool ValidateChunkAckEvidence(const ChunkAck& value) {
  return IsNonZero(value.ack_evidence_sha256) &&
         value.ack_evidence_sha256 == ChunkAckEvidence(value);
}

bool ValidateSealEvidence(const Seal& value) {
  return IsNonZero(value.seal_request_evidence_sha256) &&
         value.seal_request_evidence_sha256 == SealRequestEvidence(value);
}

bool ValidateSealAckEvidence(const SealAck& value) {
  return IsNonZero(value.seal_evidence_sha256) &&
         value.seal_evidence_sha256 == SealAckEvidence(value);
}

bool ValidateBindEvidence(const Bind& value) {
  return IsNonZero(value.authenticated_receipt_uuid) &&
         IsAdmittedCommandSurface(value.command_surface_id) &&
         value.structural_occurrence != 0 && value.import_occurrence != 0 &&
         IsNonZero(value.syntax_demand_sha256) &&
         value.syntax_demand_sha256 == BindDemandEvidence(value);
}

bool ValidateBindAckEvidence(const BindAck& value) {
  return IsNonZero(value.authenticated_receipt_uuid) &&
         IsNonZero(value.binding_uuid) && value.binding_generation != 0 &&
         value.structural_occurrence != 0 && value.import_occurrence != 0 &&
         IsNonZero(value.syntax_demand_sha256) &&
         IsNonZero(value.binding_evidence_sha256) &&
         value.binding_evidence_sha256 == BindAckEvidence(value);
}

bool DecodeBindTargetNameAtoms(
    const std::vector<std::uint8_t>& encoded,
    std::vector<BindTargetNameAtom>* atoms,
    std::string* detail) {
  if (atoms != nullptr) {
    atoms->clear();
  }
  if (!DecodeBindTargetNameAtomsStrict(encoded, atoms)) {
    return Fail(detail, "bulk_import.bind.target_name_atoms.invalid");
  }
  return true;
}

bool EncodeBindTargetNameAtoms(
    const std::vector<BindTargetNameAtom>& atoms,
    std::vector<std::uint8_t>* encoded,
    std::string* detail) {
  if (!EncodeBindTargetNameAtomsStrict(atoms, encoded)) {
    return Fail(detail, "bulk_import.bind.target_name_atoms.invalid");
  }
  return true;
}

bool EncodeChunk(const Chunk& value,
                 std::vector<std::uint8_t>* bytes,
                 std::string* detail) {
  if (bytes != nullptr) {
    bytes->clear();
  }
  if (bytes == nullptr || !IsValidChunk(value)) {
    return Fail(detail, "bulk_import.chunk.invalid");
  }
  EncodeChunkCanonical(value, bytes);
  return true;
}

bool DecodeChunk(const std::uint8_t* data,
                 std::size_t size,
                 Chunk* value,
                 std::string* detail) {
  if (value != nullptr) {
    *value = Chunk{};
  }
  Fields fields;
  Chunk decoded;
  if (value == nullptr || !ParseExactTlv(data, size, 12, &fields, detail) ||
      !CopyArray(fields[0], &decoded.authenticated_receipt_uuid) ||
      !CopyArray(fields[1], &decoded.stream_uuid) ||
      !ReadU64(fields[2], &decoded.stream_generation) ||
      !ReadU64(fields[3], &decoded.structural_occurrence) ||
      !ReadU32(fields[4], &decoded.import_occurrence) ||
      !CopyArray(fields[5], &decoded.descriptor_evidence) ||
      !ReadU64(fields[6], &decoded.chunk_sequence) ||
      !ReadU64(fields[7], &decoded.byte_offset) ||
      !CopyArray(fields[8], &decoded.previous_chain_sha256) ||
      !CopyArray(fields[9], &decoded.payload_sha256) ||
      fields[10].empty() || fields[10].size() > kMaximumChunkBytes ||
      !CopyArray(fields[11], &decoded.chunk_chain_sha256)) {
    return Fail(detail, "bulk_import.chunk.shape_invalid");
  }
  decoded.chunk_payload = fields[10];
  if (!IsValidChunk(decoded) ||
      !IsCanonicalEncoding(data, size, decoded, EncodeChunkCanonical)) {
    return Fail(detail, "bulk_import.chunk.semantic_invalid");
  }
  *value = std::move(decoded);
  return true;
}

bool EncodeChunkAck(const ChunkAck& value,
                    std::vector<std::uint8_t>* bytes,
                    std::string* detail) {
  if (bytes != nullptr) {
    bytes->clear();
  }
  if (bytes == nullptr || !IsValidChunkAck(value)) {
    return Fail(detail, "bulk_import.chunk_ack.invalid");
  }
  EncodeChunkAckCanonical(value, bytes);
  return true;
}

bool DecodeChunkAck(const std::uint8_t* data,
                    std::size_t size,
                    ChunkAck* value,
                    std::string* detail) {
  if (value != nullptr) {
    *value = ChunkAck{};
  }
  Fields fields;
  ChunkAck decoded;
  if (value == nullptr || !ParseExactTlv(data, size, 7, &fields, detail) ||
      !CopyArray(fields[0], &decoded.stream_uuid) ||
      !ReadU64(fields[1], &decoded.stream_generation) ||
      !ReadU64(fields[2], &decoded.accepted_sequence) ||
      !ReadU64(fields[3], &decoded.accepted_total_bytes) ||
      !CopyArray(fields[4], &decoded.accepted_chain_sha256) ||
      !ReadU64(fields[5], &decoded.durable_spool_generation) ||
      !CopyArray(fields[6], &decoded.ack_evidence_sha256) ||
      !IsValidChunkAck(decoded) ||
      !IsCanonicalEncoding(data, size, decoded, EncodeChunkAckCanonical)) {
    return Fail(detail, "bulk_import.chunk_ack.invalid");
  }
  *value = std::move(decoded);
  return true;
}

bool EncodeSeal(const Seal& value,
                std::vector<std::uint8_t>* bytes,
                std::string* detail) {
  if (bytes != nullptr) {
    bytes->clear();
  }
  if (bytes == nullptr || !IsValidSeal(value)) {
    return Fail(detail, "bulk_import.seal.invalid");
  }
  EncodeSealCanonical(value, bytes);
  return true;
}

bool DecodeSeal(const std::uint8_t* data,
                std::size_t size,
                Seal* value,
                std::string* detail) {
  if (value != nullptr) {
    *value = Seal{};
  }
  Fields fields;
  Seal decoded;
  if (value == nullptr || !ParseExactTlv(data, size, 9, &fields, detail) ||
      !CopyArray(fields[0], &decoded.authenticated_receipt_uuid) ||
      !CopyArray(fields[1], &decoded.stream_uuid) ||
      !ReadU64(fields[2], &decoded.stream_generation) ||
      !CopyArray(fields[3], &decoded.descriptor_evidence) ||
      !ReadU64(fields[4], &decoded.final_chunk_count) ||
      !ReadU64(fields[5], &decoded.total_stream_bytes) ||
      !CopyArray(fields[6], &decoded.final_chain_sha256) ||
      !CopyArray(fields[7], &decoded.content_sha256) ||
      !CopyArray(fields[8], &decoded.seal_request_evidence_sha256) ||
      !IsValidSeal(decoded) ||
      !IsCanonicalEncoding(data, size, decoded, EncodeSealCanonical)) {
    return Fail(detail, "bulk_import.seal.invalid");
  }
  *value = std::move(decoded);
  return true;
}

bool EncodeSealAck(const SealAck& value,
                   std::vector<std::uint8_t>* bytes,
                   std::string* detail) {
  if (bytes != nullptr) {
    bytes->clear();
  }
  if (bytes == nullptr || !IsValidSealAck(value)) {
    return Fail(detail, "bulk_import.seal_ack.invalid");
  }
  EncodeSealAckCanonical(value, bytes);
  return true;
}

bool DecodeSealAck(const std::uint8_t* data,
                   std::size_t size,
                   SealAck* value,
                   std::string* detail) {
  if (value != nullptr) {
    *value = SealAck{};
  }
  Fields fields;
  SealAck decoded;
  if (value == nullptr || !ParseExactTlv(data, size, 9, &fields, detail) ||
      !CopyArray(fields[0], &decoded.stream_uuid) ||
      !ReadU64(fields[1], &decoded.stream_generation) ||
      !CopyArray(fields[2], &decoded.durable_spool_uuid) ||
      !ReadU64(fields[3], &decoded.durable_spool_generation) ||
      !ReadU64(fields[4], &decoded.chunk_count) ||
      !ReadU64(fields[5], &decoded.total_stream_bytes) ||
      !CopyArray(fields[6], &decoded.final_chain_sha256) ||
      !CopyArray(fields[7], &decoded.content_sha256) ||
      !CopyArray(fields[8], &decoded.seal_evidence_sha256) ||
      !IsValidSealAck(decoded) ||
      !IsCanonicalEncoding(data, size, decoded, EncodeSealAckCanonical)) {
    return Fail(detail, "bulk_import.seal_ack.invalid");
  }
  *value = std::move(decoded);
  return true;
}

bool EncodeBind(const Bind& value,
                std::vector<std::uint8_t>* bytes,
                std::string* detail) {
  if (bytes != nullptr) {
    bytes->clear();
  }
  if (bytes == nullptr || !IsValidBind(value)) {
    return Fail(detail, "bulk_import.bind.invalid");
  }
  EncodeBindCanonical(value, bytes);
  return true;
}

bool DecodeBind(const std::uint8_t* data,
                std::size_t size,
                Bind* value,
                std::string* detail) {
  if (value != nullptr) {
    *value = Bind{};
  }
  Fields fields;
  Bind decoded;
  if (value == nullptr || !ParseExactTlv(data, size, 12, &fields, detail) ||
      !CopyArray(fields[0], &decoded.authenticated_receipt_uuid) ||
      fields[1].empty() || fields[1].size() > 4096 ||
      !ReadU64(fields[2], &decoded.structural_occurrence) ||
      !ReadU32(fields[3], &decoded.import_occurrence) || fields[4].empty() ||
      !ReadU8(fields[5], &decoded.input_format_demand) ||
      !ReadU8(fields[6], &decoded.character_encoding_demand) ||
      !ReadU8(fields[7], &decoded.conversion_policy_demand) ||
      !ReadU8(fields[8], &decoded.null_default_policy_demand) ||
      !ReadU8(fields[9], &decoded.reject_policy_demand) ||
      !ReadU64(fields[10], &decoded.maximum_rejected_rows) ||
      !CopyArray(fields[11], &decoded.syntax_demand_sha256)) {
    return Fail(detail, "bulk_import.bind.shape_invalid");
  }
  decoded.command_surface_id.assign(
      reinterpret_cast<const char*>(fields[1].data()), fields[1].size());
  decoded.target_name_atom_vector = fields[4];
  if (!IsValidBind(decoded) ||
      !IsCanonicalEncoding(data, size, decoded, EncodeBindCanonical)) {
    return Fail(detail, "bulk_import.bind.semantic_invalid");
  }
  *value = std::move(decoded);
  return true;
}

bool EncodeBindAck(const BindAck& value,
                   std::vector<std::uint8_t>* bytes,
                   std::string* detail) {
  if (bytes != nullptr) {
    bytes->clear();
  }
  if (bytes == nullptr || !IsValidBindAck(value)) {
    return Fail(detail, "bulk_import.bind_ack.invalid");
  }
  EncodeBindAckCanonical(value, bytes);
  return true;
}

bool DecodeBindAck(const std::uint8_t* data,
                   std::size_t size,
                   BindAck* value,
                   std::string* detail) {
  if (value != nullptr) {
    *value = BindAck{};
  }
  Fields fields;
  BindAck decoded;
  if (value == nullptr || !ParseExactTlv(data, size, 7, &fields, detail) ||
      !CopyArray(fields[0], &decoded.authenticated_receipt_uuid) ||
      !CopyArray(fields[1], &decoded.binding_uuid) ||
      !ReadU64(fields[2], &decoded.binding_generation) ||
      !ReadU64(fields[3], &decoded.structural_occurrence) ||
      !ReadU32(fields[4], &decoded.import_occurrence) ||
      !CopyArray(fields[5], &decoded.syntax_demand_sha256) ||
      !CopyArray(fields[6], &decoded.binding_evidence_sha256) ||
      !IsValidBindAck(decoded) ||
      !IsCanonicalEncoding(data, size, decoded, EncodeBindAckCanonical)) {
    return Fail(detail, "bulk_import.bind_ack.invalid");
  }
  *value = std::move(decoded);
  return true;
}

}  // namespace scratchbird::wire::sbps_bulk_import
