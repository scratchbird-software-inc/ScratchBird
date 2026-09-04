#include "sblr_bulk_import_stream_registry.hpp"

#include "core/hash/hash_digest.hpp"
#include "wire/parser_server_ipc/sbps_bulk_import_stream_codec.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace scratchbird::engine::internal_api {
namespace {

using Uuid = engine::sblr::BulkImportUuid;
using Sha = engine::sblr::BulkImportSha;
namespace bulk_wire = scratchbird::wire::sbps_bulk_import;

constexpr std::array<std::uint8_t, 8> kJournalMagic = {'S', 'B', 'B', 'S', 'J', '0', '0', '2'};
constexpr std::array<std::uint8_t, 8> kMetadataMagic = {'S', 'B', 'B', 'M', 'E', 'T', 'A', '2'};
constexpr std::uint16_t kFormatVersion = 2;
constexpr std::size_t kJournalHeaderBytes = 28;
constexpr std::size_t kJournalChecksumBytes = 32;
constexpr std::size_t kMaximumJournalRecordBytes = 1024 * 1024;
constexpr std::size_t kResultWireBytes = 192;

enum class JournalRecordType : std::uint8_t {
  allocation = 1,
  chunk = 2,
  seal = 3,
  transition = 4,
  abort = 5,
  result = 6,
};

struct ByteWriter {
  std::vector<std::uint8_t> bytes;

  void U8(std::uint8_t value) { bytes.push_back(value); }
  void U16(std::uint16_t value) {
    U8(static_cast<std::uint8_t>(value));
    U8(static_cast<std::uint8_t>(value >> 8));
  }
  void U32(std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) U8(static_cast<std::uint8_t>(value >> (i * 8)));
  }
  void U64(std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) U8(static_cast<std::uint8_t>(value >> (i * 8)));
  }
  template <std::size_t N>
  void Fixed(const std::array<std::uint8_t, N>& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  void Raw(const std::vector<std::uint8_t>& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
};

struct ByteReader {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t offset = 0;

  bool U8(std::uint8_t* value) {
    if (!value || offset + 1 > size) return false;
    *value = data[offset++];
    return true;
  }
  bool U16(std::uint16_t* value) {
    if (!value || offset + 2 > size) return false;
    *value = static_cast<std::uint16_t>(data[offset]) |
             (static_cast<std::uint16_t>(data[offset + 1]) << 8);
    offset += 2;
    return true;
  }
  bool U32(std::uint32_t* value) {
    if (!value || offset + 4 > size) return false;
    *value = 0;
    for (std::size_t i = 0; i < 4; ++i) *value |= static_cast<std::uint32_t>(data[offset + i]) << (i * 8);
    offset += 4;
    return true;
  }
  bool U64(std::uint64_t* value) {
    if (!value || offset + 8 > size) return false;
    *value = 0;
    for (std::size_t i = 0; i < 8; ++i) *value |= static_cast<std::uint64_t>(data[offset + i]) << (i * 8);
    offset += 8;
    return true;
  }
  template <std::size_t N>
  bool Fixed(std::array<std::uint8_t, N>* value) {
    if (!value || offset + N > size) return false;
    std::copy_n(data + offset, N, value->begin());
    offset += N;
    return true;
  }
  bool Raw(std::size_t count, std::vector<std::uint8_t>* value) {
    if (!value || offset + count > size) return false;
    value->assign(data + offset, data + offset + count);
    offset += count;
    return true;
  }
  bool Done() const { return offset == size; }
};

bool NonZero(const Uuid& value) {
  return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte != 0; });
}

bool NonZero(const Sha& value) {
  return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte != 0; });
}

std::uint64_t ReadLe64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  return value;
}

std::string HexUuid(const Uuid& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(32);
  for (const auto byte : value) {
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 0x0f]);
  }
  return text;
}

Sha Digest(const std::vector<std::uint8_t>& bytes) {
  return core::hash::ComputeSha256Digest(bytes).digest;
}

bool WriteAll(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const auto count = ::write(fd, data + written, size - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    written += static_cast<std::size_t>(count);
  }
  return true;
}

bool PwriteAll(int fd, const std::uint8_t* data, std::size_t size, std::uint64_t offset) {
  std::size_t written = 0;
  while (written < size) {
    const auto count = ::pwrite(fd,
                                data + written,
                                size - written,
                                static_cast<off_t>(offset + written));
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    written += static_cast<std::size_t>(count);
  }
  return true;
}

bool PreadAll(int fd, std::uint8_t* data, std::size_t size, std::uint64_t offset) {
  std::size_t read = 0;
  while (read < size) {
    const auto count = ::pread(fd,
                              data + read,
                              size - read,
                              static_cast<off_t>(offset + read));
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    read += static_cast<std::size_t>(count);
  }
  return true;
}

bool RegularFd(int fd, std::uint64_t* size = nullptr) {
  struct stat status {};
  if (fd < 0 || ::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1 || status.st_uid != ::geteuid() ||
      (status.st_mode & 0777) != 0600) {
    return false;
  }
  if (status.st_size < 0) return false;
  if (size) *size = static_cast<std::uint64_t>(status.st_size);
  return true;
}

bool PrivateDirectoryFd(int fd) {
  struct stat status {};
  return fd >= 0 && ::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode) &&
         status.st_uid == ::geteuid() && (status.st_mode & 0777) == 0700;
}

BulkImportStreamRegistryResult Failure(BulkImportStreamState state, std::string error) {
  BulkImportStreamRegistryResult result;
  result.state = state;
  result.error = std::move(error);
  return result;
}

BulkImportStreamRegistryResult Success(BulkImportStreamState state,
                                       std::vector<std::uint8_t> response = {},
                                       bool replayed = false) {
  BulkImportStreamRegistryResult result;
  result.ok = true;
  result.state = state;
  result.response_wire = std::move(response);
  result.replayed = replayed;
  result.durable = true;
  return result;
}

bool NormalState(BulkImportStreamState state) {
  return state <= BulkImportStreamState::result_recorded;
}

bool ParseHexUuid(std::string_view text, Uuid* out) {
  if (!out || text.size() != 32) return false;
  auto nibble=[](char c)->int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
  for (std::size_t i=0;i<16;++i) { const int hi=nibble(text[i*2]), lo=nibble(text[i*2+1]); if(hi<0||lo<0)return false; (*out)[i]=static_cast<std::uint8_t>((hi<<4)|lo); }
  return true;
}

std::uint8_t NormalStateOrdinal(BulkImportStreamState state) {
  return static_cast<std::uint8_t>(state);
}

bool AllocationValid(const BulkImportStreamAllocation& value) {
  return NonZero(value.authenticated_receipt_uuid) &&
         NonZero(value.stream_uuid) &&
         value.stream_generation != 0 &&
         value.structural_occurrence != 0 &&
         value.import_occurrence != 0 &&
         NonZero(value.descriptor_evidence) && NonZero(value.authority_evidence_sha256) &&
         NonZero(value.syntax_demand_sha256) &&
         NonZero(value.durable_spool_uuid) &&
         value.durable_spool_generation != 0 &&
         NonZero(value.target_relation_uuid) && value.target_relation_generation != 0 &&
         NonZero(value.owning_transaction_uuid) &&
         value.owning_local_transaction_id != 0 &&
         NonZero(value.statement_snapshot_uuid) && NonZero(value.catalog_epoch_uuid) &&
         value.catalog_generation != 0 && NonZero(value.security_context_uuid) &&
         value.security_epoch != 0 && NonZero(value.policy_snapshot_uuid) &&
         value.policy_generation != 0 && NonZero(value.route_snapshot_uuid) &&
         value.route_generation != 0 && NonZero(value.recovery_operation_uuid) &&
         value.recovery_generation != 0 && NonZero(value.row_shape_uuid) &&
         value.row_shape_generation != 0 && NonZero(value.column_descriptor_set_sha256) &&
         NonZero(value.import_policy_bundle_sha256) && NonZero(value.resource_grant_uuid) &&
         value.resource_grant_generation != 0 &&
         (value.cluster_bound
              ? value.cluster_epoch != 0 && NonZero(value.cluster_fence_uuid)
              : value.cluster_epoch == 0 && !NonZero(value.cluster_fence_uuid)) &&
         value.executor_availability_generation != 0 &&
         value.effective_maximum_stream_bytes != 0 &&
         value.effective_maximum_stream_bytes <= kBulkImportStreamMaximumBytesV1 &&
         value.effective_maximum_chunk_count != 0 &&
         value.effective_maximum_chunk_count <= kBulkImportStreamMaximumChunksV1 &&
         value.effective_maximum_chunk_bytes != 0 &&
         value.effective_maximum_chunk_bytes <= kBulkImportStreamMaximumChunkBytesV1 &&
         value.effective_maximum_rows != 0 &&
         value.effective_maximum_rows <= kBulkImportStreamMaximumRowsV1 &&
         value.effective_maximum_target_columns != 0 &&
         value.effective_maximum_target_columns <= 65535u;
}

