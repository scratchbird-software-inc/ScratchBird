#include "wire/parser_server_ipc/sbps_bulk_import_stream_codec.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#ifdef NDEBUG
#undef assert
#define assert(condition)                                                     \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::abort();                                                           \
    }                                                                         \
  } while (false)
#endif

using namespace scratchbird::wire::sbps_bulk_import;

namespace {

template <typename T>
void Fill(T* value, std::uint8_t byte) {
  std::fill(value->begin(), value->end(), byte);
}

Chunk CanonicalChunk() {
  Chunk chunk;
  Fill(&chunk.authenticated_receipt_uuid, 1);
  Fill(&chunk.stream_uuid, 2);
  Fill(&chunk.descriptor_evidence, 3);
  chunk.stream_generation = 7;
  chunk.structural_occurrence = 8;
  chunk.import_occurrence = 9;
  chunk.chunk_sequence = 1;
  chunk.byte_offset = 0;
  chunk.chunk_payload = {10, 11, 12};
  chunk.previous_chain_sha256 =
      ChainStart(chunk.stream_uuid, chunk.stream_generation,
                 chunk.descriptor_evidence);
  chunk.payload_sha256 = PayloadSha256(chunk.chunk_payload);
  chunk.chunk_chain_sha256 =
      ChainStep(chunk.previous_chain_sha256, chunk.chunk_sequence,
                chunk.byte_offset, chunk.payload_sha256,
                static_cast<std::uint32_t>(chunk.chunk_payload.size()));
  return chunk;
}

Bind CanonicalBind(const Uuid& receipt_uuid) {
  Bind bind;
  bind.authenticated_receipt_uuid = receipt_uuid;
  bind.command_surface_id = "SBSQL-465931ED7427";
  bind.structural_occurrence = 1;
  bind.import_occurrence = 1;
  bind.target_name_atom_vector = {1, 3, 0, 't', 'b', 'l', 0};
  bind.input_format_demand = 1;
  bind.character_encoding_demand = 1;
  bind.conversion_policy_demand = 1;
  bind.null_default_policy_demand = 1;
  bind.reject_policy_demand = 1;
  bind.maximum_rejected_rows = 0;
  bind.syntax_demand_sha256 = BindDemandEvidence(bind);
  return bind;
}

void RefreshChunkEvidence(Chunk* chunk) {
  chunk->payload_sha256 = PayloadSha256(chunk->chunk_payload);
  chunk->chunk_chain_sha256 =
      ChainStep(chunk->previous_chain_sha256, chunk->chunk_sequence,
                chunk->byte_offset, chunk->payload_sha256,
                static_cast<std::uint32_t>(chunk->chunk_payload.size()));
}

std::vector<std::size_t> TlvFieldHeaders(
    const std::vector<std::uint8_t>& bytes) {
  std::vector<std::size_t> headers;
  if (bytes.size() < 2) return headers;
  std::size_t offset = 2;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 6) return {};
    headers.push_back(offset);
    const auto extent = static_cast<std::uint32_t>(bytes[offset + 2]) |
                        (static_cast<std::uint32_t>(bytes[offset + 3]) << 8U) |
                        (static_cast<std::uint32_t>(bytes[offset + 4]) << 16U) |
                        (static_cast<std::uint32_t>(bytes[offset + 5]) << 24U);
    offset += 6;
    if (extent > bytes.size() - offset) return {};
    offset += extent;
  }
  return offset == bytes.size() ? headers : std::vector<std::size_t>{};
}

template <typename Decode>
void RequireTlvShapeRefusals(const std::vector<std::uint8_t>& canonical,
                             std::size_t expected_fields,
                             Decode&& decode) {
  const auto headers = TlvFieldHeaders(canonical);
  assert(headers.size() == expected_fields);

  auto malformed = canonical;
  malformed[0] = 2;
  assert(!decode(malformed));

  malformed = canonical;
  malformed.resize(headers.back());
  assert(!decode(malformed));  // Missing required field.

  malformed = canonical;
  malformed[headers[1]] = 1;
  malformed[headers[1] + 1] = 0;
  assert(!decode(malformed));  // Duplicate field id.

  malformed = canonical;
  malformed[headers[2]] = 1;
  malformed[headers[2] + 1] = 0;
  assert(!decode(malformed));  // Descending field id.

  malformed = canonical;
  malformed[headers[0]] = 0xff;
  malformed[headers[0] + 1] = 0x7f;
  assert(!decode(malformed));  // Unknown field id.

  malformed = canonical;
  --malformed[headers[0] + 2];
  assert(!decode(malformed));  // Wrong primitive length.

  malformed = canonical;
  malformed.push_back(0);
  assert(!decode(malformed));  // Trailing partial field.

  malformed = canonical;
  malformed.pop_back();
  assert(!decode(malformed));  // Truncated value.
}

}  // namespace

