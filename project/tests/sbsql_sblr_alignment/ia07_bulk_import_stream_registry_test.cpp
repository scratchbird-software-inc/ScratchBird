#include "core/hash/hash_digest.hpp"
#include "engine/internal_api/sblr_bulk_import_stream_registry.hpp"
#include "wire/parser_server_ipc/sbps_bulk_import_stream_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace wire = scratchbird::wire::sbps_bulk_import;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "bulk_import_stream_registry: " << detail << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool value, std::string_view detail) {
  if (!value) Fail(detail);
}

sblr::BulkImportUuid Uuid(std::uint8_t seed) {
  sblr::BulkImportUuid value{};
  value[0] = 0x19;
  value[1] = seed;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = static_cast<std::uint8_t>(seed ^ 0xa5u);
  return value;
}

sblr::BulkImportSha Hash(std::string_view text) {
  return scratchbird::core::hash::ComputeSha256Digest(
             reinterpret_cast<const std::uint8_t*>(text.data()), text.size())
      .digest;
}

void StoreLe64(std::uint8_t* output, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) output[i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::string Hex(const sblr::BulkImportUuid& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : value) {
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 0x0f]);
  }
  return text;
}

std::filesystem::path UniqueRoot() {
  std::array<char, 64> pattern{};
  const std::string prefix = "/tmp/sb_bulk_import_registry_ia07_XXXXXX";
  std::copy(prefix.begin(), prefix.end(), pattern.begin());
  char* created = ::mkdtemp(pattern.data());
  Require(created != nullptr, "mkdtemp failed");
  return created;
}

api::BulkImportStreamAllocation Allocation(std::uint8_t seed,
                                           std::uint64_t maximum_bytes = 1024) {
  api::BulkImportStreamAllocation value;
  value.authenticated_receipt_uuid = Uuid(static_cast<std::uint8_t>(seed + 1));
  value.stream_uuid = Uuid(static_cast<std::uint8_t>(seed + 2));
  value.stream_generation = 3;
  value.structural_occurrence = 7;
  value.import_occurrence = 2;
  value.descriptor_evidence = Hash("descriptor-" + std::to_string(seed));
  value.authority_evidence_sha256 = Hash("authority-" + std::to_string(seed));
  value.syntax_demand_sha256 = Hash("syntax-demand-" + std::to_string(seed));
  value.durable_spool_uuid = Uuid(static_cast<std::uint8_t>(seed + 3));
  value.durable_spool_generation = 1;
  value.target_relation_uuid = Uuid(static_cast<std::uint8_t>(seed + 4));
  value.target_relation_generation = 1;
  value.owning_transaction_uuid = Uuid(static_cast<std::uint8_t>(seed + 5));
  value.owning_local_transaction_id = 11;
  value.statement_snapshot_uuid = Uuid(static_cast<std::uint8_t>(seed + 6));
  value.catalog_epoch_uuid = Uuid(static_cast<std::uint8_t>(seed + 7));
  value.catalog_generation = 1;
  value.security_context_uuid = Uuid(static_cast<std::uint8_t>(seed + 8));
  value.security_epoch = 1;
  value.policy_snapshot_uuid = Uuid(static_cast<std::uint8_t>(seed + 9));
  value.policy_generation = 1;
  value.route_snapshot_uuid = Uuid(static_cast<std::uint8_t>(seed + 10));
  value.route_generation = 1;
  value.recovery_operation_uuid = Uuid(static_cast<std::uint8_t>(seed + 11));
  value.recovery_generation = 1;
  value.row_shape_uuid = Uuid(static_cast<std::uint8_t>(seed + 12));
  value.row_shape_generation = 1;
  value.column_descriptor_set_sha256 = Hash("columns-" + std::to_string(seed));
  value.import_policy_bundle_sha256 = Hash("policy-" + std::to_string(seed));
  value.resource_grant_uuid = Uuid(static_cast<std::uint8_t>(seed + 13));
  value.resource_grant_generation = 1;
  value.executor_availability_generation = 5;
  value.effective_maximum_stream_bytes = maximum_bytes;
  value.effective_maximum_chunk_count = 16;
  value.effective_maximum_chunk_bytes = 32;
  return value;
}

api::BulkImportChunk Chunk(const api::BulkImportStreamAllocation& allocation,
                           std::uint64_t sequence,
                           std::uint64_t offset,
                           const sblr::BulkImportSha& previous,
                           std::vector<std::uint8_t> payload) {
  api::BulkImportChunk value;
  value.authenticated_receipt_uuid = allocation.authenticated_receipt_uuid;
  value.stream_uuid = allocation.stream_uuid;
  value.stream_generation = allocation.stream_generation;
  value.structural_occurrence = allocation.structural_occurrence;
  value.import_occurrence = allocation.import_occurrence;
  value.descriptor_evidence = allocation.descriptor_evidence;
  value.sequence = sequence;
  value.byte_offset = offset;
  value.previous_chain_sha = previous;
  value.payload = std::move(payload);
  value.payload_sha = wire::PayloadSha256(value.payload);
  value.chain_sha = wire::ChainStep(value.previous_chain_sha,
                                    value.sequence,
                                    value.byte_offset,
                                    value.payload_sha,
                                    static_cast<std::uint32_t>(value.payload.size()));
  return value;
}

sblr::BulkImportSha ContentHash(const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain = "ScratchBird.BulkImportStreamContent.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  for (std::size_t i = 0; i < 8; ++i) {
    material.push_back(static_cast<std::uint8_t>(payload.size() >> (i * 8)));
  }
  material.insert(material.end(), payload.begin(), payload.end());
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