bool SameAllocation(const BulkImportStreamAllocation& left,
                    const BulkImportStreamAllocation& right) {
  return left.authenticated_receipt_uuid == right.authenticated_receipt_uuid &&
         left.stream_uuid == right.stream_uuid &&
         left.stream_generation == right.stream_generation &&
         left.structural_occurrence == right.structural_occurrence &&
         left.import_occurrence == right.import_occurrence &&
         left.descriptor_evidence == right.descriptor_evidence &&
         left.authority_evidence_sha256 == right.authority_evidence_sha256 &&
         left.syntax_demand_sha256 == right.syntax_demand_sha256 &&
         left.durable_spool_uuid == right.durable_spool_uuid &&
         left.durable_spool_generation == right.durable_spool_generation &&
         left.target_relation_uuid == right.target_relation_uuid &&
         left.target_relation_generation == right.target_relation_generation &&
         left.owning_transaction_uuid == right.owning_transaction_uuid &&
         left.owning_local_transaction_id == right.owning_local_transaction_id &&
         left.statement_snapshot_uuid == right.statement_snapshot_uuid &&
         left.catalog_epoch_uuid == right.catalog_epoch_uuid &&
         left.catalog_generation == right.catalog_generation &&
         left.security_context_uuid == right.security_context_uuid &&
         left.security_epoch == right.security_epoch &&
         left.policy_snapshot_uuid == right.policy_snapshot_uuid &&
         left.policy_generation == right.policy_generation &&
         left.route_snapshot_uuid == right.route_snapshot_uuid &&
         left.route_generation == right.route_generation &&
         left.recovery_operation_uuid == right.recovery_operation_uuid &&
         left.recovery_generation == right.recovery_generation &&
         left.row_shape_uuid == right.row_shape_uuid &&
         left.row_shape_generation == right.row_shape_generation &&
         left.column_descriptor_set_sha256 == right.column_descriptor_set_sha256 &&
         left.import_policy_bundle_sha256 == right.import_policy_bundle_sha256 &&
         left.resource_grant_uuid == right.resource_grant_uuid &&
         left.resource_grant_generation == right.resource_grant_generation &&
         left.cluster_bound == right.cluster_bound &&
         left.cluster_epoch == right.cluster_epoch &&
         left.cluster_fence_uuid == right.cluster_fence_uuid &&
         left.executor_availability_generation == right.executor_availability_generation &&
         left.effective_maximum_stream_bytes == right.effective_maximum_stream_bytes &&
         left.effective_maximum_chunk_count == right.effective_maximum_chunk_count &&
         left.effective_maximum_chunk_bytes == right.effective_maximum_chunk_bytes &&
         left.effective_maximum_rows == right.effective_maximum_rows &&
         left.effective_maximum_target_columns == right.effective_maximum_target_columns;
}

std::vector<std::uint8_t> EncodeAllocation(const BulkImportStreamAllocation& value) {
  ByteWriter out;
  out.Fixed(value.authenticated_receipt_uuid);
  out.Fixed(value.stream_uuid);
  out.U64(value.stream_generation);
  out.U64(value.structural_occurrence);
  out.U32(value.import_occurrence);
  out.Fixed(value.descriptor_evidence);
  out.Fixed(value.authority_evidence_sha256);
  out.Fixed(value.syntax_demand_sha256);
  out.Fixed(value.durable_spool_uuid);
  out.U64(value.durable_spool_generation);
  out.Fixed(value.target_relation_uuid);
  out.U64(value.target_relation_generation);
  out.Fixed(value.owning_transaction_uuid);
  out.U64(value.owning_local_transaction_id);
  out.Fixed(value.statement_snapshot_uuid);
  out.Fixed(value.catalog_epoch_uuid);
  out.U64(value.catalog_generation);
  out.Fixed(value.security_context_uuid);
  out.U64(value.security_epoch);
  out.Fixed(value.policy_snapshot_uuid);
  out.U64(value.policy_generation);
  out.Fixed(value.route_snapshot_uuid);
  out.U64(value.route_generation);
  out.Fixed(value.recovery_operation_uuid);
  out.U64(value.recovery_generation);
  out.Fixed(value.row_shape_uuid);
  out.U64(value.row_shape_generation);
  out.Fixed(value.column_descriptor_set_sha256);
  out.Fixed(value.import_policy_bundle_sha256);
  out.Fixed(value.resource_grant_uuid);
  out.U64(value.resource_grant_generation);
  out.U8(value.cluster_bound ? 1 : 0);
  for (std::size_t i = 0; i < 7; ++i) out.U8(0);
  out.U64(value.cluster_epoch);
  out.Fixed(value.cluster_fence_uuid);
  out.U64(value.executor_availability_generation);
  out.U64(value.effective_maximum_stream_bytes);
  out.U64(value.effective_maximum_chunk_count);
  out.U32(value.effective_maximum_chunk_bytes);
  out.U64(value.effective_maximum_rows);
  out.U32(value.effective_maximum_target_columns);
  return std::move(out.bytes);
}

bool DecodeAllocation(const std::uint8_t* data,
                      std::size_t size,
                      BulkImportStreamAllocation* value) {
  if (!value) return false;
  ByteReader in{data, size};
  BulkImportStreamAllocation decoded;
  std::uint8_t cluster_bound = 0;
  std::array<std::uint8_t, 7> cluster_reserved{};
  std::array<std::uint8_t, 7> zero{};
  if (!in.Fixed(&decoded.authenticated_receipt_uuid) ||
      !in.Fixed(&decoded.stream_uuid) ||
      !in.U64(&decoded.stream_generation) ||
      !in.U64(&decoded.structural_occurrence) ||
      !in.U32(&decoded.import_occurrence) ||
      !in.Fixed(&decoded.descriptor_evidence) ||
      !in.Fixed(&decoded.authority_evidence_sha256) ||
      !in.Fixed(&decoded.syntax_demand_sha256) ||
      !in.Fixed(&decoded.durable_spool_uuid) ||
      !in.U64(&decoded.durable_spool_generation) ||
      !in.Fixed(&decoded.target_relation_uuid) ||
      !in.U64(&decoded.target_relation_generation) ||
      !in.Fixed(&decoded.owning_transaction_uuid) ||
      !in.U64(&decoded.owning_local_transaction_id) ||
      !in.Fixed(&decoded.statement_snapshot_uuid) ||
      !in.Fixed(&decoded.catalog_epoch_uuid) ||
      !in.U64(&decoded.catalog_generation) ||
      !in.Fixed(&decoded.security_context_uuid) ||
      !in.U64(&decoded.security_epoch) ||
      !in.Fixed(&decoded.policy_snapshot_uuid) ||
      !in.U64(&decoded.policy_generation) ||
      !in.Fixed(&decoded.route_snapshot_uuid) ||
      !in.U64(&decoded.route_generation) ||
      !in.Fixed(&decoded.recovery_operation_uuid) ||
      !in.U64(&decoded.recovery_generation) ||
      !in.Fixed(&decoded.row_shape_uuid) ||
      !in.U64(&decoded.row_shape_generation) ||
      !in.Fixed(&decoded.column_descriptor_set_sha256) ||
      !in.Fixed(&decoded.import_policy_bundle_sha256) ||
      !in.Fixed(&decoded.resource_grant_uuid) ||
      !in.U64(&decoded.resource_grant_generation) ||
      !in.U8(&cluster_bound) || cluster_bound > 1 ||
      !in.Fixed(&cluster_reserved) || cluster_reserved != zero ||
      !in.U64(&decoded.cluster_epoch) ||
      !in.Fixed(&decoded.cluster_fence_uuid) ||
      !in.U64(&decoded.executor_availability_generation) ||
      !in.U64(&decoded.effective_maximum_stream_bytes) ||
      !in.U64(&decoded.effective_maximum_chunk_count) ||
      !in.U32(&decoded.effective_maximum_chunk_bytes) ||
      !in.U64(&decoded.effective_maximum_rows) ||
      !in.U32(&decoded.effective_maximum_target_columns) ||
      !in.Done()) {
    return false;
  }
  decoded.cluster_bound = cluster_bound != 0;
  if (!AllocationValid(decoded)) return false;
  *value = decoded;
  return true;
}

bool ValidResultWire(const BulkImportStreamEntry& entry,
                     const std::vector<std::uint8_t>& wire) {
  if (wire.size() != kResultWireBytes) return false;
  engine::sblr::SblrBulkImportStreamResultV1 decoded;
  if (!engine::sblr::DecodeSblrBulkImportStreamResultV1(
          wire.data(), wire.size(), &decoded, nullptr)) {
    return false;
  }
  if (engine::sblr::EncodeSblrBulkImportStreamResultV1(decoded) != wire) return false;
  const auto& allocation = entry.allocation;
  if (!std::equal(wire.begin() + 16, wire.begin() + 32, allocation.stream_uuid.begin()) ||
      ReadLe64(wire.data() + 32) != allocation.stream_generation ||
      !std::equal(wire.begin() + 40,
                  wire.begin() + 56,
                  entry.publication.durable_publication_uuid.begin()) ||
      ReadLe64(wire.data() + 56) != entry.publication.durable_publication_generation ||
      ReadLe64(wire.data() + 80) != entry.received_bytes ||
      ReadLe64(wire.data() + 88) != entry.received_chunks ||
      !std::equal(wire.begin() + 96, wire.begin() + 112, allocation.owning_transaction_uuid.begin()) ||
      ReadLe64(wire.data() + 112) != allocation.owning_local_transaction_id ||
      !std::equal(wire.begin() + 120, wire.begin() + 152, entry.content_sha.begin()) ||
      ReadLe64(wire.data() + 184) != allocation.executor_availability_generation) {
    return false;
  }
  const auto affected = ReadLe64(wire.data() + 64);
  const auto rejected = ReadLe64(wire.data() + 72);
  return affected == entry.publication.affected_rows &&
         rejected == entry.publication.rejected_rows &&
         affected <= allocation.effective_maximum_rows &&
         rejected <= allocation.effective_maximum_rows &&
         affected + rejected >= affected &&
         affected + rejected != 0 &&
         affected + rejected <= kBulkImportStreamMaximumRowsV1;
}

Sha RecoveryKey(const BulkImportStreamEntry& entry) {
  ByteWriter material;
  constexpr std::string_view domain = "ScratchBird.BulkImportStreamRecoveryKey.V1";
  material.bytes.insert(material.bytes.end(), domain.begin(), domain.end());
  const auto& allocation = entry.allocation;
  material.Fixed(allocation.recovery_operation_uuid);
  material.U64(allocation.recovery_generation);
  material.Fixed(allocation.stream_uuid);
  material.U64(allocation.stream_generation);
  material.Fixed(allocation.descriptor_evidence);
  material.Fixed(allocation.target_relation_uuid);
  material.U64(allocation.target_relation_generation);
  material.Fixed(allocation.owning_transaction_uuid);
  material.U64(allocation.owning_local_transaction_id);
  material.Fixed(entry.content_sha);
  material.U64(allocation.route_generation);
  material.U8(allocation.cluster_bound ? 1 : 0);
  material.U64(allocation.cluster_bound ? allocation.cluster_epoch : 0);
  material.Fixed(allocation.cluster_fence_uuid);
  return Digest(material.bytes);
}

