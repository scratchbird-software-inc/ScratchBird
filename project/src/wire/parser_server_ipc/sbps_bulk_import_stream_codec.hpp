#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::wire::sbps_bulk_import {
using Uuid = std::array<std::uint8_t, 16>;
using Hash256 = std::array<std::uint8_t, 32>;

struct Chunk {
  Uuid authenticated_receipt_uuid{}, stream_uuid{};
  std::uint64_t stream_generation = 0, structural_occurrence = 0;
  std::uint32_t import_occurrence = 0;
  Hash256 descriptor_evidence{}, previous_chain_sha256{}, payload_sha256{}, chunk_chain_sha256{};
  std::uint64_t chunk_sequence = 0, byte_offset = 0;
  std::vector<std::uint8_t> chunk_payload;
};
struct ChunkAck {
  Uuid stream_uuid{};
  std::uint64_t stream_generation = 0, accepted_sequence = 0, accepted_total_bytes = 0;
  Hash256 accepted_chain_sha256{}, ack_evidence_sha256{};
  std::uint64_t durable_spool_generation = 0;
};
struct Seal {
  Uuid authenticated_receipt_uuid{}, stream_uuid{};
  std::uint64_t stream_generation = 0;
  Hash256 descriptor_evidence{};
  std::uint64_t final_chunk_count = 0, total_stream_bytes = 0;
  Hash256 final_chain_sha256{}, content_sha256{}, seal_request_evidence_sha256{};
};
struct SealAck {
  Uuid stream_uuid{}, durable_spool_uuid{};
  std::uint64_t stream_generation = 0, durable_spool_generation = 0;
  std::uint64_t chunk_count = 0, total_stream_bytes = 0;
  Hash256 final_chain_sha256{}, content_sha256{}, seal_evidence_sha256{};
};
struct Bind {
  Uuid authenticated_receipt_uuid{}; std::string command_surface_id;
  std::uint64_t structural_occurrence=0; std::uint32_t import_occurrence=0;
  std::vector<std::uint8_t> target_name_atom_vector;
  std::uint8_t input_format_demand=0, character_encoding_demand=0, conversion_policy_demand=0, null_default_policy_demand=0, reject_policy_demand=0;
  std::uint64_t maximum_rejected_rows=0; Hash256 syntax_demand_sha256{};
};
struct BindAck { Uuid authenticated_receipt_uuid{}, binding_uuid{}; std::uint64_t binding_generation=0, structural_occurrence=0; std::uint32_t import_occurrence=0; Hash256 syntax_demand_sha256{}, binding_evidence_sha256{}; };

struct BindTargetNameAtom {
  std::string raw_utf8;
  bool quoted = false;
};

bool EncodeChunk(const Chunk&, std::vector<std::uint8_t>*, std::string* = nullptr);
bool DecodeChunk(const std::uint8_t*, std::size_t, Chunk*, std::string* = nullptr);
bool EncodeChunkAck(const ChunkAck&, std::vector<std::uint8_t>*, std::string* = nullptr);
bool DecodeChunkAck(const std::uint8_t*, std::size_t, ChunkAck*, std::string* = nullptr);
bool EncodeSeal(const Seal&, std::vector<std::uint8_t>*, std::string* = nullptr);
bool DecodeSeal(const std::uint8_t*, std::size_t, Seal*, std::string* = nullptr);
bool EncodeSealAck(const SealAck&, std::vector<std::uint8_t>*, std::string* = nullptr);
bool DecodeSealAck(const std::uint8_t*, std::size_t, SealAck*, std::string* = nullptr);
bool EncodeBind(const Bind&, std::vector<std::uint8_t>*, std::string* = nullptr);
bool DecodeBind(const std::uint8_t*, std::size_t, Bind*, std::string* = nullptr);
bool DecodeBindTargetNameAtoms(const std::vector<std::uint8_t>&,
                               std::vector<BindTargetNameAtom>*,
                               std::string* = nullptr);
bool EncodeBindTargetNameAtoms(const std::vector<BindTargetNameAtom>&,
                               std::vector<std::uint8_t>*,
                               std::string* = nullptr);
bool EncodeBindAck(const BindAck&, std::vector<std::uint8_t>*, std::string* = nullptr);
bool DecodeBindAck(const std::uint8_t*, std::size_t, BindAck*, std::string* = nullptr);
Hash256 PayloadSha256(const std::vector<std::uint8_t>& payload);
Hash256 ContentSha256(const std::vector<std::uint8_t>& payload);
Hash256 ChainStart(const Uuid&, std::uint64_t, const Hash256&);
Hash256 ChainStep(const Hash256&, std::uint64_t, std::uint64_t, const Hash256&, std::uint32_t);
Hash256 ChunkAckEvidence(const ChunkAck&);
Hash256 SealRequestEvidence(const Seal&);
Hash256 SealAckEvidence(const SealAck&);
Hash256 BindDemandEvidence(const Bind&);
Hash256 BindAckEvidence(const BindAck&);
bool ValidateChunkEvidence(const Chunk&);
bool ValidateChunkAckEvidence(const ChunkAck&);
bool ValidateSealEvidence(const Seal&);
bool ValidateSealAckEvidence(const SealAck&);
bool ValidateBindEvidence(const Bind&);
bool ValidateBindAckEvidence(const BindAck&);
}