api::BulkImportSeal Seal(const api::BulkImportStreamAllocation& allocation,
                         std::uint64_t chunks,
                         std::uint64_t bytes,
                         const sblr::BulkImportSha& chain,
                         const sblr::BulkImportSha& content) {
  api::BulkImportSeal value;
  value.authenticated_receipt_uuid = allocation.authenticated_receipt_uuid;
  value.stream_uuid = allocation.stream_uuid;
  value.stream_generation = allocation.stream_generation;
  value.descriptor_evidence = allocation.descriptor_evidence;
  value.final_chunk_count = chunks;
  value.total_stream_bytes = bytes;
  value.final_chain_sha = chain;
  value.content_sha = content;
  wire::Seal encoded;
  encoded.authenticated_receipt_uuid = value.authenticated_receipt_uuid;
  encoded.stream_uuid = value.stream_uuid;
  encoded.stream_generation = value.stream_generation;
  encoded.descriptor_evidence = value.descriptor_evidence;
  encoded.final_chunk_count = value.final_chunk_count;
  encoded.total_stream_bytes = value.total_stream_bytes;
  encoded.final_chain_sha256 = value.final_chain_sha;
  encoded.content_sha256 = value.content_sha;
  value.seal_request_evidence = wire::SealRequestEvidence(encoded);
  return value;
}

std::vector<std::uint8_t> ResultWire(const api::BulkImportStreamAllocation& allocation,
                                     const api::BulkImportPublicationRecord& publication,
                                     std::uint64_t input_bytes,
                                     std::uint64_t chunks,
                                     const sblr::BulkImportSha& content) {
  sblr::SblrBulkImportStreamResultV1 result;
  auto& body = result.canonical_body;
  std::copy(allocation.stream_uuid.begin(), allocation.stream_uuid.end(), body.begin());
  StoreLe64(body.data() + 16, allocation.stream_generation);
  std::copy(publication.durable_publication_uuid.begin(),
            publication.durable_publication_uuid.end(),
            body.begin() + 24);
  StoreLe64(body.data() + 40, publication.durable_publication_generation);
  StoreLe64(body.data() + 48, publication.affected_rows);
  StoreLe64(body.data() + 56, publication.rejected_rows);
  StoreLe64(body.data() + 64, input_bytes);
  StoreLe64(body.data() + 72, chunks);
  std::copy(allocation.owning_transaction_uuid.begin(),
            allocation.owning_transaction_uuid.end(),
            body.begin() + 80);
  StoreLe64(body.data() + 96, allocation.owning_local_transaction_id);
  std::copy(content.begin(), content.end(), body.begin() + 104);
  result.availability_generation = allocation.executor_availability_generation;
  const auto wire = sblr::EncodeSblrBulkImportStreamResultV1(result);
  Require(wire.size() == sblr::BulkImportWireLayout::result_size, "BIRS fixture encoding failed");
  return wire;
}

void AppendBytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::app);
  Require(bool(output), "artifact append open failed");
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  Require(bool(output), "artifact append failed");
}

void Touch(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  Require(bool(output), "artifact touch failed");
  output.close();
  Require(::chmod(path.c_str(), 0600) == 0,
          "artifact private-mode setup failed");
}

void FlipByte(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  Require(bool(file), "artifact mutation open failed");
  file.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  file.read(&byte, 1);
  Require(bool(file), "artifact mutation read failed");
  byte ^= 0x5a;
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(&byte, 1);
  file.flush();
  Require(bool(file), "artifact mutation write failed");
}

}  // namespace