bool IncrementalContentHash(int fd, std::uint64_t total_bytes, Sha* output) {
  if (fd < 0 || !output || total_bytes == 0) return false;
  using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) return false;
  static constexpr char kDomain[] = "ScratchBird.BulkImportStreamContent.V1";
  if (EVP_DigestUpdate(context.get(), kDomain, sizeof(kDomain) - 1) != 1) return false;
  std::array<std::uint8_t, 8> total_le{};
  for (std::size_t i = 0; i < total_le.size(); ++i) {
    total_le[i] = static_cast<std::uint8_t>(total_bytes >> (i * 8));
  }
  if (EVP_DigestUpdate(context.get(), total_le.data(), total_le.size()) != 1) return false;
  std::vector<std::uint8_t> buffer(1024 * 1024);
  std::uint64_t offset = 0;
  while (offset < total_bytes) {
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), total_bytes - offset));
    if (!PreadAll(fd, buffer.data(), count, offset) ||
        EVP_DigestUpdate(context.get(), buffer.data(), count) != 1) {
      return false;
    }
    offset += count;
  }
  unsigned int digest_size = 0;
  return EVP_DigestFinal_ex(context.get(), output->data(), &digest_size) == 1 &&
         digest_size == output->size();
}

std::vector<std::uint8_t> BuildJournalRecord(JournalRecordType type,
                                             std::uint64_t ordinal,
                                             const std::vector<std::uint8_t>& body) {
  ByteWriter out;
  out.bytes.insert(out.bytes.end(), kJournalMagic.begin(), kJournalMagic.end());
  const auto total = kJournalHeaderBytes + body.size() + kJournalChecksumBytes;
  out.U32(static_cast<std::uint32_t>(total));
  out.U16(kFormatVersion);
  out.U8(static_cast<std::uint8_t>(type));
  out.U8(0);
  out.U64(ordinal);
  out.U32(static_cast<std::uint32_t>(body.size()));
  out.Raw(body);
  out.Fixed(Digest(out.bytes));
  return std::move(out.bytes);
}

std::vector<std::uint8_t> BuildMetadata(const BulkImportStreamEntry& entry) {
  ByteWriter body;
  body.U8(static_cast<std::uint8_t>(entry.state));
  body.U8(0);
  body.U16(0);
  body.Raw(EncodeAllocation(entry.allocation));
  body.U64(entry.received_chunks);
  body.U64(entry.received_bytes);
  body.Fixed(entry.chain_sha);
  body.Fixed(entry.content_sha);
  body.U8(entry.seal.present ? 1 : 0);
  for (std::size_t i = 0; i < 7; ++i) body.U8(0);
  body.U64(entry.seal.final_chunk_count);
  body.U64(entry.seal.total_stream_bytes);
  body.Fixed(entry.seal.final_chain_sha);
  body.Fixed(entry.seal.content_sha);
  body.Fixed(entry.seal.request_evidence);
  body.Fixed(entry.recovery_key_sha256);
  body.Fixed(entry.publication.durable_publication_uuid);
  body.U64(entry.publication.durable_publication_generation);
  body.U64(entry.publication.affected_rows);
  body.U64(entry.publication.rejected_rows);
  body.Fixed(entry.publication.postcondition_evidence_sha256);
  body.Fixed(entry.executor_evidence.executor_evidence_sha256);
  body.U32(static_cast<std::uint32_t>(entry.abort_reason));
  body.U32(static_cast<std::uint32_t>(entry.seal.ack_wire.size()));
  body.U32(static_cast<std::uint32_t>(entry.result_wire.size()));
  body.U32(0);
  body.U64(entry.journal_records);
  body.U64(entry.journal_bytes);
  body.Raw(entry.seal.ack_wire);
  body.Raw(entry.result_wire);

  ByteWriter out;
  out.bytes.insert(out.bytes.end(), kMetadataMagic.begin(), kMetadataMagic.end());
  out.U16(kFormatVersion);
  out.U16(0);
  out.U32(static_cast<std::uint32_t>(16 + body.bytes.size() + 32));
  out.Raw(body.bytes);
  out.Fixed(Digest(out.bytes));
  return std::move(out.bytes);
}

}  // namespace

SblrBulkImportStreamRegistry::SblrBulkImportStreamRegistry(std::filesystem::path root)
    : root_(std::move(root)) {
  struct stat initial_status {};
  if (::lstat(root_.c_str(), &initial_status) == 0) {
    if (S_ISLNK(initial_status.st_mode)) {
      startup_error_ = "root_symlink_forbidden";
      return;
    }
  } else if (errno == ENOENT) {
    if (::mkdir(root_.c_str(), 0700) != 0) {
      startup_error_ = "root_create_failed";
      return;
    }
  } else {
    startup_error_ = "root_status_failed";
    return;
  }
  std::error_code error;
  root_ = std::filesystem::weakly_canonical(root_, error);
  if (error) {
    startup_error_ = "root_canonicalization_failed";
    return;
  }
  root_fd_ = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root_fd_ < 0) {
    startup_error_ = "root_open_failed";
    return;
  }
  if (!PrivateDirectoryFd(root_fd_)) {
    startup_error_ = "root_permissions_invalid";
    return;
  }
  lock_fd_ = ::openat(root_fd_, "LOCK", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd_ < 0 || !RegularFd(lock_fd_)) {
    startup_error_ = "root_lock_invalid";
    return;
  }
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    startup_error_ = "root_lock_busy";
    return;
  }
  healthy_ = true;
}

SblrBulkImportStreamRegistry::~SblrBulkImportStreamRegistry() {
  if (lock_fd_ >= 0) {
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
  }
  if (root_fd_ >= 0) ::close(root_fd_);
}

bool SblrBulkImportStreamRegistry::healthy() const { return healthy_; }

const std::string& SblrBulkImportStreamRegistry::startup_error() const { return startup_error_; }

std::string SblrBulkImportStreamRegistry::BaseName(const Uuid& id) const {
  return "stream_" + HexUuid(id);
}

std::string SblrBulkImportStreamRegistry::MetaName(const Uuid& id) const {
  return BaseName(id) + ".meta";
}

std::string SblrBulkImportStreamRegistry::SpoolName(const Uuid& id) const {
  return BaseName(id) + ".spool";
}

std::string SblrBulkImportStreamRegistry::JournalName(const Uuid& id) const {
  return BaseName(id) + ".journal";
}

bool SblrBulkImportStreamRegistry::SafeRegularFile(const std::string& name,
                                                   bool allow_absent,
                                                   std::uint64_t* size) const {
  if (root_fd_ < 0 || name.empty() || name.find('/') != std::string::npos) return false;
  struct stat status {};
  if (::fstatat(root_fd_, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    return allow_absent && errno == ENOENT;
  }
  if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
      status.st_uid != ::geteuid() || (status.st_mode & 0777) != 0600 ||
      status.st_size < 0) {
    return false;
  }
  if (size) *size = static_cast<std::uint64_t>(status.st_size);
  return true;
}

bool SblrBulkImportStreamRegistry::Save(const BulkImportStreamEntry& entry) const {
  if (!healthy_ || !SafeRegularFile(MetaName(entry.allocation.stream_uuid), true, nullptr)) return false;
  const auto bytes = BuildMetadata(entry);
  std::string temp;
  int fd = -1;
  for (std::size_t attempt = 0; attempt < 16 && fd < 0; ++attempt) {
    temp = "." + MetaName(entry.allocation.stream_uuid) + ".tmp." +
           std::to_string(static_cast<unsigned long long>(::getpid())) + "." +
           std::to_string(++temporary_ordinal_);
    fd = ::openat(root_fd_,
                  temp.c_str(),
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0 && errno != EEXIST) break;
  }
  if (fd < 0 || !RegularFd(fd)) {
    if (fd >= 0) ::close(fd);
    return false;
  }
  const bool written = WriteAll(fd, bytes.data(), bytes.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!written || ::renameat(root_fd_, temp.c_str(), root_fd_, MetaName(entry.allocation.stream_uuid).c_str()) != 0 ||
      ::fsync(root_fd_) != 0) {
    ::unlinkat(root_fd_, temp.c_str(), 0);
    return false;
  }
  return true;
}