int main() {
  auto chunk = CanonicalChunk();
  std::vector<std::uint8_t> bytes;
  assert(EncodeChunk(chunk, &bytes));
  Chunk decoded_chunk;
  assert(DecodeChunk(bytes.data(), bytes.size(), &decoded_chunk));
  assert(decoded_chunk.chunk_payload == chunk.chunk_payload);
  assert(decoded_chunk.chunk_sequence == 1);
  const auto canonical_chunk_bytes = bytes;
  std::vector<std::uint8_t> reencoded;
  assert(EncodeChunk(decoded_chunk, &reencoded));
  assert(reencoded == canonical_chunk_bytes);
  RequireTlvShapeRefusals(
      canonical_chunk_bytes, 12, [&](const auto& candidate) {
        return DecodeChunk(candidate.data(), candidate.size(), &decoded_chunk);
      });

  auto malformed = canonical_chunk_bytes;
  malformed[2] = 2;  // Field 1 cannot be renamed to field 2.
  assert(!DecodeChunk(malformed.data(), malformed.size(), &decoded_chunk));
  malformed = bytes;
  malformed.push_back(0);  // No trailing material.
  assert(!DecodeChunk(malformed.data(), malformed.size(), &decoded_chunk));
  malformed = bytes;
  malformed.pop_back();  // No truncation.
  assert(!DecodeChunk(malformed.data(), malformed.size(), &decoded_chunk));

  auto wrong_first_chain = chunk;
  Fill(&wrong_first_chain.previous_chain_sha256, 4);
  RefreshChunkEvidence(&wrong_first_chain);
  bytes = {9};
  assert(!EncodeChunk(wrong_first_chain, &bytes) && bytes.empty());

  auto second_chunk = chunk;
  second_chunk.chunk_sequence = 2;
  second_chunk.byte_offset = chunk.chunk_payload.size();
  second_chunk.previous_chain_sha256 = chunk.chunk_chain_sha256;
  second_chunk.chunk_payload = {13};
  RefreshChunkEvidence(&second_chunk);
  assert(EncodeChunk(second_chunk, &bytes));

  auto overflow_chunk = second_chunk;
  overflow_chunk.byte_offset = 17'179'869'184ULL;
  RefreshChunkEvidence(&overflow_chunk);
  bytes = {9};
  assert(!EncodeChunk(overflow_chunk, &bytes) && bytes.empty());

  auto zero_chunk = chunk;
  zero_chunk.stream_uuid.fill(0);
  bytes = {9};
  assert(!EncodeChunk(zero_chunk, &bytes) && bytes.empty());

  auto over_limit_chunk = chunk;
  over_limit_chunk.chunk_payload.assign(8'388'609, 1);
  bytes = {9};
  assert(!EncodeChunk(over_limit_chunk, &bytes) && bytes.empty());

  auto one_byte_chunk = chunk;
  one_byte_chunk.chunk_payload = {0x7f};
  RefreshChunkEvidence(&one_byte_chunk);
  assert(EncodeChunk(one_byte_chunk, &bytes));
  assert(DecodeChunk(bytes.data(), bytes.size(), &decoded_chunk));
  assert(decoded_chunk.chunk_payload == one_byte_chunk.chunk_payload);

  auto maximum_chunk = chunk;
  maximum_chunk.chunk_payload.assign(8'388'608, 0x5a);
  RefreshChunkEvidence(&maximum_chunk);
  assert(EncodeChunk(maximum_chunk, &bytes));
  assert(DecodeChunk(bytes.data(), bytes.size(), &decoded_chunk));
  assert(decoded_chunk.chunk_payload.size() == 8'388'608);
  assert(decoded_chunk.payload_sha256 == maximum_chunk.payload_sha256);

  ChunkAck chunk_ack;
  chunk_ack.stream_uuid = chunk.stream_uuid;
  chunk_ack.stream_generation = chunk.stream_generation;
  chunk_ack.accepted_sequence = 1;
  chunk_ack.accepted_total_bytes = chunk.chunk_payload.size();
  chunk_ack.accepted_chain_sha256 = chunk.chunk_chain_sha256;
  chunk_ack.durable_spool_generation = 2;
  chunk_ack.ack_evidence_sha256 = ChunkAckEvidence(chunk_ack);
  assert(EncodeChunkAck(chunk_ack, &bytes));
  const auto canonical_chunk_ack_bytes = bytes;
  ChunkAck decoded_chunk_ack;
  assert(DecodeChunkAck(bytes.data(), bytes.size(), &decoded_chunk_ack));
  assert(EncodeChunkAck(decoded_chunk_ack, &reencoded));
  assert(reencoded == canonical_chunk_ack_bytes);
  RequireTlvShapeRefusals(
      canonical_chunk_ack_bytes, 7, [&](const auto& candidate) {
        return DecodeChunkAck(candidate.data(), candidate.size(),
                              &decoded_chunk_ack);
      });
  auto bad_chunk_ack = chunk_ack;
  bad_chunk_ack.accepted_sequence = 0;
  bad_chunk_ack.ack_evidence_sha256 = ChunkAckEvidence(bad_chunk_ack);
  assert(!EncodeChunkAck(bad_chunk_ack, &bytes) && bytes.empty());
  bad_chunk_ack = chunk_ack;
  bad_chunk_ack.ack_evidence_sha256[0] ^= 1;
  assert(!ValidateChunkAckEvidence(bad_chunk_ack));
  for (const auto clear : {0, 1, 2, 3, 4, 5}) {
    bad_chunk_ack = chunk_ack;
    switch (clear) {
      case 0: bad_chunk_ack.stream_uuid.fill(0); break;
      case 1: bad_chunk_ack.stream_generation = 0; break;
      case 2: bad_chunk_ack.accepted_sequence = 0; break;
      case 3: bad_chunk_ack.accepted_total_bytes = 0; break;
      case 4: bad_chunk_ack.accepted_chain_sha256.fill(0); break;
      case 5: bad_chunk_ack.durable_spool_generation = 0; break;
    }
    bad_chunk_ack.ack_evidence_sha256 = ChunkAckEvidence(bad_chunk_ack);
    assert(!EncodeChunkAck(bad_chunk_ack, &bytes) && bytes.empty());
  }

  Seal seal;
  seal.authenticated_receipt_uuid = chunk.authenticated_receipt_uuid;
  seal.stream_uuid = chunk.stream_uuid;
  seal.stream_generation = chunk.stream_generation;
  seal.descriptor_evidence = chunk.descriptor_evidence;
  seal.final_chunk_count = 1;
  seal.total_stream_bytes = chunk.chunk_payload.size();
  seal.final_chain_sha256 = chunk.chunk_chain_sha256;
  Fill(&seal.content_sha256, 5);
  seal.seal_request_evidence_sha256 = SealRequestEvidence(seal);
  assert(EncodeSeal(seal, &bytes));
  const auto canonical_seal_bytes = bytes;
  Seal decoded_seal;
  assert(DecodeSeal(bytes.data(), bytes.size(), &decoded_seal));
  assert(EncodeSeal(decoded_seal, &reencoded));
  assert(reencoded == canonical_seal_bytes);
  RequireTlvShapeRefusals(
      canonical_seal_bytes, 9, [&](const auto& candidate) {
        return DecodeSeal(candidate.data(), candidate.size(), &decoded_seal);
      });
  auto bad_seal = seal;
  bad_seal.final_chunk_count = 262'145;
  bad_seal.seal_request_evidence_sha256 = SealRequestEvidence(bad_seal);
  assert(!EncodeSeal(bad_seal, &bytes) && bytes.empty());
  bad_seal = seal;
  bad_seal.descriptor_evidence.fill(0);
  bad_seal.seal_request_evidence_sha256 = SealRequestEvidence(bad_seal);
  assert(!EncodeSeal(bad_seal, &bytes) && bytes.empty());

  const std::vector<std::uint8_t> multi_chunk_payload = {10, 11, 12, 13};
  auto multi_chunk_seal = seal;
  multi_chunk_seal.final_chunk_count = 2;
  multi_chunk_seal.total_stream_bytes = multi_chunk_payload.size();
  multi_chunk_seal.final_chain_sha256 = second_chunk.chunk_chain_sha256;
  multi_chunk_seal.content_sha256 = ContentSha256(multi_chunk_payload);
  multi_chunk_seal.seal_request_evidence_sha256 =
      SealRequestEvidence(multi_chunk_seal);
  assert(EncodeSeal(multi_chunk_seal, &bytes));
  assert(DecodeSeal(bytes.data(), bytes.size(), &decoded_seal));
  assert(decoded_seal.final_chunk_count == 2);
  assert(decoded_seal.final_chain_sha256 == second_chunk.chunk_chain_sha256);

  SealAck seal_ack;
  seal_ack.stream_uuid = chunk.stream_uuid;
  Fill(&seal_ack.durable_spool_uuid, 7);
  seal_ack.stream_generation = chunk.stream_generation;
  seal_ack.durable_spool_generation = 2;
  seal_ack.chunk_count = 1;
  seal_ack.total_stream_bytes = chunk.chunk_payload.size();
  seal_ack.final_chain_sha256 = chunk.chunk_chain_sha256;
  seal_ack.content_sha256 = seal.content_sha256;
  seal_ack.seal_evidence_sha256 = SealAckEvidence(seal_ack);
  assert(EncodeSealAck(seal_ack, &bytes));
  const auto canonical_seal_ack_bytes = bytes;
  SealAck decoded_seal_ack;
  assert(DecodeSealAck(bytes.data(), bytes.size(), &decoded_seal_ack));
  assert(EncodeSealAck(decoded_seal_ack, &reencoded));
  assert(reencoded == canonical_seal_ack_bytes);
  RequireTlvShapeRefusals(
      canonical_seal_ack_bytes, 9, [&](const auto& candidate) {
        return DecodeSealAck(candidate.data(), candidate.size(),
                             &decoded_seal_ack);
      });
  auto bad_seal_ack = seal_ack;
  bad_seal_ack.durable_spool_uuid.fill(0);
  bad_seal_ack.seal_evidence_sha256 = SealAckEvidence(bad_seal_ack);
  assert(!EncodeSealAck(bad_seal_ack, &bytes) && bytes.empty());

  auto bind = CanonicalBind(chunk.authenticated_receipt_uuid);
  assert(EncodeBind(bind, &bytes));
  const auto canonical_bind_bytes = bytes;
  Bind decoded_bind;
  assert(DecodeBind(bytes.data(), bytes.size(), &decoded_bind));
  assert(EncodeBind(decoded_bind, &reencoded));
  assert(reencoded == canonical_bind_bytes);
  RequireTlvShapeRefusals(
      canonical_bind_bytes, 12, [&](const auto& candidate) {
        return DecodeBind(candidate.data(), candidate.size(), &decoded_bind);
      });
  assert(DecodeBind(canonical_bind_bytes.data(), canonical_bind_bytes.size(),
                    &decoded_bind));
  std::vector<BindTargetNameAtom> decoded_atoms;
  assert(DecodeBindTargetNameAtoms(decoded_bind.target_name_atom_vector,
                                   &decoded_atoms));
  assert(decoded_atoms.size() == 1);
  assert(decoded_atoms[0].raw_utf8 == "tbl");
  assert(!decoded_atoms[0].quoted);

  auto utf8_bind = bind;
  utf8_bind.target_name_atom_vector = {1, 2, 0, 0xc3, 0xa9, 1};
  utf8_bind.syntax_demand_sha256 = BindDemandEvidence(utf8_bind);
  assert(EncodeBind(utf8_bind, &bytes));
  assert(DecodeBind(bytes.data(), bytes.size(), &decoded_bind));
  assert(DecodeBindTargetNameAtoms(decoded_bind.target_name_atom_vector,
                                   &decoded_atoms));
  assert(decoded_atoms.size() == 1);
  assert(decoded_atoms[0].raw_utf8 == "\xc3\xa9");
  assert(decoded_atoms[0].quoted);

  auto three_atom_bind = bind;
  three_atom_bind.target_name_atom_vector = {
      3, 1, 0, 'a', 0, 1, 0, 'b', 1, 1, 0, 'c', 0};
  three_atom_bind.syntax_demand_sha256 = BindDemandEvidence(three_atom_bind);
  assert(EncodeBind(three_atom_bind, &bytes));
  assert(DecodeBindTargetNameAtoms(three_atom_bind.target_name_atom_vector,
                                   &decoded_atoms));
  assert(decoded_atoms.size() == 3);
  assert(decoded_atoms[0].raw_utf8 == "a" && !decoded_atoms[0].quoted);
  assert(decoded_atoms[1].raw_utf8 == "b" && decoded_atoms[1].quoted);
  assert(decoded_atoms[2].raw_utf8 == "c" && !decoded_atoms[2].quoted);

  std::string atom_detail;
  decoded_atoms = {{"must-be-cleared", true}};
  assert(!DecodeBindTargetNameAtoms({}, &decoded_atoms, &atom_detail));
  assert(decoded_atoms.empty());
  assert(atom_detail == "bulk_import.bind.target_name_atoms.invalid");
  assert(!DecodeBindTargetNameAtoms({0}, &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms({4}, &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms({1, 1}, &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms({1, 0, 0, 0}, &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms({1, 1, 0, 0xff, 0}, &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms({1, 1, 0, 'x'}, &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms({1, 1, 0, 'x', 2}, &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms({1, 1, 0, 'x', 0, 0},
                                    &decoded_atoms));
  assert(!DecodeBindTargetNameAtoms(bind.target_name_atom_vector, nullptr,
                                    &atom_detail));

  auto bad_bind = bind;
  bad_bind.target_name_atom_vector = {1, 1, 0, 0xff, 0};
  bad_bind.syntax_demand_sha256 = BindDemandEvidence(bad_bind);
  assert(!EncodeBind(bad_bind, &bytes) && bytes.empty());
  bad_bind = bind;
  bad_bind.target_name_atom_vector = {1, 1, 0, 0, 0};
  bad_bind.syntax_demand_sha256 = BindDemandEvidence(bad_bind);
  assert(!EncodeBind(bad_bind, &bytes) && bytes.empty());
  bad_bind = bind;
  bad_bind.target_name_atom_vector = {1, 1, 0, 'x', 2};
  bad_bind.syntax_demand_sha256 = BindDemandEvidence(bad_bind);
  assert(!EncodeBind(bad_bind, &bytes) && bytes.empty());
  bad_bind = bind;
  bad_bind.input_format_demand = 2;
  bad_bind.syntax_demand_sha256 = BindDemandEvidence(bad_bind);
  assert(!EncodeBind(bad_bind, &bytes) && bytes.empty());
  bad_bind = bind;
  bad_bind.maximum_rejected_rows = 1;
  bad_bind.syntax_demand_sha256 = BindDemandEvidence(bad_bind);
  assert(!EncodeBind(bad_bind, &bytes) && bytes.empty());
  bad_bind = bind;
  bad_bind.command_surface_id = "SBSQL-NOT-ADMITTED";
  bad_bind.syntax_demand_sha256 = BindDemandEvidence(bad_bind);
  assert(!EncodeBind(bad_bind, &bytes) && bytes.empty());
  bad_bind = bind;
  bad_bind.syntax_demand_sha256[0] ^= 1;
  assert(!ValidateBindEvidence(bad_bind));

  assert(EncodeBind(bind, &bytes));
  malformed = bytes;
  malformed[2] = 2;
  assert(!DecodeBind(malformed.data(), malformed.size(), &decoded_bind));
  malformed = bytes;
  malformed.push_back(0);
  assert(!DecodeBind(malformed.data(), malformed.size(), &decoded_bind));

  BindAck bind_ack;
  bind_ack.authenticated_receipt_uuid = bind.authenticated_receipt_uuid;
  Fill(&bind_ack.binding_uuid, 8);
  bind_ack.binding_generation = 1;
  bind_ack.structural_occurrence = bind.structural_occurrence;
  bind_ack.import_occurrence = bind.import_occurrence;
  bind_ack.syntax_demand_sha256 = bind.syntax_demand_sha256;
  bind_ack.binding_evidence_sha256 = BindAckEvidence(bind_ack);
  assert(EncodeBindAck(bind_ack, &bytes));
  const auto canonical_bind_ack_bytes = bytes;
  BindAck decoded_bind_ack;
  assert(DecodeBindAck(bytes.data(), bytes.size(), &decoded_bind_ack));
  assert(EncodeBindAck(decoded_bind_ack, &reencoded));
  assert(reencoded == canonical_bind_ack_bytes);
  RequireTlvShapeRefusals(
      canonical_bind_ack_bytes, 7, [&](const auto& candidate) {
        return DecodeBindAck(candidate.data(), candidate.size(),
                             &decoded_bind_ack);
      });
  auto bad_bind_ack = bind_ack;
  bad_bind_ack.binding_evidence_sha256[0] ^= 1;
  assert(!ValidateBindAckEvidence(bad_bind_ack));
  bad_bind_ack = bind_ack;
  bad_bind_ack.binding_uuid.fill(0);
  bad_bind_ack.binding_evidence_sha256 = BindAckEvidence(bad_bind_ack);
  assert(!EncodeBindAck(bad_bind_ack, &bytes) && bytes.empty());

  return 0;
}
