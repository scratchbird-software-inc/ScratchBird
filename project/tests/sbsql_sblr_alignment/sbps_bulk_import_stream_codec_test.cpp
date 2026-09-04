#include "wire/parser_server_ipc/sbps_bulk_import_stream_codec.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
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

}  // namespace

int main() {
  auto chunk = CanonicalChunk();
  std::vector<std::uint8_t> bytes;
  assert(EncodeChunk(chunk, &bytes));
  Chunk decoded_chunk;
  assert(DecodeChunk(bytes.data(), bytes.size(), &decoded_chunk));
  assert(decoded_chunk.chunk_payload == chunk.chunk_payload);
  assert(decoded_chunk.chunk_sequence == 1);

  auto malformed = bytes;
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

  ChunkAck chunk_ack;
  chunk_ack.stream_uuid = chunk.stream_uuid;
  chunk_ack.stream_generation = chunk.stream_generation;
  chunk_ack.accepted_sequence = 1;
  chunk_ack.accepted_total_bytes = chunk.chunk_payload.size();
  chunk_ack.accepted_chain_sha256 = chunk.chunk_chain_sha256;
  chunk_ack.durable_spool_generation = 2;
  chunk_ack.ack_evidence_sha256 = ChunkAckEvidence(chunk_ack);
  assert(EncodeChunkAck(chunk_ack, &bytes));
  ChunkAck decoded_chunk_ack;
  assert(DecodeChunkAck(bytes.data(), bytes.size(), &decoded_chunk_ack));
  auto bad_chunk_ack = chunk_ack;
  bad_chunk_ack.accepted_sequence = 0;
  bad_chunk_ack.ack_evidence_sha256 = ChunkAckEvidence(bad_chunk_ack);
  assert(!EncodeChunkAck(bad_chunk_ack, &bytes) && bytes.empty());
  bad_chunk_ack = chunk_ack;
  bad_chunk_ack.ack_evidence_sha256[0] ^= 1;
  assert(!ValidateChunkAckEvidence(bad_chunk_ack));

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
  Seal decoded_seal;
  assert(DecodeSeal(bytes.data(), bytes.size(), &decoded_seal));
  auto bad_seal = seal;
  bad_seal.final_chunk_count = 262'145;
  bad_seal.seal_request_evidence_sha256 = SealRequestEvidence(bad_seal);
  assert(!EncodeSeal(bad_seal, &bytes) && bytes.empty());
  bad_seal = seal;
  bad_seal.descriptor_evidence.fill(0);
  bad_seal.seal_request_evidence_sha256 = SealRequestEvidence(bad_seal);
  assert(!EncodeSeal(bad_seal, &bytes) && bytes.empty());

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
  SealAck decoded_seal_ack;
  assert(DecodeSealAck(bytes.data(), bytes.size(), &decoded_seal_ack));
  auto bad_seal_ack = seal_ack;
  bad_seal_ack.durable_spool_uuid.fill(0);
  bad_seal_ack.seal_evidence_sha256 = SealAckEvidence(bad_seal_ack);
  assert(!EncodeSealAck(bad_seal_ack, &bytes) && bytes.empty());

  auto bind = CanonicalBind(chunk.authenticated_receipt_uuid);
  assert(EncodeBind(bind, &bytes));
  Bind decoded_bind;
  assert(DecodeBind(bytes.data(), bytes.size(), &decoded_bind));
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
  BindAck decoded_bind_ack;
  assert(DecodeBindAck(bytes.data(), bytes.size(), &decoded_bind_ack));
  auto bad_bind_ack = bind_ack;
  bad_bind_ack.binding_evidence_sha256[0] ^= 1;
  assert(!ValidateBindAckEvidence(bad_bind_ack));
  bad_bind_ack = bind_ack;
  bad_bind_ack.binding_uuid.fill(0);
  bad_bind_ack.binding_evidence_sha256 = BindAckEvidence(bad_bind_ack);
  assert(!EncodeBindAck(bad_bind_ack, &bytes) && bytes.empty());

  return 0;
}