bool SblrBulkImportStreamRegistry::AppendJournal(BulkImportStreamEntry* entry,
                                                 std::uint8_t record_type,
                                                 const std::vector<std::uint8_t>& body,
                                                 std::string* error) const {
  if (!entry || record_type < static_cast<std::uint8_t>(JournalRecordType::allocation) ||
      record_type > static_cast<std::uint8_t>(JournalRecordType::result)) {
    if (error) *error = "journal_record_invalid";
    return false;
  }
  const auto record = BuildJournalRecord(static_cast<JournalRecordType>(record_type),
                                         entry->journal_records + 1,
                                         body);
  if (record.size() > kMaximumJournalRecordBytes ||
      !SafeRegularFile(JournalName(entry->allocation.stream_uuid), false, nullptr)) {
    if (error) *error = "journal_file_invalid";
    return false;
  }
  const int fd = ::openat(root_fd_,
                          JournalName(entry->allocation.stream_uuid).c_str(),
                          O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
  std::uint64_t original_size = 0;
  if (fd < 0 || !RegularFd(fd, &original_size) || original_size != entry->journal_bytes) {
    if (fd >= 0) ::close(fd);
    if (error) *error = "journal_position_conflict";
    return false;
  }
  const bool committed = WriteAll(fd, record.data(), record.size()) && ::fsync(fd) == 0;
  if (!committed) {
    const bool repaired = ::ftruncate(fd, static_cast<off_t>(original_size)) == 0 &&
                          ::fsync(fd) == 0;
    ::close(fd);
    if (!repaired) recovery_required_ = true;
    if (error) *error = repaired ? "journal_sync_failed" : "journal_recovery_required";
    return false;
  }
  ::close(fd);
  entry->journal_records += 1;
  entry->journal_bytes += record.size();
  return true;
}

SblrBulkImportStreamRegistry::LoadOutcome SblrBulkImportStreamRegistry::Load(
    const Uuid& id,
    BulkImportStreamEntry* output,
    std::string* load_error) const {
  if (!healthy_ || !output) return LoadOutcome::io_error;
  if (!SafeRegularFile(MetaName(id), true, nullptr)) {
    if (load_error) *load_error = "metadata_path_invalid";
    return LoadOutcome::corrupt;
  }
  std::uint64_t journal_size = 0;
  if (!SafeRegularFile(JournalName(id), true, &journal_size)) {
    if (load_error) *load_error = "journal_path_invalid";
    return LoadOutcome::corrupt;
  }
  const int journal_fd = ::openat(root_fd_,
                                  JournalName(id).c_str(),
                                  O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (journal_fd < 0) {
    if (errno == ENOENT) return LoadOutcome::absent;
    if (load_error) *load_error = "journal_open_failed";
    return LoadOutcome::io_error;
  }
  if (!RegularFd(journal_fd, &journal_size)) {
    ::close(journal_fd);
    if (load_error) *load_error = "journal_not_regular";
    return LoadOutcome::corrupt;
  }

  BulkImportStreamEntry entry;
  std::uint64_t offset = 0;
  std::uint64_t expected_ordinal = 1;
  bool have_allocation = false;
  bool repaired_tail = false;

  while (offset < journal_size) {
    const auto remaining = journal_size - offset;
    if (remaining < kJournalHeaderBytes) {
      repaired_tail = true;
      break;
    }
    std::array<std::uint8_t, kJournalHeaderBytes> header{};
    if (!PreadAll(journal_fd, header.data(), header.size(), offset)) {
      ::close(journal_fd);
      if (load_error) *load_error = "journal_read_failed";
      return LoadOutcome::io_error;
    }
    ByteReader head{header.data(), header.size()};
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t total = 0;
    std::uint16_t version = 0;
    std::uint8_t type_raw = 0;
    std::uint8_t reserved = 0;
    std::uint64_t ordinal = 0;
    std::uint32_t body_size = 0;
    if (!head.Fixed(&magic) || !head.U32(&total) || !head.U16(&version) ||
        !head.U8(&type_raw) || !head.U8(&reserved) || !head.U64(&ordinal) ||
        !head.U32(&body_size) || !head.Done() || magic != kJournalMagic ||
        version != kFormatVersion || reserved != 0 || ordinal != expected_ordinal ||
        total != kJournalHeaderBytes + body_size + kJournalChecksumBytes ||
        total > kMaximumJournalRecordBytes || total < kJournalHeaderBytes + kJournalChecksumBytes) {
      ::close(journal_fd);
      if (load_error) *load_error = "journal_header_corrupt";
      return LoadOutcome::corrupt;
    }
    if (total > remaining) {
      repaired_tail = true;
      break;
    }
    std::vector<std::uint8_t> record(total);
    if (!PreadAll(journal_fd, record.data(), record.size(), offset)) {
      ::close(journal_fd);
      if (load_error) *load_error = "journal_record_read_failed";
      return LoadOutcome::io_error;
    }
    std::vector<std::uint8_t> preimage(record.begin(), record.end() - kJournalChecksumBytes);
    Sha expected_checksum{};
    std::copy_n(record.end() - kJournalChecksumBytes,
                kJournalChecksumBytes,
                expected_checksum.begin());
    if (Digest(preimage) != expected_checksum) {
      ::close(journal_fd);
      if (load_error) *load_error = "journal_checksum_corrupt";
      return LoadOutcome::corrupt;
    }

    const auto* body_data = record.data() + kJournalHeaderBytes;
    ByteReader body{body_data, body_size};
    const auto type = static_cast<JournalRecordType>(type_raw);
    bool valid = true;
    switch (type) {
      case JournalRecordType::allocation: {
        valid = !have_allocation && expected_ordinal == 1 &&
                DecodeAllocation(body_data, body_size, &entry.allocation) &&
                entry.allocation.stream_uuid == id;
        if (valid) {
          have_allocation = true;
          entry.state = BulkImportStreamState::allocated;
          entry.chain_sha = bulk_wire::ChainStart(entry.allocation.stream_uuid,
                                                  entry.allocation.stream_generation,
                                                  entry.allocation.descriptor_evidence);
        }
        break;
      }
      case JournalRecordType::chunk: {
        BulkImportAcceptedChunk chunk;
        std::uint32_t ack_size = 0;
        valid = have_allocation && NormalState(entry.state) &&
                entry.state <= BulkImportStreamState::receiving &&
                body.U64(&chunk.sequence) && body.U64(&chunk.byte_offset) &&
                body.U32(&chunk.payload_bytes) && body.Fixed(&chunk.previous_chain_sha) &&
                body.Fixed(&chunk.payload_sha) && body.Fixed(&chunk.chain_sha) &&
                body.U32(&ack_size) && ack_size != 0 && ack_size <= 4096 &&
                body.Raw(ack_size, &chunk.ack_wire) && body.Done() &&
                chunk.sequence == entry.received_chunks + 1 &&
                chunk.byte_offset == entry.received_bytes &&
                chunk.payload_bytes != 0 &&
                chunk.payload_bytes <= entry.allocation.effective_maximum_chunk_bytes &&
                chunk.sequence <= entry.allocation.effective_maximum_chunk_count &&
                chunk.byte_offset <= entry.allocation.effective_maximum_stream_bytes &&
                chunk.payload_bytes <= entry.allocation.effective_maximum_stream_bytes - chunk.byte_offset &&
                chunk.previous_chain_sha == entry.chain_sha &&
                chunk.chain_sha == bulk_wire::ChainStep(chunk.previous_chain_sha,
                                                        chunk.sequence,
                                                        chunk.byte_offset,
                                                        chunk.payload_sha,
                                                        chunk.payload_bytes);
        bulk_wire::ChunkAck ack;
        valid = valid && bulk_wire::DecodeChunkAck(chunk.ack_wire.data(),
                                                   chunk.ack_wire.size(),
                                                   &ack,
                                                   nullptr) &&
                ack.stream_uuid == entry.allocation.stream_uuid &&
                ack.stream_generation == entry.allocation.stream_generation &&
                ack.accepted_sequence == chunk.sequence &&
                ack.accepted_total_bytes == chunk.byte_offset + chunk.payload_bytes &&
                ack.accepted_chain_sha256 == chunk.chain_sha &&
                ack.durable_spool_generation == entry.allocation.durable_spool_generation;
        if (valid) {
          entry.accepted_chunks.push_back(chunk);
          entry.received_chunks = chunk.sequence;
          entry.received_bytes = chunk.byte_offset + chunk.payload_bytes;
          entry.chain_sha = chunk.chain_sha;
          entry.state = BulkImportStreamState::receiving;
        }
        break;
      }
      case JournalRecordType::seal: {
        std::uint32_t ack_size = 0;
        BulkImportSealSnapshot seal;
        seal.present = true;
        valid = have_allocation && entry.state == BulkImportStreamState::receiving &&
                body.U64(&seal.final_chunk_count) && body.U64(&seal.total_stream_bytes) &&
                body.Fixed(&seal.final_chain_sha) && body.Fixed(&seal.content_sha) &&
                body.Fixed(&seal.request_evidence) && body.U32(&ack_size) &&
                ack_size != 0 && ack_size <= 4096 && body.Raw(ack_size, &seal.ack_wire) &&
                body.Done() && seal.final_chunk_count == entry.received_chunks &&
                seal.total_stream_bytes == entry.received_bytes &&
                seal.final_chain_sha == entry.chain_sha && NonZero(seal.content_sha) &&
                NonZero(seal.request_evidence);
        bulk_wire::SealAck ack;
        valid = valid && bulk_wire::DecodeSealAck(seal.ack_wire.data(),
                                                  seal.ack_wire.size(),
                                                  &ack,
                                                  nullptr) &&
                ack.stream_uuid == entry.allocation.stream_uuid &&
                ack.stream_generation == entry.allocation.stream_generation &&
                ack.durable_spool_uuid == entry.allocation.durable_spool_uuid &&
                ack.durable_spool_generation == entry.allocation.durable_spool_generation &&
                ack.chunk_count == seal.final_chunk_count &&
                ack.total_stream_bytes == seal.total_stream_bytes &&
                ack.final_chain_sha256 == seal.final_chain_sha &&
                ack.content_sha256 == seal.content_sha;
        if (valid) {
          entry.seal = std::move(seal);
          entry.content_sha = entry.seal.content_sha;
          entry.state = BulkImportStreamState::sealed;
        }
        break;
      }
      case JournalRecordType::transition: {
        std::uint8_t state_raw = 0;
        std::array<std::uint8_t, 7> zero{};
        std::array<std::uint8_t, 7> reserved_bytes{};
        valid = have_allocation && body.U8(&state_raw) && body.Fixed(&reserved_bytes) &&
                reserved_bytes == zero;
        const auto target = static_cast<BulkImportStreamState>(state_raw);
        if (valid && entry.state == BulkImportStreamState::sealed &&
            target == BulkImportStreamState::executing) {
          valid = body.Fixed(&entry.recovery_key_sha256) && body.Done() &&
                  entry.recovery_key_sha256 == RecoveryKey(entry);
        } else if (valid && entry.state == BulkImportStreamState::executing &&
                   target == BulkImportStreamState::published) {
          entry.publication.stream_uuid = entry.allocation.stream_uuid;
          entry.publication.stream_generation = entry.allocation.stream_generation;
          valid = body.Fixed(&entry.publication.recovery_key_sha256) &&
                  body.Fixed(&entry.publication.durable_publication_uuid) &&
                  body.U64(&entry.publication.durable_publication_generation) &&
                  body.U64(&entry.publication.affected_rows) &&
                  body.U64(&entry.publication.rejected_rows) &&
                  body.Fixed(&entry.publication.postcondition_evidence_sha256) &&
                  body.Done() &&
                  entry.publication.recovery_key_sha256 == entry.recovery_key_sha256 &&
                  NonZero(entry.publication.durable_publication_uuid) &&
                  entry.publication.durable_publication_generation != 0 &&
                  entry.publication.affected_rows + entry.publication.rejected_rows >=
                      entry.publication.affected_rows &&
                  entry.publication.affected_rows + entry.publication.rejected_rows != 0 &&
                  entry.publication.affected_rows + entry.publication.rejected_rows <=
                      entry.allocation.effective_maximum_rows &&
                  NonZero(entry.publication.postcondition_evidence_sha256);
        } else if (valid && entry.state == BulkImportStreamState::published &&
                   target == BulkImportStreamState::evidenced) {
          entry.executor_evidence.stream_uuid = entry.allocation.stream_uuid;
          entry.executor_evidence.stream_generation = entry.allocation.stream_generation;
          valid = body.Fixed(&entry.executor_evidence.durable_publication_uuid) &&
                  body.U64(&entry.executor_evidence.durable_publication_generation) &&
                  body.U64(&entry.executor_evidence.executor_availability_generation) &&
                  body.Fixed(&entry.executor_evidence.executor_evidence_sha256) &&
                  body.Done() &&
                  entry.executor_evidence.durable_publication_uuid ==
                      entry.publication.durable_publication_uuid &&
                  entry.executor_evidence.durable_publication_generation ==
                      entry.publication.durable_publication_generation &&
                  entry.executor_evidence.executor_availability_generation ==
                      entry.allocation.executor_availability_generation &&
                  NonZero(entry.executor_evidence.executor_evidence_sha256);
        } else {
          valid = false;
        }
        if (valid) entry.state = target;
        break;
      }
      case JournalRecordType::abort: {
        std::uint32_t reason = 0;
        valid = have_allocation && body.U32(&reason) && body.Done() &&
                entry.state < BulkImportStreamState::published &&
                reason >= static_cast<std::uint32_t>(BulkImportStreamAbortReason::chunk_conflict) &&
                reason <= static_cast<std::uint32_t>(BulkImportStreamAbortReason::execution_aborted);
        if (valid) {
          entry.abort_reason = static_cast<BulkImportStreamAbortReason>(reason);
          entry.state = BulkImportStreamState::aborted;
        }
        break;
      }
      case JournalRecordType::result: {
        std::uint32_t result_size = 0;
        std::vector<std::uint8_t> result_wire;
        valid = have_allocation && entry.state == BulkImportStreamState::evidenced &&
                body.U32(&result_size) && result_size == kResultWireBytes &&
                body.Raw(result_size, &result_wire) && body.Done() &&
                ValidResultWire(entry, result_wire);
        if (valid) {
          entry.result_wire = std::move(result_wire);
          entry.state = BulkImportStreamState::result_recorded;
        }
        break;
      }
      default:
        valid = false;
        break;
    }
    if (!valid) {
      ::close(journal_fd);
      if (load_error) *load_error = "journal_semantic_corrupt";
      return LoadOutcome::corrupt;
    }
    offset += total;
    expected_ordinal += 1;
  }

  if (repaired_tail) {
    if (::ftruncate(journal_fd, static_cast<off_t>(offset)) != 0 || ::fsync(journal_fd) != 0) {
      ::close(journal_fd);
      if (load_error) *load_error = "journal_tail_repair_failed";
      return LoadOutcome::io_error;
    }
  }
  ::close(journal_fd);
  if (!have_allocation) {
    if (load_error) *load_error = "journal_missing_allocation";
    return journal_size == 0 ? LoadOutcome::absent : LoadOutcome::corrupt;
  }
  entry.journal_records = expected_ordinal - 1;
  entry.journal_bytes = offset;

  std::uint64_t spool_size = 0;
  if (!SafeRegularFile(SpoolName(id), false, &spool_size)) {
    if (load_error) *load_error = "spool_path_invalid";
    return LoadOutcome::corrupt;
  }
  const int spool_fd = ::openat(root_fd_, SpoolName(id).c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (spool_fd < 0 || !RegularFd(spool_fd, &spool_size)) {
    if (spool_fd >= 0) ::close(spool_fd);
    if (load_error) *load_error = "spool_open_failed";
    return LoadOutcome::corrupt;
  }
  if (spool_size < entry.received_bytes) {
    ::close(spool_fd);
    if (load_error) *load_error = "spool_truncated";
    return LoadOutcome::corrupt;
  }
  if (spool_size > entry.received_bytes &&
      (::ftruncate(spool_fd, static_cast<off_t>(entry.received_bytes)) != 0 || ::fsync(spool_fd) != 0)) {
    ::close(spool_fd);
    if (load_error) *load_error = "spool_tail_repair_failed";
    return LoadOutcome::io_error;
  }
  for (const auto& chunk : entry.accepted_chunks) {
    std::vector<std::uint8_t> payload(chunk.payload_bytes);
    if (!PreadAll(spool_fd, payload.data(), payload.size(), chunk.byte_offset) ||
        bulk_wire::PayloadSha256(payload) != chunk.payload_sha) {
      ::close(spool_fd);
      if (load_error) *load_error = "spool_chunk_hash_corrupt";
      return LoadOutcome::corrupt;
    }
  }
  if (entry.seal.present) {
    Sha content{};
    if (!IncrementalContentHash(spool_fd, entry.received_bytes, &content) ||
        content != entry.content_sha) {
      ::close(spool_fd);
      if (load_error) *load_error = "spool_content_hash_corrupt";
      return LoadOutcome::corrupt;
    }
  }
  ::close(spool_fd);
  // The checksummed stream journal and fsynced spool retain durable transport
  // state.
  // Metadata is a replaceable acceleration checkpoint and is rebuilt best
  // effort after successful recovery.
  Save(entry);
  *output = std::move(entry);
  return LoadOutcome::loaded;
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::EnsureLoaded(
    const Uuid& id,
    BulkImportStreamEntry** output) const {
  if (!healthy_) return Failure(BulkImportStreamState::allocated, startup_error_);
  if (recovery_required_) {
    return Failure(BulkImportStreamState::aborted, "registry_recovery_required");
  }
  if (!output || !NonZero(id)) return Failure(BulkImportStreamState::allocated, "stream_identity_required");
  auto found = entries_.find(id);
  if (found != entries_.end()) {
    *output = &found->second;
    return Success(found->second.state, {}, true);
  }
  BulkImportStreamEntry loaded;
  std::string error;
  switch (Load(id, &loaded, &error)) {
    case LoadOutcome::loaded: {
      auto inserted = entries_.emplace(id, std::move(loaded)).first;
      *output = &inserted->second;
      return Success(inserted->second.state, {}, true);
    }
    case LoadOutcome::absent:
      return Failure(BulkImportStreamState::allocated, "unknown_stream");
    case LoadOutcome::corrupt:
      return Failure(BulkImportStreamState::aborted, error.empty() ? "stream_corrupt" : error);
    case LoadOutcome::io_error:
      return Failure(BulkImportStreamState::allocated, error.empty() ? "stream_io_error" : error);
  }
  return Failure(BulkImportStreamState::allocated, "stream_load_failed");
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::Allocate(
    const BulkImportStreamAllocation& allocation) {
  std::unique_lock lock(mutex_);
  if (!healthy_) return Failure(BulkImportStreamState::allocated, startup_error_);
  if (recovery_required_) {
    return Failure(BulkImportStreamState::aborted, "registry_recovery_required");
  }
  if (!AllocationValid(allocation)) return Failure(BulkImportStreamState::allocated, "allocation_invalid");

  auto in_memory = entries_.find(allocation.stream_uuid);
  if (in_memory != entries_.end()) {
    if (!SameAllocation(in_memory->second.allocation, allocation)) {
      return Failure(in_memory->second.state, "allocation_conflict");
    }
    return Success(in_memory->second.state, {}, true);
  }

  BulkImportStreamEntry disk;
  std::string load_error;
  const auto loaded = Load(allocation.stream_uuid, &disk, &load_error);
  if (loaded == LoadOutcome::loaded) {
    if (!SameAllocation(disk.allocation, allocation)) return Failure(disk.state, "allocation_conflict");
    auto inserted = entries_.emplace(allocation.stream_uuid, std::move(disk)).first;
    return Success(inserted->second.state, {}, true);
  }
  if (loaded == LoadOutcome::corrupt || loaded == LoadOutcome::io_error) {
    return Failure(BulkImportStreamState::allocated,
                   load_error.empty() ? "allocation_recovery_failed" : load_error);
  }

  std::uint64_t orphan_spool_size = 0;
  std::uint64_t orphan_journal_size = 0;
  const bool spool_path_safe = SafeRegularFile(SpoolName(allocation.stream_uuid), true, &orphan_spool_size);
  const bool journal_path_safe = SafeRegularFile(JournalName(allocation.stream_uuid), true, &orphan_journal_size);
  const bool meta_path_safe = SafeRegularFile(MetaName(allocation.stream_uuid), true, nullptr);
  if (!spool_path_safe || !journal_path_safe || !meta_path_safe) {
    return Failure(BulkImportStreamState::allocated, "allocation_path_invalid");
  }
  struct stat orphan_status {};
  const bool spool_exists = ::fstatat(root_fd_,
                                      SpoolName(allocation.stream_uuid).c_str(),
                                      &orphan_status,
                                      AT_SYMLINK_NOFOLLOW) == 0;
  const bool journal_exists = ::fstatat(root_fd_,
                                        JournalName(allocation.stream_uuid).c_str(),
                                        &orphan_status,
                                        AT_SYMLINK_NOFOLLOW) == 0;
  const bool meta_exists = ::fstatat(root_fd_,
                                     MetaName(allocation.stream_uuid).c_str(),
                                     &orphan_status,
                                     AT_SYMLINK_NOFOLLOW) == 0;
  if (meta_exists) {
    return Failure(BulkImportStreamState::allocated, "orphaned_metadata_conflict");
  }
  if ((spool_exists && orphan_spool_size != 0) ||
      (journal_exists && orphan_journal_size != 0)) {
    return Failure(BulkImportStreamState::allocated, "orphaned_stream_conflict");
  }
  if (spool_exists && ::unlinkat(root_fd_, SpoolName(allocation.stream_uuid).c_str(), 0) != 0) {
    return Failure(BulkImportStreamState::allocated, "orphaned_spool_cleanup_failed");
  }
  if (journal_exists && ::unlinkat(root_fd_, JournalName(allocation.stream_uuid).c_str(), 0) != 0) {
    return Failure(BulkImportStreamState::allocated, "orphaned_journal_cleanup_failed");
  }
  if ((spool_exists || journal_exists) && ::fsync(root_fd_) != 0) {
    return Failure(BulkImportStreamState::allocated, "orphan_cleanup_sync_failed");
  }

  int spool_fd = ::openat(root_fd_,
                          SpoolName(allocation.stream_uuid).c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          0600);
  if (spool_fd < 0 || !RegularFd(spool_fd) || ::fsync(spool_fd) != 0) {
    if (spool_fd >= 0) ::close(spool_fd);
    ::unlinkat(root_fd_, SpoolName(allocation.stream_uuid).c_str(), 0);
    return Failure(BulkImportStreamState::allocated, "spool_create_failed");
  }
  ::close(spool_fd);
  const int journal_fd = ::openat(root_fd_,
                                  JournalName(allocation.stream_uuid).c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                  0600);
  if (journal_fd < 0 || !RegularFd(journal_fd) || ::fsync(journal_fd) != 0) {
    if (journal_fd >= 0) ::close(journal_fd);
    ::unlinkat(root_fd_, JournalName(allocation.stream_uuid).c_str(), 0);
    ::unlinkat(root_fd_, SpoolName(allocation.stream_uuid).c_str(), 0);
    return Failure(BulkImportStreamState::allocated, "journal_create_failed");
  }
  ::close(journal_fd);
  if (::fsync(root_fd_) != 0) return Failure(BulkImportStreamState::allocated, "allocation_directory_sync_failed");

  BulkImportStreamEntry entry;
  entry.allocation = allocation;
  entry.chain_sha = bulk_wire::ChainStart(allocation.stream_uuid,
                                          allocation.stream_generation,
                                          allocation.descriptor_evidence);
  std::string journal_error;
  if (!AppendJournal(&entry,
                     static_cast<std::uint8_t>(JournalRecordType::allocation),
                     EncodeAllocation(allocation),
                     &journal_error)) {
    ::unlinkat(root_fd_, JournalName(allocation.stream_uuid).c_str(), 0);
    ::unlinkat(root_fd_, SpoolName(allocation.stream_uuid).c_str(), 0);
    ::fsync(root_fd_);
    return Failure(BulkImportStreamState::allocated, journal_error);
  }
  Save(entry);
  entries_.emplace(allocation.stream_uuid, entry);
  return Success(BulkImportStreamState::allocated);
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::Abort(
    BulkImportStreamEntry* entry,
    BulkImportStreamAbortReason reason,
    std::string error) {
  if (!entry) return Failure(BulkImportStreamState::aborted, std::move(error));
  if (entry->state >= BulkImportStreamState::published && NormalState(entry->state)) {
    return Failure(entry->state, std::move(error));
  }
  if (entry->state == BulkImportStreamState::aborted) {
    return Failure(entry->state, std::move(error));
  }
  ByteWriter body;
  body.U32(static_cast<std::uint32_t>(reason));
  std::string journal_error;
  if (!AppendJournal(entry,
                     static_cast<std::uint8_t>(JournalRecordType::abort),
                     body.bytes,
                     &journal_error)) {
    return Failure(entry->state, std::move(error) + ":" + journal_error);
  }
  entry->abort_reason = reason;
  entry->state = BulkImportStreamState::aborted;
  Save(*entry);
  auto result = Failure(entry->state, std::move(error));
  result.durable = true;
  return result;
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::AllocateOrReplay(
    const Uuid& receipt, std::uint64_t structural_occurrence,
    std::uint32_t occurrence,
    const Sha& authority_evidence, const BulkImportAllocationFactory& factory) {
  std::unique_lock lock(mutex_);
  if (!healthy_) return Failure(BulkImportStreamState::allocated, startup_error_);
  for (const auto& [id, current] : entries_) {
    const auto& a = current.allocation;
    if (a.authenticated_receipt_uuid == receipt &&
        a.structural_occurrence == structural_occurrence &&
        a.import_occurrence == occurrence) {
      if (a.authority_evidence_sha256 != authority_evidence)
        return Failure(current.state, "allocation_authority_conflict");
      auto replay = Success(current.state, {}, true);
      replay.allocation = a;
      return replay;
    }
  }
  std::error_code scan_error;
  for (const auto& file : std::filesystem::directory_iterator(root_, scan_error)) {
    const auto name = file.path().filename().string();
    if (name.size() != 47 || name.compare(0, 7, "stream_") != 0 || name.substr(39) != ".journal") continue;
    Uuid id{};
    if (!ParseHexUuid(std::string_view(name).substr(7, 32), &id)) continue;
    BulkImportStreamEntry disk; std::string load_error;
    if (Load(id, &disk, &load_error) != LoadOutcome::loaded) continue;
    entries_.emplace(id, disk);
    const auto& a = entries_.find(id)->second.allocation;
    if (a.authenticated_receipt_uuid == receipt && a.structural_occurrence == structural_occurrence && a.import_occurrence == occurrence) {
      if (a.authority_evidence_sha256 != authority_evidence) return Failure(entries_.find(id)->second.state, "allocation_authority_conflict");
      auto replay = Success(entries_.find(id)->second.state, {}, true); replay.allocation = a; return replay;
    }
  }
  if (!factory) return Failure(BulkImportStreamState::allocated, "allocation_factory_missing");
  BulkImportStreamAllocation allocation;
  if (!factory(&allocation) || allocation.authenticated_receipt_uuid != receipt ||
      allocation.structural_occurrence != structural_occurrence ||
      allocation.import_occurrence != occurrence ||
      allocation.authority_evidence_sha256 != authority_evidence)
    return Failure(BulkImportStreamState::allocated, "allocation_factory_invalid");
  auto result = Allocate(allocation);
  if (result.ok) result.allocation = allocation;
  return result;
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::Append(const BulkImportChunk& chunk) {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(chunk.stream_uuid, &entry);
  if (!loaded.ok || !entry) return loaded;
  if (entry->state == BulkImportStreamState::aborted || entry->state == BulkImportStreamState::refused) {
    return Failure(entry->state, "stream_terminal");
  }
  const auto& allocation = entry->allocation;
  if (chunk.authenticated_receipt_uuid != allocation.authenticated_receipt_uuid ||
      chunk.stream_generation != allocation.stream_generation ||
      chunk.structural_occurrence != allocation.structural_occurrence ||
      chunk.import_occurrence != allocation.import_occurrence ||
      chunk.descriptor_evidence != allocation.descriptor_evidence) {
    return Failure(entry->state, "stream_identity_conflict");
  }
  const bool reuses_sequence = chunk.sequence != 0 && chunk.sequence <= entry->received_chunks;
  const auto offset_match = std::find_if(
      entry->accepted_chunks.begin(),
      entry->accepted_chunks.end(),
      [&](const BulkImportAcceptedChunk& accepted) {
        return accepted.byte_offset == chunk.byte_offset;
      });
  const bool reuses_offset = offset_match != entry->accepted_chunks.end();
  if (reuses_sequence || reuses_offset) {
    const BulkImportAcceptedChunk* accepted = nullptr;
    if (reuses_sequence) {
      accepted = &entry->accepted_chunks[static_cast<std::size_t>(chunk.sequence - 1)];
    }
    if (reuses_offset && accepted != &*offset_match) accepted = nullptr;
    bool exact = accepted != nullptr &&
                 chunk.byte_offset == accepted->byte_offset &&
                 chunk.sequence == accepted->sequence &&
                 chunk.payload.size() == accepted->payload_bytes &&
                 chunk.previous_chain_sha == accepted->previous_chain_sha &&
                 chunk.payload_sha == accepted->payload_sha &&
                 chunk.chain_sha == accepted->chain_sha;
    if (exact) {
      const int fd = ::openat(root_fd_, SpoolName(chunk.stream_uuid).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      std::vector<std::uint8_t> persisted(chunk.payload.size());
      exact = fd >= 0 && RegularFd(fd) &&
              PreadAll(fd, persisted.data(), persisted.size(), accepted->byte_offset) &&
              persisted == chunk.payload;
      if (fd >= 0) ::close(fd);
    }
    if (exact) return Success(entry->state, accepted->ack_wire, true);
    return Abort(entry, BulkImportStreamAbortReason::chunk_conflict, "chunk_conflict");
  }

  if (chunk.payload.empty() ||
      chunk.payload.size() > allocation.effective_maximum_chunk_bytes ||
      chunk.payload.size() > kBulkImportStreamMaximumChunkBytesV1 ||
      chunk.sequence == 0 || chunk.byte_offset > allocation.effective_maximum_stream_bytes ||
      chunk.payload.size() > allocation.effective_maximum_stream_bytes - chunk.byte_offset ||
      chunk.sequence > allocation.effective_maximum_chunk_count) {
    return Failure(entry->state, "chunk_limit_invalid");
  }
  if (bulk_wire::PayloadSha256(chunk.payload) != chunk.payload_sha) {
    return Failure(entry->state, "payload_hash_mismatch");
  }

  if ((entry->state != BulkImportStreamState::allocated &&
       entry->state != BulkImportStreamState::receiving) ||
      chunk.sequence != entry->received_chunks + 1 ||
      chunk.byte_offset != entry->received_bytes ||
      chunk.previous_chain_sha != entry->chain_sha) {
    return Failure(entry->state, "chunk_sequence_or_offset_invalid");
  }
  const auto expected_chain = bulk_wire::ChainStep(chunk.previous_chain_sha,
                                                   chunk.sequence,
                                                   chunk.byte_offset,
                                                   chunk.payload_sha,
                                                   static_cast<std::uint32_t>(chunk.payload.size()));
  if (chunk.chain_sha != expected_chain) return Failure(entry->state, "chunk_chain_mismatch");

  std::uint64_t spool_size = 0;
  const int spool_fd = ::openat(root_fd_,
                                SpoolName(chunk.stream_uuid).c_str(),
                                O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (spool_fd < 0 || !RegularFd(spool_fd, &spool_size) || spool_size != entry->received_bytes) {
    if (spool_fd >= 0) ::close(spool_fd);
    return Failure(entry->state, "spool_position_conflict");
  }
  if (!PwriteAll(spool_fd, chunk.payload.data(), chunk.payload.size(), chunk.byte_offset) ||
      ::fsync(spool_fd) != 0) {
    const bool repaired = ::ftruncate(spool_fd, static_cast<off_t>(entry->received_bytes)) == 0 &&
                          ::fsync(spool_fd) == 0;
    if (!repaired) recovery_required_ = true;
    ::close(spool_fd);
    return Failure(entry->state,
                   repaired ? "spool_append_failed" : "spool_recovery_required");
  }
  ::close(spool_fd);

  bulk_wire::ChunkAck ack;
  ack.stream_uuid = chunk.stream_uuid;
  ack.stream_generation = chunk.stream_generation;
  ack.accepted_sequence = chunk.sequence;
  ack.accepted_total_bytes = chunk.byte_offset + chunk.payload.size();
  ack.accepted_chain_sha256 = chunk.chain_sha;
  ack.durable_spool_generation = allocation.durable_spool_generation;
  ack.ack_evidence_sha256 = bulk_wire::ChunkAckEvidence(ack);
  std::vector<std::uint8_t> ack_wire;
  if (!bulk_wire::EncodeChunkAck(ack, &ack_wire, nullptr)) {
    const int repair_fd = ::openat(root_fd_, SpoolName(chunk.stream_uuid).c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    bool repaired = false;
    if (repair_fd >= 0) {
      repaired = ::ftruncate(repair_fd, static_cast<off_t>(entry->received_bytes)) == 0 &&
                 ::fsync(repair_fd) == 0;
      ::close(repair_fd);
    }
    if (!repaired) recovery_required_ = true;
    return Failure(entry->state,
                   repaired ? "chunk_ack_encoding_failed" : "spool_recovery_required");
  }
  BulkImportAcceptedChunk accepted;
  accepted.sequence = chunk.sequence;
  accepted.byte_offset = chunk.byte_offset;
  accepted.payload_bytes = static_cast<std::uint32_t>(chunk.payload.size());
  accepted.previous_chain_sha = chunk.previous_chain_sha;
  accepted.payload_sha = chunk.payload_sha;
  accepted.chain_sha = chunk.chain_sha;
  accepted.ack_wire = ack_wire;
  ByteWriter body;
  body.U64(accepted.sequence);
  body.U64(accepted.byte_offset);
  body.U32(accepted.payload_bytes);
  body.Fixed(accepted.previous_chain_sha);
  body.Fixed(accepted.payload_sha);
  body.Fixed(accepted.chain_sha);
  body.U32(static_cast<std::uint32_t>(accepted.ack_wire.size()));
  body.Raw(accepted.ack_wire);
  std::string journal_error;
  if (!AppendJournal(entry,
                     static_cast<std::uint8_t>(JournalRecordType::chunk),
                     body.bytes,
                     &journal_error)) {
    const int repair_fd = ::openat(root_fd_, SpoolName(chunk.stream_uuid).c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    bool repaired = false;
    if (repair_fd >= 0) {
      repaired = ::ftruncate(repair_fd, static_cast<off_t>(entry->received_bytes)) == 0 &&
                 ::fsync(repair_fd) == 0;
      ::close(repair_fd);
    }
    if (!repaired) recovery_required_ = true;
    return Failure(entry->state,
                   repaired ? journal_error : "spool_recovery_required");
  }
  entry->accepted_chunks.push_back(accepted);
  entry->received_chunks = accepted.sequence;
  entry->received_bytes = accepted.byte_offset + accepted.payload_bytes;
  entry->chain_sha = accepted.chain_sha;
  entry->state = BulkImportStreamState::receiving;
  Save(*entry);
  return Success(entry->state, std::move(ack_wire));
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::Seal(const BulkImportSeal& seal) {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(seal.stream_uuid, &entry);
  if (!loaded.ok || !entry) return loaded;
  if (entry->state == BulkImportStreamState::aborted || entry->state == BulkImportStreamState::refused) {
    return Failure(entry->state, "stream_terminal");
  }
  const auto& allocation = entry->allocation;
  if (seal.authenticated_receipt_uuid != allocation.authenticated_receipt_uuid ||
      seal.stream_generation != allocation.stream_generation ||
      seal.descriptor_evidence != allocation.descriptor_evidence) {
    return Failure(entry->state, "stream_identity_conflict");
  }
  bulk_wire::Seal wire_seal;
  wire_seal.authenticated_receipt_uuid = seal.authenticated_receipt_uuid;
  wire_seal.stream_uuid = seal.stream_uuid;
  wire_seal.stream_generation = seal.stream_generation;
  wire_seal.descriptor_evidence = seal.descriptor_evidence;
  wire_seal.final_chunk_count = seal.final_chunk_count;
  wire_seal.total_stream_bytes = seal.total_stream_bytes;
  wire_seal.final_chain_sha256 = seal.final_chain_sha;
  wire_seal.content_sha256 = seal.content_sha;
  wire_seal.seal_request_evidence_sha256 = seal.seal_request_evidence;
  if (!bulk_wire::ValidateSealEvidence(wire_seal)) return Failure(entry->state, "seal_evidence_invalid");

  if (entry->seal.present) {
    const bool exact = seal.final_chunk_count == entry->seal.final_chunk_count &&
                       seal.total_stream_bytes == entry->seal.total_stream_bytes &&
                       seal.final_chain_sha == entry->seal.final_chain_sha &&
                       seal.content_sha == entry->seal.content_sha &&
                       seal.seal_request_evidence == entry->seal.request_evidence;
    if (exact) return Success(entry->state, entry->seal.ack_wire, true);
    return Abort(entry, BulkImportStreamAbortReason::seal_conflict, "seal_conflict");
  }
  if (entry->state != BulkImportStreamState::receiving ||
      entry->received_chunks == 0 || entry->received_bytes == 0 ||
      seal.final_chunk_count != entry->received_chunks ||
      seal.total_stream_bytes != entry->received_bytes ||
      seal.final_chain_sha != entry->chain_sha) {
    return Failure(entry->state, "seal_tuple_invalid");
  }
  std::uint64_t spool_size = 0;
  const int spool_fd = ::openat(root_fd_, SpoolName(seal.stream_uuid).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  Sha content{};
  const bool content_valid = spool_fd >= 0 && RegularFd(spool_fd, &spool_size) &&
                             spool_size == entry->received_bytes &&
                             IncrementalContentHash(spool_fd, entry->received_bytes, &content) &&
                             content == seal.content_sha;
  if (spool_fd >= 0) ::close(spool_fd);
  if (!content_valid) return Failure(entry->state, "content_hash_mismatch");

  bulk_wire::SealAck ack;
  ack.stream_uuid = seal.stream_uuid;
  ack.stream_generation = seal.stream_generation;
  ack.durable_spool_uuid = allocation.durable_spool_uuid;
  ack.durable_spool_generation = allocation.durable_spool_generation;
  ack.chunk_count = seal.final_chunk_count;
  ack.total_stream_bytes = seal.total_stream_bytes;
  ack.final_chain_sha256 = seal.final_chain_sha;
  ack.content_sha256 = seal.content_sha;
  ack.seal_evidence_sha256 = bulk_wire::SealAckEvidence(ack);
  std::vector<std::uint8_t> ack_wire;
  if (!bulk_wire::EncodeSealAck(ack, &ack_wire, nullptr)) {
    return Failure(entry->state, "seal_ack_encoding_failed");
  }
  BulkImportSealSnapshot snapshot;
  snapshot.present = true;
  snapshot.final_chunk_count = seal.final_chunk_count;
  snapshot.total_stream_bytes = seal.total_stream_bytes;
  snapshot.final_chain_sha = seal.final_chain_sha;
  snapshot.content_sha = seal.content_sha;
  snapshot.request_evidence = seal.seal_request_evidence;
  snapshot.ack_wire = ack_wire;
  ByteWriter body;
  body.U64(snapshot.final_chunk_count);
  body.U64(snapshot.total_stream_bytes);
  body.Fixed(snapshot.final_chain_sha);
  body.Fixed(snapshot.content_sha);
  body.Fixed(snapshot.request_evidence);
  body.U32(static_cast<std::uint32_t>(snapshot.ack_wire.size()));
  body.Raw(snapshot.ack_wire);
  std::string journal_error;
  if (!AppendJournal(entry,
                     static_cast<std::uint8_t>(JournalRecordType::seal),
                     body.bytes,
                     &journal_error)) {
    return Failure(entry->state, journal_error);
  }
  entry->seal = std::move(snapshot);
  entry->content_sha = seal.content_sha;
  entry->state = BulkImportStreamState::sealed;
  Save(*entry);
  return Success(entry->state, std::move(ack_wire));
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::BeginExecution(
    const Uuid& id,
    std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(id, &entry);
  if (!loaded.ok || !entry) return loaded;
  if (entry->allocation.stream_generation != generation) return Failure(entry->state, "generation_conflict");
  if (!NormalState(entry->state)) return Failure(entry->state, "stream_terminal");
  if (NormalStateOrdinal(entry->state) >= NormalStateOrdinal(BulkImportStreamState::executing)) {
    return NonZero(entry->recovery_key_sha256)
               ? Success(entry->state, {}, true)
               : Failure(entry->state, "recovery_record_missing");
  }
  if (entry->state != BulkImportStreamState::sealed) return Failure(entry->state, "stream_not_sealed");
  const auto recovery_key = RecoveryKey(*entry);
  if (!NonZero(recovery_key)) return Failure(entry->state, "recovery_key_invalid");
  ByteWriter body;
  body.U8(static_cast<std::uint8_t>(BulkImportStreamState::executing));
  for (std::size_t i = 0; i < 7; ++i) body.U8(0);
  body.Fixed(recovery_key);
  std::string journal_error;
  if (!AppendJournal(entry,
                     static_cast<std::uint8_t>(JournalRecordType::transition),
                     body.bytes,
                     &journal_error)) {
    return Failure(entry->state, journal_error);
  }
  entry->recovery_key_sha256 = recovery_key;
  entry->state = BulkImportStreamState::executing;
  Save(*entry);
  return Success(entry->state);
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::Publish(
    const BulkImportPublicationRecord& publication) {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(publication.stream_uuid, &entry);
  if (!loaded.ok || !entry) return loaded;
  const bool identity_valid = publication.stream_generation == entry->allocation.stream_generation &&
                              publication.recovery_key_sha256 == entry->recovery_key_sha256 &&
                              NonZero(publication.durable_publication_uuid) &&
                              publication.durable_publication_generation != 0 &&
                              publication.affected_rows + publication.rejected_rows >=
                                  publication.affected_rows &&
                              publication.affected_rows + publication.rejected_rows != 0 &&
                              publication.affected_rows + publication.rejected_rows <=
                                  entry->allocation.effective_maximum_rows &&
                              NonZero(publication.postcondition_evidence_sha256);
  if (!identity_valid) return Failure(entry->state, "publication_record_invalid");
  if (NormalState(entry->state) &&
      NormalStateOrdinal(entry->state) >= NormalStateOrdinal(BulkImportStreamState::published)) {
    const auto& persisted = entry->publication;
    const bool exact = persisted.stream_uuid == publication.stream_uuid &&
                       persisted.stream_generation == publication.stream_generation &&
                       persisted.recovery_key_sha256 == publication.recovery_key_sha256 &&
                       persisted.durable_publication_uuid == publication.durable_publication_uuid &&
                       persisted.durable_publication_generation == publication.durable_publication_generation &&
                       persisted.affected_rows == publication.affected_rows &&
                       persisted.rejected_rows == publication.rejected_rows &&
                       persisted.postcondition_evidence_sha256 == publication.postcondition_evidence_sha256;
    return exact ? Success(entry->state, {}, true)
                 : Failure(entry->state, "publication_conflict");
  }
  if (entry->state != BulkImportStreamState::executing) return Failure(entry->state, "execution_record_missing");
  ByteWriter body;
  body.U8(static_cast<std::uint8_t>(BulkImportStreamState::published));
  for (std::size_t i = 0; i < 7; ++i) body.U8(0);
  body.Fixed(publication.recovery_key_sha256);
  body.Fixed(publication.durable_publication_uuid);
  body.U64(publication.durable_publication_generation);
  body.U64(publication.affected_rows);
  body.U64(publication.rejected_rows);
  body.Fixed(publication.postcondition_evidence_sha256);
  std::string journal_error;
  if (!AppendJournal(entry,
                     static_cast<std::uint8_t>(JournalRecordType::transition),
                     body.bytes,
                     &journal_error)) {
    return Failure(entry->state, journal_error);
  }
  entry->publication = publication;
  entry->state = BulkImportStreamState::published;
  Save(*entry);
  return Success(entry->state);
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::RecordEvidence(
    const BulkImportExecutorEvidenceRecord& evidence) {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(evidence.stream_uuid, &entry);
  if (!loaded.ok || !entry) return loaded;
  const bool identity_valid = evidence.stream_generation == entry->allocation.stream_generation &&
                              evidence.durable_publication_uuid ==
                                  entry->publication.durable_publication_uuid &&
                              evidence.durable_publication_generation ==
                                  entry->publication.durable_publication_generation &&
                              evidence.executor_availability_generation ==
                                  entry->allocation.executor_availability_generation &&
                              NonZero(evidence.executor_evidence_sha256);
  if (!identity_valid) return Failure(entry->state, "executor_evidence_invalid");
  if (NormalState(entry->state) &&
      NormalStateOrdinal(entry->state) >= NormalStateOrdinal(BulkImportStreamState::evidenced)) {
    const auto& persisted = entry->executor_evidence;
    const bool exact = persisted.stream_uuid == evidence.stream_uuid &&
                       persisted.stream_generation == evidence.stream_generation &&
                       persisted.durable_publication_uuid == evidence.durable_publication_uuid &&
                       persisted.durable_publication_generation == evidence.durable_publication_generation &&
                       persisted.executor_availability_generation == evidence.executor_availability_generation &&
                       persisted.executor_evidence_sha256 == evidence.executor_evidence_sha256;
    return exact ? Success(entry->state, {}, true)
                 : Failure(entry->state, "executor_evidence_conflict");
  }
  if (entry->state != BulkImportStreamState::published) return Failure(entry->state, "publication_record_missing");
  ByteWriter body;
  body.U8(static_cast<std::uint8_t>(BulkImportStreamState::evidenced));
  for (std::size_t i = 0; i < 7; ++i) body.U8(0);
  body.Fixed(evidence.durable_publication_uuid);
  body.U64(evidence.durable_publication_generation);
  body.U64(evidence.executor_availability_generation);
  body.Fixed(evidence.executor_evidence_sha256);
  std::string journal_error;
  if (!AppendJournal(entry,
                     static_cast<std::uint8_t>(JournalRecordType::transition),
                     body.bytes,
                     &journal_error)) {
    return Failure(entry->state, journal_error);
  }
  entry->executor_evidence = evidence;
  entry->state = BulkImportStreamState::evidenced;
  Save(*entry);
  return Success(entry->state);
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::RecordResult(
    const Uuid& id,
    std::uint64_t generation,
    std::vector<std::uint8_t> wire) {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(id, &entry);
  if (!loaded.ok || !entry) return loaded;
  if (entry->allocation.stream_generation != generation) return Failure(entry->state, "generation_conflict");
  if (entry->state == BulkImportStreamState::result_recorded) {
    if (entry->result_wire != wire) return Failure(entry->state, "result_conflict");
    return Success(entry->state, entry->result_wire, true);
  }
  if (entry->state != BulkImportStreamState::evidenced || !ValidResultWire(*entry, wire)) {
    return Failure(entry->state, "result_precondition_invalid");
  }
  ByteWriter body;
  body.U32(static_cast<std::uint32_t>(wire.size()));
  body.Raw(wire);
  std::string journal_error;
  if (!AppendJournal(entry,
                     static_cast<std::uint8_t>(JournalRecordType::result),
                     body.bytes,
                     &journal_error)) {
    return Failure(entry->state, journal_error);
  }
  entry->result_wire = wire;
  entry->state = BulkImportStreamState::result_recorded;
  Save(*entry);
  return Success(entry->state, std::move(wire));
}

BulkImportStreamRegistryResult
SblrBulkImportStreamRegistry::AbortBeforePublication(
    const Uuid& id, std::uint64_t generation,
    BulkImportStreamAbortReason reason, std::string detail) {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(id, &entry);
  if (!loaded.ok || !entry) return loaded;
  if (entry->allocation.stream_generation != generation) {
    return Failure(entry->state, "generation_conflict");
  }
  if (NormalState(entry->state) &&
      NormalStateOrdinal(entry->state) >=
          NormalStateOrdinal(BulkImportStreamState::published)) {
    return Failure(entry->state, "publication_already_durable");
  }
  if (entry->state == BulkImportStreamState::aborted) {
    return entry->abort_reason == reason
               ? Success(entry->state, {}, true)
               : Failure(entry->state, "abort_reason_conflict");
  }
  if (entry->state == BulkImportStreamState::refused) {
    return Failure(entry->state, "stream_terminal");
  }
  if (reason == BulkImportStreamAbortReason::none) {
    return Failure(entry->state, "abort_reason_required");
  }
  return Abort(entry, reason,
               detail.empty() ? "bulk_import_aborted" : std::move(detail));
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::Recover(
    const Uuid& id,
    BulkImportStreamEntry* output) const {
  std::lock_guard lock(mutex_);
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(id, &entry);
  if (!loaded.ok || !entry) return loaded;
  if (!output) return Failure(entry->state, "recovery_output_required");
  *output = *entry;
  auto result = Success(entry->state, entry->result_wire, true);
  if (entry->state != BulkImportStreamState::result_recorded) result.response_wire.clear();
  return result;
}

BulkImportStreamRegistryResult SblrBulkImportStreamRegistry::ReadSealedSpool(
    const Uuid& id, std::uint64_t generation, const Uuid& receipt,
    const Sha& descriptor_evidence, const BulkImportSpoolReader& reader,
    BulkImportSealedSpoolSnapshot* snapshot) const {
  std::lock_guard lock(mutex_);
  if (!healthy_) return Failure(BulkImportStreamState::allocated, startup_error_);
  if (!reader || !snapshot) return Failure(BulkImportStreamState::allocated, "spool_reader_missing");
  std::uint64_t observed_size = 0;
  if (!SafeRegularFile(SpoolName(id), false, &observed_size))
    return Failure(BulkImportStreamState::allocated, "spool_path_invalid");
  BulkImportStreamEntry* entry = nullptr;
  auto loaded = EnsureLoaded(id, &entry);
  if (!loaded.ok || !entry) return loaded;
  const auto& allocation = entry->allocation;
  if (allocation.stream_uuid != id || allocation.stream_generation != generation ||
      allocation.authenticated_receipt_uuid != receipt ||
      allocation.descriptor_evidence != descriptor_evidence)
    return Failure(entry->state, "spool_identity_mismatch");
  if (!NormalState(entry->state) ||
      NormalStateOrdinal(entry->state) < NormalStateOrdinal(BulkImportStreamState::sealed) ||
      !entry->seal.present || observed_size != entry->received_bytes ||
      entry->seal.total_stream_bytes != entry->received_bytes ||
      entry->seal.final_chunk_count != entry->received_chunks ||
      entry->seal.final_chain_sha != entry->chain_sha ||
      entry->seal.content_sha != entry->content_sha ||
      !NonZero(entry->seal.content_sha) || !NonZero(entry->seal.final_chain_sha))
    return Failure(entry->state, "spool_seal_metadata_invalid");
  const int fd = ::openat(root_fd_, SpoolName(id).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 || !RegularFd(fd, &observed_size) || observed_size != entry->received_bytes) {
    if (fd >= 0) ::close(fd);
    return Failure(entry->state, "spool_extent_invalid");
  }
  Sha content{};
  if (!IncrementalContentHash(fd, entry->received_bytes, &content) || content != entry->seal.content_sha) {
    ::close(fd);
    return Failure(entry->state, "spool_content_hash_invalid");
  }
  *snapshot = {};
  snapshot->stream_uuid = id;
  snapshot->stream_generation = generation;
  snapshot->authenticated_receipt_uuid = receipt;
  snapshot->descriptor_evidence = descriptor_evidence;
  snapshot->total_stream_bytes = entry->received_bytes;
  snapshot->final_chunk_count = entry->received_chunks;
  snapshot->final_chain_sha = entry->chain_sha;
  snapshot->content_sha = content;
  std::array<std::uint8_t, 1024 * 1024> buffer{};
  std::uint64_t offset = 0;
  while (offset < entry->received_bytes) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), entry->received_bytes - offset));
    if (!PreadAll(fd, buffer.data(), count, offset) || !reader(buffer.data(), count, offset)) {
      ::close(fd);
      return Failure(entry->state, "spool_reader_aborted");
    }
    offset += count;
  }
  ::close(fd);
  return Success(entry->state);
}

}  // namespace scratchbird::engine::internal_api