int main() {
  const auto root = UniqueRoot();
  const auto allocation_a = Allocation(10);
  auto allocation_b = Allocation(20);
  allocation_b.stream_uuid[0] = allocation_a.stream_uuid[0];
  const auto base_a = "stream_" + Hex(allocation_a.stream_uuid);
  const auto base_b = "stream_" + Hex(allocation_b.stream_uuid);
  Require(base_a != base_b, "full UUID filename fixture collided");

  std::vector<std::uint8_t> ack_one;
  std::vector<std::uint8_t> seal_ack;
  std::vector<std::uint8_t> result_wire;
  api::BulkImportChunk chunk_one;
  api::BulkImportSeal exact_seal;

  {
    api::SblrBulkImportStreamRegistry registry(root);
    Require(registry.healthy(), "registry root was not admitted");
    api::SblrBulkImportStreamRegistry competing(root);
    Require(!competing.healthy() && competing.startup_error() == "root_lock_busy",
            "same-root process lock was not exclusive");
    bool factory_called = false;
    const auto first_allocation = registry.AllocateOrReplay(
        allocation_a.authenticated_receipt_uuid,
        allocation_a.structural_occurrence, allocation_a.import_occurrence,
        allocation_a.authority_evidence_sha256,
        [&](api::BulkImportStreamAllocation* out) { factory_called = true; *out = allocation_a; return true; });
    Require(first_allocation.ok && factory_called && registry.Allocate(allocation_b).ok,
            "two full UUID allocations failed");
    factory_called = false;
    const auto replay_allocation = registry.AllocateOrReplay(
        allocation_a.authenticated_receipt_uuid, allocation_a.structural_occurrence,
        allocation_a.import_occurrence, allocation_a.authority_evidence_sha256,
        [&](api::BulkImportStreamAllocation*) { factory_called = true; return false; });
    Require(replay_allocation.ok && replay_allocation.replayed && !factory_called &&
                replay_allocation.allocation.stream_uuid == allocation_a.stream_uuid,
            "in-process allocation replay called the factory");
    std::size_t artifacts_before_replay = 0;
    for (const auto& artifact : std::filesystem::directory_iterator(root)) {
      (void)artifact;
      ++artifacts_before_replay;
    }
    auto conflicting_authority = allocation_a.authority_evidence_sha256;
    conflicting_authority[0] ^= 1;
    factory_called = false;
    const auto conflicting_allocation = registry.AllocateOrReplay(
        allocation_a.authenticated_receipt_uuid, allocation_a.structural_occurrence,
        allocation_a.import_occurrence, conflicting_authority,
        [&](api::BulkImportStreamAllocation*) { factory_called = true; return false; });
    std::size_t artifacts_after_conflict = 0;
    for (const auto& artifact : std::filesystem::directory_iterator(root)) {
      (void)artifact;
      ++artifacts_after_conflict;
    }
    Require(!conflicting_allocation.ok &&
                conflicting_allocation.error == "allocation_authority_conflict" &&
                !factory_called && artifacts_before_replay == artifacts_after_conflict,
            "allocation authority conflict was not refused atomically");

    const auto h0 = wire::ChainStart(allocation_a.stream_uuid,
                                     allocation_a.stream_generation,
                                     allocation_a.descriptor_evidence);
    chunk_one = Chunk(allocation_a, 1, 0, h0, {1, 2, 3});
    const auto first = registry.Append(chunk_one);
    Require(first.ok && !first.replayed && !first.response_wire.empty(), "first chunk append failed");
    ack_one = first.response_wire;

    const auto chunk_two = Chunk(allocation_a, 2, 3, chunk_one.chain_sha, {4, 5});
    const auto second = registry.Append(chunk_two);
    Require(second.ok && !second.replayed, "second chunk append failed");
    const auto replay = registry.Append(chunk_one);
    Require(replay.ok && replay.replayed && replay.response_wire == ack_one,
            "historical duplicate did not replay byte-exact ACK");

    auto wrong_previous = Chunk(allocation_a, 3, 5, Hash("wrong-chain"), {6});
    Require(!registry.Append(wrong_previous).ok, "previous-chain drift was accepted");
    auto wrong_offset = Chunk(allocation_a, 3, 4, chunk_two.chain_sha, {6});
    Require(!registry.Append(wrong_offset).ok, "byte-offset drift was accepted");
    auto wrong_payload_hash = Chunk(allocation_a, 3, 5, chunk_two.chain_sha, {6});
    wrong_payload_hash.payload_sha[0] ^= 1;
    Require(!registry.Append(wrong_payload_hash).ok, "payload hash drift was accepted");
    auto wrong_chain = Chunk(allocation_a, 3, 5, chunk_two.chain_sha, {6});
    wrong_chain.chain_sha[0] ^= 1;
    Require(!registry.Append(wrong_chain).ok, "chunk chain drift was accepted");
    api::BulkImportStreamEntry unchanged_after_faults;
    Require(registry.Recover(allocation_a.stream_uuid, &unchanged_after_faults).ok &&
                unchanged_after_faults.received_chunks == 2 &&
                unchanged_after_faults.received_bytes == 5,
            "chunk fault refusal mutated durable state");

    const std::vector<std::uint8_t> all_payload{1, 2, 3, 4, 5};
    exact_seal = Seal(allocation_a, 2, 5, chunk_two.chain_sha, ContentHash(all_payload));
    Require(!registry.BeginExecution(allocation_a.stream_uuid,
                                     allocation_a.stream_generation).ok,
            "execution began before seal");
    auto wrong_content = exact_seal;
    wrong_content.content_sha[0] ^= 1;
    wire::Seal wrong_content_wire;
    wrong_content_wire.authenticated_receipt_uuid = wrong_content.authenticated_receipt_uuid;
    wrong_content_wire.stream_uuid = wrong_content.stream_uuid;
    wrong_content_wire.stream_generation = wrong_content.stream_generation;
    wrong_content_wire.descriptor_evidence = wrong_content.descriptor_evidence;
    wrong_content_wire.final_chunk_count = wrong_content.final_chunk_count;
    wrong_content_wire.total_stream_bytes = wrong_content.total_stream_bytes;
    wrong_content_wire.final_chain_sha256 = wrong_content.final_chain_sha;
    wrong_content_wire.content_sha256 = wrong_content.content_sha;
    wrong_content.seal_request_evidence = wire::SealRequestEvidence(wrong_content_wire);
    Require(!registry.Seal(wrong_content).ok, "incorrect content hash was accepted at seal");
    const auto sealed = registry.Seal(exact_seal);
    Require(sealed.ok && !sealed.replayed && !sealed.response_wire.empty(), "seal failed");
    seal_ack = sealed.response_wire;
    Require(registry.BeginExecution(allocation_a.stream_uuid, allocation_a.stream_generation).ok,
            "begin execution failed");
    std::vector<std::uint8_t> streamed_spool;
    std::size_t maximum_read_chunk = 0;
    api::BulkImportSealedSpoolSnapshot spool_snapshot;
    const auto streamed = registry.ReadSealedSpool(
        allocation_a.stream_uuid, allocation_a.stream_generation,
        allocation_a.authenticated_receipt_uuid, allocation_a.descriptor_evidence,
        [&](const std::uint8_t* bytes, std::size_t count, std::uint64_t offset) {
          Require(offset == streamed_spool.size(), "sealed spool offset was not contiguous");
          maximum_read_chunk = std::max(maximum_read_chunk, count);
          streamed_spool.insert(streamed_spool.end(), bytes, bytes + count);
          return true;
        }, &spool_snapshot);
    Require(streamed.ok && streamed_spool == std::vector<std::uint8_t>({1, 2, 3, 4, 5}) &&
                spool_snapshot.total_stream_bytes == streamed_spool.size() &&
                maximum_read_chunk <= 1024 * 1024,
            "sealed spool reader did not return exact bounded bytes");
    Require(!registry.ReadSealedSpool(
                 allocation_a.stream_uuid, allocation_a.stream_generation + 1,
                 allocation_a.authenticated_receipt_uuid, allocation_a.descriptor_evidence,
                 [](const std::uint8_t*, std::size_t, std::uint64_t) { return true; },
                 &spool_snapshot).ok,
            "sealed spool accepted a generation mismatch");
    Require(!registry.ReadSealedSpool(
                 allocation_b.stream_uuid, allocation_b.stream_generation,
                 allocation_b.authenticated_receipt_uuid, allocation_b.descriptor_evidence,
                 [](const std::uint8_t*, std::size_t, std::uint64_t) { return true; },
                 &spool_snapshot).ok,
            "unsealed spool was readable");
    Require(registry.Seal(exact_seal).ok && registry.Seal(exact_seal).response_wire == seal_ack,
            "seal replay during execution was not exact");
    api::BulkImportStreamEntry executing;
    Require(registry.Recover(allocation_a.stream_uuid, &executing).ok &&
                std::any_of(executing.recovery_key_sha256.begin(),
                            executing.recovery_key_sha256.end(),
                            [](std::uint8_t byte) { return byte != 0; }),
            "durable recovery key was not recorded");
    api::BulkImportExecutorEvidenceRecord premature_evidence;
    premature_evidence.stream_uuid = allocation_a.stream_uuid;
    premature_evidence.stream_generation = allocation_a.stream_generation;
    premature_evidence.executor_availability_generation =
        allocation_a.executor_availability_generation;
    premature_evidence.executor_evidence_sha256 = Hash("premature-evidence");
    Require(!registry.RecordEvidence(premature_evidence).ok,
            "executor evidence was recorded before publication");
    api::BulkImportPublicationRecord publication;
    publication.stream_uuid = allocation_a.stream_uuid;
    publication.stream_generation = allocation_a.stream_generation;
    publication.recovery_key_sha256 = executing.recovery_key_sha256;
    publication.durable_publication_uuid = Uuid(0xf1);
    publication.durable_publication_generation = 1;
    publication.affected_rows = 2;
    publication.postcondition_evidence_sha256 = Hash("publication-postcondition");
    Require(registry.Publish(publication).ok, "publication transition failed");
    const auto publication_replay = registry.Publish(publication);
    Require(publication_replay.ok && publication_replay.replayed,
            "exact publication replay was not idempotent");
    result_wire = ResultWire(allocation_a, publication, 5, 2, exact_seal.content_sha);
    Require(!registry.RecordResult(allocation_a.stream_uuid,
                                   allocation_a.stream_generation,
                                   result_wire).ok,
            "BIRS was recorded before executor evidence");
    api::BulkImportExecutorEvidenceRecord evidence;
    evidence.stream_uuid = allocation_a.stream_uuid;
    evidence.stream_generation = allocation_a.stream_generation;
    evidence.durable_publication_uuid = publication.durable_publication_uuid;
    evidence.durable_publication_generation = publication.durable_publication_generation;
    evidence.executor_availability_generation = allocation_a.executor_availability_generation;
    evidence.executor_evidence_sha256 = Hash("executor-evidence");
    Require(registry.RecordEvidence(evidence).ok, "evidence transition failed");
    const auto evidence_replay = registry.RecordEvidence(evidence);
    Require(evidence_replay.ok && evidence_replay.replayed,
            "exact executor evidence replay was not idempotent");
    const auto recorded = registry.RecordResult(allocation_a.stream_uuid,
                                                allocation_a.stream_generation,
                                                result_wire);
    Require(recorded.ok && recorded.response_wire == result_wire,
            "exact BIRS was not durably recorded");
    Require(registry.Seal(exact_seal).ok && registry.Seal(exact_seal).response_wire == seal_ack,
            "seal replay after result was not exact");
  }

  {
    api::SblrBulkImportStreamRegistry restarted(root);
    Require(restarted.healthy(), "allocation replay restart registry was not admitted");
    bool restart_factory_called = false;
    const auto replay = restarted.AllocateOrReplay(
        allocation_a.authenticated_receipt_uuid, allocation_a.structural_occurrence,
        allocation_a.import_occurrence, allocation_a.authority_evidence_sha256,
        [&](api::BulkImportStreamAllocation*) { restart_factory_called = true; return false; });
    Require(replay.ok && replay.replayed && !restart_factory_called &&
                replay.allocation.stream_uuid == allocation_a.stream_uuid,
            "restart allocation replay did not return the persisted allocation");
  }

  const auto meta_a = root / (base_a + ".meta");
  const auto spool_a = root / (base_a + ".spool");
  const auto journal_a = root / (base_a + ".journal");
  const auto committed_journal_bytes = std::filesystem::file_size(journal_a);
  FlipByte(meta_a, 20);
  AppendBytes(spool_a, {0xaa, 0xbb, 0xcc});
  AppendBytes(journal_a, {0xde, 0xad, 0xbe, 0xef, 0x01});

  {
    api::SblrBulkImportStreamRegistry restarted(root);
    Require(restarted.healthy(), "restart registry was not admitted");
    api::BulkImportStreamEntry recovered;
    const auto recovery = restarted.Recover(allocation_a.stream_uuid, &recovered);
    Require(recovery.ok && recovery.state == api::BulkImportStreamState::result_recorded &&
                recovery.response_wire == result_wire && recovered.result_wire == result_wire &&
                recovered.received_chunks == 2 && recovered.received_bytes == 5,
            "journal recovery did not reconstruct exact terminal state");
    Require(std::filesystem::file_size(spool_a) == 5,
            "uncommitted spool tail was not truncated");
    Require(std::filesystem::file_size(journal_a) == committed_journal_bytes,
            "torn journal tail was not truncated to the last durable record");
    const auto old_duplicate = restarted.Append(chunk_one);
    Require(old_duplicate.ok && old_duplicate.replayed && old_duplicate.response_wire == ack_one,
            "restart historical duplicate ACK was not byte exact");
    const auto replayed_seal = restarted.Seal(exact_seal);
    Require(replayed_seal.ok && replayed_seal.replayed && replayed_seal.response_wire == seal_ack,
            "restart seal ACK was not byte exact");
    const auto replayed_result = restarted.RecordResult(allocation_a.stream_uuid,
                                                        allocation_a.stream_generation,
                                                        result_wire);
    Require(replayed_result.ok && replayed_result.replayed &&
                replayed_result.response_wire == result_wire,
            "restart BIRS replay was not byte exact");
    auto conflicting_result = result_wire;
    conflicting_result[64] ^= 1;
    Require(!restarted.RecordResult(allocation_a.stream_uuid,
                                    allocation_a.stream_generation,
                                    conflicting_result).ok,
            "conflicting BIRS replay was accepted");
    api::BulkImportStreamEntry recovered_b;
    Require(restarted.Recover(allocation_b.stream_uuid, &recovered_b).ok &&
                recovered_b.allocation.stream_uuid == allocation_b.stream_uuid,
            "same-prefix full UUID stream did not survive restart");
  }

  const auto conflict_root = UniqueRoot();
  const auto conflict_allocation = Allocation(40);
  {
    api::SblrBulkImportStreamRegistry registry(conflict_root);
    Require(registry.healthy() && registry.Allocate(conflict_allocation).ok,
            "conflict fixture allocation failed");
    const auto h0 = wire::ChainStart(conflict_allocation.stream_uuid,
                                     conflict_allocation.stream_generation,
                                     conflict_allocation.descriptor_evidence);
    const auto original = Chunk(conflict_allocation, 1, 0, h0, {7, 8, 9});
    Require(registry.Append(original).ok, "conflict fixture append failed");
    auto conflicting = original;
    conflicting.payload.back() = 0;
    const auto refused = registry.Append(conflicting);
    Require(!refused.ok && refused.state == api::BulkImportStreamState::aborted && refused.durable,
            "conflicting duplicate did not durably abort");
    api::BulkImportStreamEntry aborted;
    Require(registry.Recover(conflict_allocation.stream_uuid, &aborted).ok &&
                aborted.state == api::BulkImportStreamState::aborted &&
                aborted.abort_reason == api::BulkImportStreamAbortReason::chunk_conflict &&
                aborted.received_bytes == 3,
            "aborted duplicate state was not recoverable");
  }
  {
    api::SblrBulkImportStreamRegistry restarted(conflict_root);
    api::BulkImportStreamEntry aborted;
    Require(restarted.Recover(conflict_allocation.stream_uuid, &aborted).ok &&
                aborted.state == api::BulkImportStreamState::aborted,
            "durable duplicate abort did not survive restart");
  }

  const auto seal_conflict_root = UniqueRoot();
  const auto seal_conflict_allocation = Allocation(50);
  {
    api::SblrBulkImportStreamRegistry registry(seal_conflict_root);
    Require(registry.healthy() && registry.Allocate(seal_conflict_allocation).ok,
            "seal-conflict fixture allocation failed");
    const auto h0 = wire::ChainStart(seal_conflict_allocation.stream_uuid,
                                     seal_conflict_allocation.stream_generation,
                                     seal_conflict_allocation.descriptor_evidence);
    const auto chunk = Chunk(seal_conflict_allocation, 1, 0, h0, {5, 6});
    Require(registry.Append(chunk).ok, "seal-conflict fixture append failed");
    const auto accepted_seal = Seal(seal_conflict_allocation,
                                    1,
                                    2,
                                    chunk.chain_sha,
                                    ContentHash({5, 6}));
    Require(registry.Seal(accepted_seal).ok, "seal-conflict fixture seal failed");
    auto conflicting_seal = accepted_seal;
    conflicting_seal.final_chunk_count = 2;
    wire::Seal conflicting_wire;
    conflicting_wire.authenticated_receipt_uuid = conflicting_seal.authenticated_receipt_uuid;
    conflicting_wire.stream_uuid = conflicting_seal.stream_uuid;
    conflicting_wire.stream_generation = conflicting_seal.stream_generation;
    conflicting_wire.descriptor_evidence = conflicting_seal.descriptor_evidence;
    conflicting_wire.final_chunk_count = conflicting_seal.final_chunk_count;
    conflicting_wire.total_stream_bytes = conflicting_seal.total_stream_bytes;
    conflicting_wire.final_chain_sha256 = conflicting_seal.final_chain_sha;
    conflicting_wire.content_sha256 = conflicting_seal.content_sha;
    conflicting_seal.seal_request_evidence = wire::SealRequestEvidence(conflicting_wire);
    const auto refused = registry.Seal(conflicting_seal);
    Require(!refused.ok && refused.durable &&
                refused.state == api::BulkImportStreamState::aborted,
            "conflicting seal did not durably abort");
  }
  {
    api::SblrBulkImportStreamRegistry restarted(seal_conflict_root);
    api::BulkImportStreamEntry aborted;
    Require(restarted.Recover(seal_conflict_allocation.stream_uuid, &aborted).ok &&
                aborted.state == api::BulkImportStreamState::aborted &&
                aborted.abort_reason == api::BulkImportStreamAbortReason::seal_conflict,
            "durable seal conflict did not survive restart");
  }

  const auto limit_root = UniqueRoot();
  {
    auto limited = Allocation(60, 3);
    api::SblrBulkImportStreamRegistry registry(limit_root);
    Require(registry.healthy() && registry.Allocate(limited).ok, "limit fixture allocation failed");
    const auto h0 = wire::ChainStart(limited.stream_uuid,
                                     limited.stream_generation,
                                     limited.descriptor_evidence);
    Require(!registry.Append(Chunk(limited, 1, 0, h0, {1, 2, 3, 4})).ok,
            "effective stream-byte limit was not enforced");
    api::BulkImportStreamEntry unchanged;
    Require(registry.Recover(limited.stream_uuid, &unchanged).ok && unchanged.received_bytes == 0,
            "limit refusal mutated durable stream state");

    auto invalid_stream_limit = Allocation(61);
    invalid_stream_limit.effective_maximum_stream_bytes =
        api::kBulkImportStreamMaximumBytesV1 + 1;
    Require(!registry.Allocate(invalid_stream_limit).ok,
            "above-absolute stream limit was admitted");
    auto invalid_chunk_count = Allocation(62);
    invalid_chunk_count.effective_maximum_chunk_count =
        api::kBulkImportStreamMaximumChunksV1 + 1;
    Require(!registry.Allocate(invalid_chunk_count).ok,
            "above-absolute chunk count was admitted");
    auto invalid_chunk_bytes = Allocation(63);
    invalid_chunk_bytes.effective_maximum_chunk_bytes =
        api::kBulkImportStreamMaximumChunkBytesV1 + 1;
    Require(!registry.Allocate(invalid_chunk_bytes).ok,
            "above-absolute chunk size was admitted");
    auto invalid_rows = Allocation(64);
    invalid_rows.effective_maximum_rows = api::kBulkImportStreamMaximumRowsV1 + 1;
    Require(!registry.Allocate(invalid_rows).ok,
            "above-absolute row count was admitted");
    auto invalid_columns = Allocation(65);
    invalid_columns.effective_maximum_target_columns = 65536;
    Require(!registry.Allocate(invalid_columns).ok,
            "above-absolute target-column count was admitted");

    auto chunk_limited = Allocation(66, 8);
    chunk_limited.effective_maximum_chunk_bytes = 2;
    Require(registry.Allocate(chunk_limited).ok, "chunk-size fixture allocation failed");
    const auto chunk_limited_h0 = wire::ChainStart(chunk_limited.stream_uuid,
                                                   chunk_limited.stream_generation,
                                                   chunk_limited.descriptor_evidence);
    Require(!registry.Append(Chunk(chunk_limited, 1, 0, chunk_limited_h0, {1, 2, 3})).ok,
            "effective chunk-size limit was not enforced");

    auto count_limited = Allocation(67, 8);
    count_limited.effective_maximum_chunk_count = 1;
    Require(registry.Allocate(count_limited).ok, "chunk-count fixture allocation failed");
    const auto count_h0 = wire::ChainStart(count_limited.stream_uuid,
                                           count_limited.stream_generation,
                                           count_limited.descriptor_evidence);
    const auto count_one = Chunk(count_limited, 1, 0, count_h0, {1});
    Require(registry.Append(count_one).ok, "chunk-count first append failed");
    Require(!registry.Append(Chunk(count_limited, 2, 1, count_one.chain_sha, {2})).ok,
            "effective chunk-count limit was not enforced");

    auto overflow_limited = Allocation(68, 8);
    Require(registry.Allocate(overflow_limited).ok, "overflow fixture allocation failed");
    const auto overflow_h0 = wire::ChainStart(overflow_limited.stream_uuid,
                                              overflow_limited.stream_generation,
                                              overflow_limited.descriptor_evidence);
    Require(!registry.Append(Chunk(overflow_limited,
                                   1,
                                   std::numeric_limits<std::uint64_t>::max(),
                                   overflow_h0,
                                   {1})).ok,
            "overflowing byte offset was admitted");

    auto row_limited = Allocation(69, 8);
    row_limited.effective_maximum_rows = 1;
    Require(registry.Allocate(row_limited).ok, "row-limit fixture allocation failed");
    const auto row_h0 = wire::ChainStart(row_limited.stream_uuid,
                                         row_limited.stream_generation,
                                         row_limited.descriptor_evidence);
    const auto row_chunk = Chunk(row_limited, 1, 0, row_h0, {9});
    Require(registry.Append(row_chunk).ok, "row-limit fixture append failed");
    const auto row_seal = Seal(row_limited,
                               1,
                               1,
                               row_chunk.chain_sha,
                               ContentHash({9}));
    Require(registry.Seal(row_seal).ok &&
                registry.BeginExecution(row_limited.stream_uuid,
                                        row_limited.stream_generation).ok,
            "row-limit fixture did not reach execution");
    api::BulkImportStreamEntry row_executing;
    Require(registry.Recover(row_limited.stream_uuid, &row_executing).ok,
            "row-limit recovery record unavailable");
    api::BulkImportPublicationRecord excessive_rows;
    excessive_rows.stream_uuid = row_limited.stream_uuid;
    excessive_rows.stream_generation = row_limited.stream_generation;
    excessive_rows.recovery_key_sha256 = row_executing.recovery_key_sha256;
    excessive_rows.durable_publication_uuid = Uuid(0xe1);
    excessive_rows.durable_publication_generation = 1;
    excessive_rows.affected_rows = 2;
    excessive_rows.postcondition_evidence_sha256 = Hash("row-limit-postcondition");
    Require(!registry.Publish(excessive_rows).ok,
            "effective publication row limit was not enforced");
    excessive_rows.affected_rows = 1;
    Require(registry.Publish(excessive_rows).ok,
            "publication at the effective row limit was refused");
  }

  const auto corruption_root = UniqueRoot();
  const auto corruption_allocation = Allocation(80);
  {
    api::SblrBulkImportStreamRegistry registry(corruption_root);
    Require(registry.healthy() && registry.Allocate(corruption_allocation).ok,
            "corruption fixture allocation failed");
    const auto h0 = wire::ChainStart(corruption_allocation.stream_uuid,
                                     corruption_allocation.stream_generation,
                                     corruption_allocation.descriptor_evidence);
    Require(registry.Append(Chunk(corruption_allocation, 1, 0, h0, {1})).ok,
            "corruption fixture append failed");
  }
  const auto corrupt_journal = corruption_root /
      ("stream_" + Hex(corruption_allocation.stream_uuid) + ".journal");
  FlipByte(corrupt_journal, 40);
  {
    api::SblrBulkImportStreamRegistry restarted(corruption_root);
    api::BulkImportStreamEntry ignored;
    Require(!restarted.Recover(corruption_allocation.stream_uuid, &ignored).ok,
            "interior journal corruption was accepted");
  }

  const auto final_checksum_root = UniqueRoot();
  const auto final_checksum_allocation = Allocation(90);
  {
    api::SblrBulkImportStreamRegistry registry(final_checksum_root);
    Require(registry.healthy() && registry.Allocate(final_checksum_allocation).ok,
            "final-checksum fixture allocation failed");
    const auto h0 = wire::ChainStart(final_checksum_allocation.stream_uuid,
                                     final_checksum_allocation.stream_generation,
                                     final_checksum_allocation.descriptor_evidence);
    Require(registry.Append(Chunk(final_checksum_allocation, 1, 0, h0, {1, 2})).ok,
            "final-checksum fixture append failed");
  }
  const auto final_checksum_journal = final_checksum_root /
      ("stream_" + Hex(final_checksum_allocation.stream_uuid) + ".journal");
  FlipByte(final_checksum_journal, std::filesystem::file_size(final_checksum_journal) - 1);
  {
    api::SblrBulkImportStreamRegistry restarted(final_checksum_root);
    api::BulkImportStreamEntry ignored;
    Require(!restarted.Recover(final_checksum_allocation.stream_uuid, &ignored).ok,
            "complete final record checksum corruption was treated as a torn tail");
  }

  const auto spool_corruption_root = UniqueRoot();
  const auto spool_corruption_allocation = Allocation(100);
  {
    api::SblrBulkImportStreamRegistry registry(spool_corruption_root);
    Require(registry.healthy() && registry.Allocate(spool_corruption_allocation).ok,
            "spool-corruption fixture allocation failed");
    const auto h0 = wire::ChainStart(spool_corruption_allocation.stream_uuid,
                                     spool_corruption_allocation.stream_generation,
                                     spool_corruption_allocation.descriptor_evidence);
    Require(registry.Append(Chunk(spool_corruption_allocation, 1, 0, h0, {1, 2})).ok,
            "spool-corruption fixture append failed");
  }
  const auto corrupt_spool = spool_corruption_root /
      ("stream_" + Hex(spool_corruption_allocation.stream_uuid) + ".spool");
  std::filesystem::resize_file(corrupt_spool, 1);
  {
    api::SblrBulkImportStreamRegistry restarted(spool_corruption_root);
    api::BulkImportStreamEntry ignored;
    Require(!restarted.Recover(spool_corruption_allocation.stream_uuid, &ignored).ok,
            "truncated durable spool was accepted");
  }

  const auto crash_root = UniqueRoot();
  const auto crash_allocation = Allocation(110);
  const auto crash_h0 = wire::ChainStart(crash_allocation.stream_uuid,
                                         crash_allocation.stream_generation,
                                         crash_allocation.descriptor_evidence);
  const auto crash_chunk = Chunk(crash_allocation, 1, 0, crash_h0, {3, 4, 5});
  const auto crash_seal = Seal(crash_allocation,
                               1,
                               3,
                               crash_chunk.chain_sha,
                               ContentHash({3, 4, 5}));
  std::vector<std::uint8_t> crash_chunk_ack;
  std::vector<std::uint8_t> crash_seal_ack;
  {
    api::SblrBulkImportStreamRegistry registry(crash_root);
    Require(registry.healthy() && registry.Allocate(crash_allocation).ok,
            "crash fixture allocation failed");
    const auto appended = registry.Append(crash_chunk);
    Require(appended.ok, "crash fixture append failed");
    crash_chunk_ack = appended.response_wire;
  }
  {
    api::SblrBulkImportStreamRegistry restarted(crash_root);
    api::BulkImportStreamEntry receiving;
    Require(restarted.Recover(crash_allocation.stream_uuid, &receiving).ok &&
                receiving.state == api::BulkImportStreamState::receiving,
            "post-chunk recovery did not restore receiving state");
    const auto replay = restarted.Append(crash_chunk);
    Require(replay.ok && replay.replayed && replay.response_wire == crash_chunk_ack,
            "post-chunk recovery did not replay the exact ACK");
    const auto sealed = restarted.Seal(crash_seal);
    Require(sealed.ok, "post-chunk recovery could not seal");
    crash_seal_ack = sealed.response_wire;
  }
  sblr::BulkImportSha crash_recovery_key{};
  {
    api::SblrBulkImportStreamRegistry restarted(crash_root);
    api::BulkImportStreamEntry sealed;
    Require(restarted.Recover(crash_allocation.stream_uuid, &sealed).ok &&
                sealed.state == api::BulkImportStreamState::sealed,
            "post-seal recovery did not restore sealed state");
    const auto replay = restarted.Seal(crash_seal);
    Require(replay.ok && replay.replayed && replay.response_wire == crash_seal_ack,
            "post-seal recovery did not replay the exact ACK");
    Require(restarted.BeginExecution(crash_allocation.stream_uuid,
                                     crash_allocation.stream_generation).ok,
            "post-seal recovery could not begin execution");
    api::BulkImportStreamEntry executing;
    Require(restarted.Recover(crash_allocation.stream_uuid, &executing).ok,
            "executing recovery record was unavailable");
    crash_recovery_key = executing.recovery_key_sha256;
  }
  {
    api::SblrBulkImportStreamRegistry restarted(crash_root);
    api::BulkImportStreamEntry executing;
    Require(restarted.Recover(crash_allocation.stream_uuid, &executing).ok &&
                executing.state == api::BulkImportStreamState::executing &&
                executing.recovery_key_sha256 == crash_recovery_key,
            "pre-publication execution recovery was not exact");
    const auto replay = restarted.BeginExecution(crash_allocation.stream_uuid,
                                                  crash_allocation.stream_generation);
    Require(replay.ok && replay.replayed,
            "execution recovery record was not idempotent");
  }

  const auto orphan_root = UniqueRoot();
  const auto orphan_allocation = Allocation(120);
  {
    api::SblrBulkImportStreamRegistry registry(orphan_root);
    Require(registry.healthy(), "orphan fixture registry was not admitted");
    const auto base = "stream_" + Hex(orphan_allocation.stream_uuid);
    Touch(orphan_root / (base + ".spool"));
    Touch(orphan_root / (base + ".journal"));
    Require(registry.Allocate(orphan_allocation).ok,
            "validated zero-length crash orphans blocked exact allocation retry");
  }

  const auto orphan_meta_root = UniqueRoot();
  const auto orphan_meta_allocation = Allocation(130);
  {
    api::SblrBulkImportStreamRegistry registry(orphan_meta_root);
    Require(registry.healthy(), "orphan-metadata fixture registry was not admitted");
    const auto base = "stream_" + Hex(orphan_meta_allocation.stream_uuid);
    Touch(orphan_meta_root / (base + ".meta"));
    Require(!registry.Allocate(orphan_meta_allocation).ok,
            "unverifiable orphan metadata was overwritten");
  }

  const auto concurrent_root = UniqueRoot();
  const auto concurrent_allocation = Allocation(140);
  {
    api::SblrBulkImportStreamRegistry registry(concurrent_root);
    Require(registry.healthy() && registry.Allocate(concurrent_allocation).ok,
            "concurrency fixture allocation failed");
    const auto h0 = wire::ChainStart(concurrent_allocation.stream_uuid,
                                     concurrent_allocation.stream_generation,
                                     concurrent_allocation.descriptor_evidence);
    const auto chunk = Chunk(concurrent_allocation, 1, 0, h0, {8, 9});
    std::array<api::BulkImportStreamRegistryResult, 8> results;
    std::array<std::thread, 8> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) {
      threads[i] = std::thread([&, i] { results[i] = registry.Append(chunk); });
    }
    for (auto& thread : threads) thread.join();
    const auto fresh = std::count_if(results.begin(), results.end(), [](const auto& result) {
      return result.ok && !result.replayed;
    });
    const auto replayed = std::count_if(results.begin(), results.end(), [](const auto& result) {
      return result.ok && result.replayed;
    });
    Require(fresh == 1 && replayed == 7,
            "concurrent exact append was not serialized into one mutation and seven replays");
    api::BulkImportStreamEntry recovered;
    Require(registry.Recover(concurrent_allocation.stream_uuid, &recovered).ok &&
                recovered.received_chunks == 1 && recovered.received_bytes == 2,
            "concurrent exact append duplicated durable bytes");
  }

  const auto real_root = UniqueRoot();
  const auto symlink_root = real_root.parent_path() /
      (real_root.filename().string() + "_link");
  std::error_code error;
  std::filesystem::create_directory_symlink(real_root, symlink_root, error);
  Require(!error, "root symlink fixture failed");
  {
    api::SblrBulkImportStreamRegistry rejected(symlink_root);
    Require(!rejected.healthy() && rejected.startup_error() == "root_symlink_forbidden",
            "symlink root was admitted");
  }

  const auto broad_root = UniqueRoot();
  Require(::chmod(broad_root.c_str(), 0755) == 0,
          "broad-permission root fixture failed");
  {
    api::SblrBulkImportStreamRegistry rejected(broad_root);
    Require(!rejected.healthy() &&
                rejected.startup_error() == "root_permissions_invalid",
            "non-private registry root was admitted");
  }
  Require(::chmod(broad_root.c_str(), 0700) == 0,
          "broad-permission root cleanup failed");

  const auto broad_lock_root = UniqueRoot();
  Touch(broad_lock_root / "LOCK");
  Require(::chmod((broad_lock_root / "LOCK").c_str(), 0644) == 0,
          "broad-permission lock fixture failed");
  {
    api::SblrBulkImportStreamRegistry rejected(broad_lock_root);
    Require(!rejected.healthy() && rejected.startup_error() == "root_lock_invalid",
            "non-private registry lock was admitted");
  }

  const auto child_symlink_root = UniqueRoot();
  const auto child_symlink_allocation = Allocation(150);
  const auto child_target = child_symlink_root.parent_path() /
      (child_symlink_root.filename().string() + "_target");
  Touch(child_target);
  {
    api::SblrBulkImportStreamRegistry registry(child_symlink_root);
    Require(registry.healthy(), "child-symlink fixture registry was not admitted");
    const auto spool_name = "stream_" + Hex(child_symlink_allocation.stream_uuid) + ".spool";
    std::filesystem::create_symlink(child_target, child_symlink_root / spool_name, error);
    Require(!error, "child symlink fixture failed");
    Require(!registry.Allocate(child_symlink_allocation).ok,
            "symlink stream artifact was admitted");
    Require(std::filesystem::file_size(child_target) == 0,
            "symlink target was modified during refusal");
  }

  std::filesystem::remove_all(root, error);
  std::filesystem::remove_all(conflict_root, error);
  std::filesystem::remove_all(seal_conflict_root, error);
  std::filesystem::remove_all(limit_root, error);
  std::filesystem::remove_all(corruption_root, error);
  std::filesystem::remove_all(final_checksum_root, error);
  std::filesystem::remove_all(spool_corruption_root, error);
  std::filesystem::remove_all(crash_root, error);
  std::filesystem::remove_all(orphan_root, error);
  std::filesystem::remove_all(orphan_meta_root, error);
  std::filesystem::remove_all(concurrent_root, error);
  std::filesystem::remove(symlink_root, error);
  std::filesystem::remove_all(real_root, error);
  std::filesystem::remove_all(broad_root, error);
  std::filesystem::remove_all(broad_lock_root, error);
  std::filesystem::remove_all(child_symlink_root, error);
  std::filesystem::remove(child_target, error);
  return EXIT_SUCCESS;
}
