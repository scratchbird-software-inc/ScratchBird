// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_prepared_statement_registry.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::size_t kSessionHeaderBytes = 160;
constexpr std::size_t kRecordHeaderBytes = 320;
constexpr std::size_t kVariableFieldCount = 13;
constexpr std::uint64_t kMaximumRegistryBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumRecordCount = 4096;
constexpr std::string_view kSessionDomain =
    "ScratchBird.SblrPreparedStatementRegistry.V1";
constexpr std::string_view kRecordDomain =
    "ScratchBird.SblrPreparedStatementRegistryRecord.V1";

std::mutex g_registry_mutex;
std::atomic<std::uint64_t> g_temp_ordinal{1};

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("OK", "ok", {}, false);
}

template <std::size_t N>
bool NonZero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool NoNul(std::string_view value, std::size_t maximum,
           bool allow_empty = false) {
  return (allow_empty || !value.empty()) && value.size() <= maximum &&
         value.find('\0') == std::string_view::npos;
}

bool Sha256Text(std::string_view value) {
  return value.size() == 71 && value.substr(0, 7) == "sha256:" &&
         std::all_of(value.begin() + 7, value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') ||
                  (byte >= 'a' && byte <= 'f');
         });
}

void SetLe(std::vector<std::uint8_t>* bytes, std::size_t offset,
           std::uint64_t value, std::size_t count) {
  for (std::size_t index = 0; index != count; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint64_t GetLe(const std::uint8_t* bytes, std::size_t count) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index != count; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <std::size_t N>
void Set(std::vector<std::uint8_t>* bytes, std::size_t offset,
         const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

template <std::size_t N>
void Get(const std::uint8_t* bytes, std::array<std::uint8_t, N>* value) {
  std::copy_n(bytes, N, value->begin());
}

bool Zero(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

SblrPreparedStatementRegistryHashV1 Hash(
    const std::vector<std::uint8_t>& bytes) {
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

SblrPreparedStatementRegistryHashV1 DomainHash(
    std::string_view domain, const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  return Hash(material);
}

bool CanonicalUuidBytes(std::string_view text,
                        SblrPreparedStatementRegistryUuidV1* bytes) {
  if (bytes == nullptr) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            bytes->begin());
  return NonZero(*bytes);
}

std::string UuidText(const SblrPreparedStatementRegistryUuidV1& value) {
  scratchbird::core::platform::Uuid uuid{};
  std::copy(value.begin(), value.end(), uuid.bytes.begin());
  return scratchbird::core::uuid::UuidToString(uuid);
}

bool HasAuthority(const EngineRequestContext& context) {
  const auto authorized =
      std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "private_prepared_statement_registry") !=
          context.trace_tags.end() ||
      std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "right:SBLR_PREPARED_STATEMENT_REGISTRY_RECOVERY") !=
          context.trace_tags.end() ||
      std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "private_prepared_statement_capability_check") !=
          context.trace_tags.end();
  SblrPreparedStatementRegistryUuidV1 database{};
  SblrPreparedStatementRegistryUuidV1 session{};
  SblrPreparedStatementRegistryUuidV1 principal{};
  return authorized && context.security_context_present &&
         !context.database_path.empty() &&
         CanonicalUuidBytes(context.database_uuid.canonical, &database) &&
         CanonicalUuidBytes(context.session_uuid.canonical, &session) &&
         CanonicalUuidBytes(context.principal_uuid.canonical, &principal);
}

std::string RegistryPath(const EngineRequestContext& context) {
  return context.database_path +
         ".sb.sblr_prepared_statement_registry.v1." +
         context.session_uuid.canonical;
}

bool ParameterStateValid(
    const SblrPreparedStatementRegistryRecordV1& record) {
  if (record.source_free_parameterless_query_template ==
      record.source_free_parameterized_query_template) {
    return false;
  }
  if (record.source_free_parameterless_query_template) {
    return record.parameter_set_uuid.empty() &&
           record.parameter_prepared_statement_uuid.empty() &&
           record.parameter_set_generation == 0 &&
           record.parameter_set_snapshot_uuid.empty() &&
           record.parameter_set_snapshot_generation == 0 &&
           record.ordered_slot_table_sha256.empty();
  }
  SblrPreparedStatementRegistryUuidV1 parameter_set{};
  SblrPreparedStatementRegistryUuidV1 prepared_statement{};
  SblrPreparedStatementRegistryUuidV1 snapshot{};
  return CanonicalUuidBytes(record.parameter_set_uuid, &parameter_set) &&
         CanonicalUuidBytes(record.parameter_prepared_statement_uuid,
                            &prepared_statement) &&
         record.parameter_set_generation != 0 &&
         CanonicalUuidBytes(record.parameter_set_snapshot_uuid, &snapshot) &&
         record.parameter_set_snapshot_generation != 0 &&
         Sha256Text(record.ordered_slot_table_sha256);
}

bool RecordSemanticValid(
    const SblrPreparedStatementRegistryRecordV1& record) {
  if (!NoNul(record.canonical_name, 1024) ||
      !NoNul(record.body_operation_id, 256) ||
      !NoNul(record.body_operation_family, 256) ||
      !NoNul(record.body_result_shape, 256) ||
      !ParameterStateValid(record) || !NonZero(record.statement_uuid) ||
      !NonZero(record.statement_name_uuid) ||
      !NonZero(record.preparing_receipt_uuid) ||
      record.prepared_generation == 0 ||
      !NonZero(record.descriptor_sha256) ||
      record.canonical_descriptor_bytes.empty() ||
      record.canonical_container_bytes.empty() ||
      record.canonical_execution_envelope_bytes.empty() ||
      record.canonical_prepare_result_bytes.size() != 160 ||
      record.canonical_descriptor_bytes.size() > 64ULL * 1024ULL * 1024ULL ||
      record.canonical_container_bytes.size() > 64ULL * 1024ULL * 1024ULL ||
      record.canonical_execution_envelope_bytes.size() >
          64ULL * 1024ULL * 1024ULL) {
    return false;
  }
  if (record.state != SblrPreparedStatementRegistryStateV1::active &&
      record.state != SblrPreparedStatementRegistryStateV1::freed &&
      record.state != SblrPreparedStatementRegistryStateV1::session_revoked) {
    return false;
  }
  const bool free_present = NonZero(record.free_descriptor_sha256) ||
                            !record.canonical_free_result_bytes.empty();
  if (free_present &&
      (!NonZero(record.free_descriptor_sha256) ||
       record.canonical_free_result_bytes.size() != 128)) {
    return false;
  }
  if (record.state == SblrPreparedStatementRegistryStateV1::active &&
      free_present) {
    return false;
  }
  if (record.state == SblrPreparedStatementRegistryStateV1::freed &&
      !free_present) {
    return false;
  }
  const bool execution_identity = NonZero(record.last_execution_uuid) ||
                                  NonZero(record.last_execution_receipt_uuid) ||
                                  NonZero(record.last_execution_transaction_uuid) ||
                                  record.last_execution_generation != 0;
  if (record.last_execution_terminal) {
    if (!NonZero(record.last_execution_uuid) ||
        !NonZero(record.last_execution_receipt_uuid) ||
        record.last_execution_generation == 0) {
      return false;
    }
  } else if (execution_identity || record.last_execution_final) {
    return false;
  }
  return true;
}

std::array<std::vector<std::uint8_t>, kVariableFieldCount> VariableFields(
    const SblrPreparedStatementRegistryRecordV1& record) {
  auto bytes = [](std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
  };
  return {bytes(record.canonical_name),
          bytes(record.body_operation_id),
          bytes(record.body_operation_family),
          bytes(record.body_result_shape),
          bytes(record.parameter_set_uuid),
          bytes(record.parameter_prepared_statement_uuid),
          bytes(record.parameter_set_snapshot_uuid),
          bytes(record.ordered_slot_table_sha256),
          record.canonical_descriptor_bytes,
          record.canonical_container_bytes,
          record.canonical_execution_envelope_bytes,
          record.canonical_prepare_result_bytes,
          record.canonical_free_result_bytes};
}

std::vector<std::uint8_t> EncodeRecord(
    const SblrPreparedStatementRegistryRecordV1& record) {
  if (!RecordSemanticValid(record) || record.record_generation == 0) return {};
  const auto fields = VariableFields(record);
  std::uint64_t total = kRecordHeaderBytes;
  for (const auto& field : fields) {
    if (field.size() > std::numeric_limits<std::uint32_t>::max() ||
        total > kMaximumRegistryBytes - field.size()) {
      return {};
    }
    total += field.size();
  }
  if (total > std::numeric_limits<std::uint32_t>::max()) return {};

  std::vector<std::uint8_t> encoded(kRecordHeaderBytes, 0);
  std::copy_n("SPRO", 4, encoded.begin());
  SetLe(&encoded, 4, 1, 2);
  SetLe(&encoded, 6, kRecordHeaderBytes, 2);
  SetLe(&encoded, 8, total, 4);
  SetLe(&encoded, 12, static_cast<std::uint32_t>(record.state), 4);
  std::uint32_t flags = record.quoted ? 1U : 0U;
  if (record.source_free_parameterless_query_template) flags |= 1U << 1U;
  if (record.source_free_parameterized_query_template) flags |= 1U << 2U;
  if (record.last_execution_terminal) flags |= 1U << 3U;
  if (record.last_execution_final) flags |= 1U << 4U;
  SetLe(&encoded, 16, flags, 4);
  SetLe(&encoded, 24, record.prepared_generation, 8);
  SetLe(&encoded, 32, record.parameter_set_generation, 8);
  SetLe(&encoded, 40, record.parameter_set_snapshot_generation, 8);
  SetLe(&encoded, 48, record.last_execution_generation, 8);
  Set(&encoded, 56, record.statement_uuid);
  Set(&encoded, 72, record.statement_name_uuid);
  Set(&encoded, 88, record.preparing_receipt_uuid);
  Set(&encoded, 104, record.last_execution_uuid);
  Set(&encoded, 120, record.last_execution_receipt_uuid);
  Set(&encoded, 136, record.last_execution_transaction_uuid);
  Set(&encoded, 152, record.descriptor_sha256);
  Set(&encoded, 184, record.free_descriptor_sha256);
  SetLe(&encoded, 216, record.record_generation, 8);
  for (std::size_t index = 0; index != fields.size(); ++index) {
    SetLe(&encoded, 224 + index * 4, fields[index].size(), 4);
  }
  for (const auto& field : fields) {
    encoded.insert(encoded.end(), field.begin(), field.end());
  }
  const auto evidence = DomainHash(kRecordDomain, encoded);
  if (NonZero(record.record_evidence_sha256) &&
      record.record_evidence_sha256 != evidence) {
    return {};
  }
  Set(&encoded, 276, evidence);
  return encoded;
}

bool DecodeRecord(const std::uint8_t* data, std::size_t size,
                  SblrPreparedStatementRegistryRecordV1* record) {
  if (record == nullptr || size < kRecordHeaderBytes ||
      !std::equal(data, data + 4, "SPRO") || GetLe(data + 4, 2) != 1 ||
      GetLe(data + 6, 2) != kRecordHeaderBytes ||
      GetLe(data + 8, 4) != size || !Zero(data + 20, data + 24) ||
      !Zero(data + 308, data + 320)) {
    return false;
  }
  const auto flags = GetLe(data + 16, 4);
  if ((flags & ~0x1fULL) != 0) return false;
  SblrPreparedStatementRegistryRecordV1 value;
  const auto state = GetLe(data + 12, 4);
  if (state < 1 || state > 3) return false;
  value.state = static_cast<SblrPreparedStatementRegistryStateV1>(state);
  value.quoted = (flags & 1U) != 0;
  value.source_free_parameterless_query_template = (flags & (1U << 1U)) != 0;
  value.source_free_parameterized_query_template = (flags & (1U << 2U)) != 0;
  value.last_execution_terminal = (flags & (1U << 3U)) != 0;
  value.last_execution_final = (flags & (1U << 4U)) != 0;
  value.prepared_generation = GetLe(data + 24, 8);
  value.parameter_set_generation = GetLe(data + 32, 8);
  value.parameter_set_snapshot_generation = GetLe(data + 40, 8);
  value.last_execution_generation = GetLe(data + 48, 8);
  Get(data + 56, &value.statement_uuid);
  Get(data + 72, &value.statement_name_uuid);
  Get(data + 88, &value.preparing_receipt_uuid);
  Get(data + 104, &value.last_execution_uuid);
  Get(data + 120, &value.last_execution_receipt_uuid);
  Get(data + 136, &value.last_execution_transaction_uuid);
  Get(data + 152, &value.descriptor_sha256);
  Get(data + 184, &value.free_descriptor_sha256);
  value.record_generation = GetLe(data + 216, 8);
  Get(data + 276, &value.record_evidence_sha256);

  std::array<std::uint64_t, kVariableFieldCount> lengths{};
  std::uint64_t expected = kRecordHeaderBytes;
  for (std::size_t index = 0; index != lengths.size(); ++index) {
    lengths[index] = GetLe(data + 224 + index * 4, 4);
    if (expected > size || lengths[index] > size - expected) return false;
    expected += lengths[index];
  }
  if (expected != size) return false;
  std::array<std::vector<std::uint8_t>, kVariableFieldCount> fields;
  std::size_t offset = kRecordHeaderBytes;
  for (std::size_t index = 0; index != fields.size(); ++index) {
    fields[index].assign(data + offset, data + offset + lengths[index]);
    offset += static_cast<std::size_t>(lengths[index]);
  }
  auto text = [](const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
  };
  value.canonical_name = text(fields[0]);
  value.body_operation_id = text(fields[1]);
  value.body_operation_family = text(fields[2]);
  value.body_result_shape = text(fields[3]);
  value.parameter_set_uuid = text(fields[4]);
  value.parameter_prepared_statement_uuid = text(fields[5]);
  value.parameter_set_snapshot_uuid = text(fields[6]);
  value.ordered_slot_table_sha256 = text(fields[7]);
  value.canonical_descriptor_bytes = std::move(fields[8]);
  value.canonical_container_bytes = std::move(fields[9]);
  value.canonical_execution_envelope_bytes = std::move(fields[10]);
  value.canonical_prepare_result_bytes = std::move(fields[11]);
  value.canonical_free_result_bytes = std::move(fields[12]);

  std::vector<std::uint8_t> evidence_material(data, data + size);
  std::fill(evidence_material.begin() + 276,
            evidence_material.begin() + 308, 0);
  if (!RecordSemanticValid(value) || value.record_generation == 0 ||
      value.record_evidence_sha256 !=
          DomainHash(kRecordDomain, evidence_material) ||
      EncodeRecord(value) != std::vector<std::uint8_t>(data, data + size)) {
    return false;
  }
  *record = std::move(value);
  return true;
}

std::vector<std::uint8_t> EncodeSnapshot(
    const SblrPreparedStatementRegistrySnapshotV1& input) {
  if (!NonZero(input.database_uuid) || !NonZero(input.session_uuid) ||
      !NonZero(input.principal_uuid) ||
      input.registry_generation == 0 ||
      input.records.size() > kMaximumRecordCount) {
    return {};
  }
  auto records = input.records;
  std::sort(records.begin(), records.end(), [](const auto& left,
                                                const auto& right) {
    return left.canonical_name < right.canonical_name;
  });
  std::vector<std::uint8_t> payload;
  std::string prior_name;
  std::vector<SblrPreparedStatementRegistryUuidV1> statement_uuids;
  for (const auto& record : records) {
    if ((input.session_revoked &&
         record.state !=
             SblrPreparedStatementRegistryStateV1::session_revoked) ||
        (!input.session_revoked &&
         record.state ==
             SblrPreparedStatementRegistryStateV1::session_revoked)) {
      return {};
    }
    if ((!prior_name.empty() && record.canonical_name <= prior_name) ||
        std::find(statement_uuids.begin(), statement_uuids.end(),
                  record.statement_uuid) != statement_uuids.end()) {
      return {};
    }
    const auto encoded = EncodeRecord(record);
    if (encoded.empty() ||
        payload.size() > kMaximumRegistryBytes - encoded.size()) {
      return {};
    }
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    prior_name = record.canonical_name;
    statement_uuids.push_back(record.statement_uuid);
  }
  if (kSessionHeaderBytes + payload.size() > kMaximumRegistryBytes ||
      kSessionHeaderBytes + payload.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  std::vector<std::uint8_t> encoded(kSessionHeaderBytes, 0);
  const std::array<std::uint8_t, 8> magic{'S', 'B', 'P', 'S', 'R', 'G', '1', 0};
  Set(&encoded, 0, magic);
  SetLe(&encoded, 8, 1, 2);
  SetLe(&encoded, 10, kSessionHeaderBytes, 2);
  SetLe(&encoded, 12, kSessionHeaderBytes + payload.size(), 4);
  SetLe(&encoded, 16, input.session_revoked ? 1 : 0, 4);
  SetLe(&encoded, 20, records.size(), 4);
  SetLe(&encoded, 24, input.registry_generation, 8);
  Set(&encoded, 32, input.database_uuid);
  Set(&encoded, 48, input.session_uuid);
  SetLe(&encoded, 64, payload.size(), 8);
  Set(&encoded, 72, Hash(payload));
  Set(&encoded, 136, input.principal_uuid);
  encoded.insert(encoded.end(), payload.begin(), payload.end());
  const auto evidence = DomainHash(kSessionDomain, encoded);
  if (NonZero(input.record_evidence_sha256) &&
      input.record_evidence_sha256 != evidence) {
    return {};
  }
  Set(&encoded, 104, evidence);
  return encoded;
}

bool DecodeSnapshot(const std::vector<std::uint8_t>& bytes,
                    SblrPreparedStatementRegistrySnapshotV1* snapshot) {
  const std::array<std::uint8_t, 8> magic{'S', 'B', 'P', 'S', 'R', 'G', '1', 0};
  if (snapshot == nullptr || bytes.size() < kSessionHeaderBytes ||
      bytes.size() > kMaximumRegistryBytes ||
      !std::equal(magic.begin(), magic.end(), bytes.begin()) ||
      GetLe(bytes.data() + 8, 2) != 1 ||
      GetLe(bytes.data() + 10, 2) != kSessionHeaderBytes ||
      GetLe(bytes.data() + 12, 4) != bytes.size() ||
      (GetLe(bytes.data() + 16, 4) & ~1ULL) != 0 ||
      GetLe(bytes.data() + 20, 4) > kMaximumRecordCount ||
      GetLe(bytes.data() + 64, 8) != bytes.size() - kSessionHeaderBytes ||
      !Zero(bytes.data() + 152, bytes.data() + 160)) {
    return false;
  }
  SblrPreparedStatementRegistrySnapshotV1 value;
  value.session_revoked = GetLe(bytes.data() + 16, 4) != 0;
  const auto count = GetLe(bytes.data() + 20, 4);
  value.registry_generation = GetLe(bytes.data() + 24, 8);
  Get(bytes.data() + 32, &value.database_uuid);
  Get(bytes.data() + 48, &value.session_uuid);
  Get(bytes.data() + 136, &value.principal_uuid);
  SblrPreparedStatementRegistryHashV1 payload_hash{};
  Get(bytes.data() + 72, &payload_hash);
  Get(bytes.data() + 104, &value.record_evidence_sha256);
  std::vector<std::uint8_t> payload(bytes.begin() + kSessionHeaderBytes,
                                    bytes.end());
  auto evidence_material = bytes;
  std::fill(evidence_material.begin() + 104,
            evidence_material.begin() + 136, 0);
  if (!NonZero(value.database_uuid) || !NonZero(value.session_uuid) ||
      !NonZero(value.principal_uuid) ||
      value.registry_generation == 0 || payload_hash != Hash(payload) ||
      value.record_evidence_sha256 !=
          DomainHash(kSessionDomain, evidence_material)) {
    return false;
  }
  std::size_t offset = kSessionHeaderBytes;
  std::string prior_name;
  for (std::uint64_t index = 0; index != count; ++index) {
    if (offset > bytes.size() || bytes.size() - offset < kRecordHeaderBytes) {
      return false;
    }
    const auto size = GetLe(bytes.data() + offset + 8, 4);
    if (size < kRecordHeaderBytes || size > bytes.size() - offset) return false;
    SblrPreparedStatementRegistryRecordV1 record;
    if (!DecodeRecord(bytes.data() + offset, static_cast<std::size_t>(size),
                      &record) ||
        (!prior_name.empty() && record.canonical_name <= prior_name)) {
      return false;
    }
    if (std::any_of(value.records.begin(), value.records.end(),
                    [&](const auto& existing) {
                      return existing.statement_uuid == record.statement_uuid;
                    })) {
      return false;
    }
    prior_name = record.canonical_name;
    value.records.push_back(std::move(record));
    offset += static_cast<std::size_t>(size);
  }
  if (offset != bytes.size() || EncodeSnapshot(value) != bytes) return false;
  *snapshot = std::move(value);
  return true;
}

enum class ReadStatus { absent, ok, invalid, io_error };

ReadStatus ReadFile(const std::string& path,
                    std::vector<std::uint8_t>* bytes) {
  bytes->clear();
#if defined(_WIN32)
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return std::filesystem::exists(path) ? ReadStatus::io_error
                                         : ReadStatus::absent;
  }
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) > kMaximumRegistryBytes) {
    return ReadStatus::invalid;
  }
  bytes->resize(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes->data()), end);
  return input ? ReadStatus::ok : ReadStatus::io_error;
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT ? ReadStatus::absent
                                     : ReadStatus::io_error;
  struct stat metadata {};
  if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uint64_t>(metadata.st_size) > kMaximumRegistryBytes) {
    ::close(fd);
    return ReadStatus::invalid;
  }
  bytes->resize(static_cast<std::size_t>(metadata.st_size));
  std::size_t offset = 0;
  while (offset != bytes->size()) {
    const auto count = ::read(fd, bytes->data() + offset,
                              bytes->size() - offset);
    if (count <= 0) {
      ::close(fd);
      return ReadStatus::io_error;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::close(fd) != 0) return ReadStatus::io_error;
  return ReadStatus::ok;
#endif
}

bool SyncParent(const std::string& path) {
#if defined(_WIN32)
  (void)path;
  return true;
#else
  auto parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) parent = ".";
  const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

bool WriteNewFile(const std::string& path,
                  const std::vector<std::uint8_t>& bytes) {
#if defined(_WIN32)
  if (std::filesystem::exists(path)) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  output.flush();
  return static_cast<bool>(output);
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                          O_CLOEXEC | O_NOFOLLOW,
                        0600);
  if (fd < 0) return false;
  std::size_t offset = 0;
  bool ok = true;
  while (offset != bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset,
                               bytes.size() - offset);
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  ok = ok && ::fsync(fd) == 0;
  if (::close(fd) != 0) ok = false;
  if (!ok) (void)::unlink(path.c_str());
  return ok;
#endif
}

bool ReplaceFile(const std::string& path,
                 const std::vector<std::uint8_t>& bytes) {
  const auto ordinal = g_temp_ordinal.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  const auto process = static_cast<std::uint64_t>(::getpid());
#endif
  const auto temporary = path + ".tmp." + std::to_string(process) + "." +
                         std::to_string(ordinal);
  if (!WriteNewFile(temporary, bytes)) return false;
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return SyncParent(path);
}

SblrPreparedStatementRegistryResultV1 Refused(
    std::string code, std::string key, std::string detail = {}) {
  SblrPreparedStatementRegistryResultV1 result;
  result.diagnostic =
      Diagnostic(std::move(code), std::move(key), std::move(detail));
  return result;
}

SblrPreparedStatementRegistryResultV1 Loaded(
    SblrPreparedStatementRegistrySnapshotV1 snapshot, bool found = true) {
  SblrPreparedStatementRegistryResultV1 result;
  result.ok = true;
  result.found = found;
  result.diagnostic = OkDiagnostic();
  result.snapshot = std::move(snapshot);
  return result;
}

SblrPreparedStatementRegistryResultV1 LoadExact(
    const EngineRequestContext& context) {
  std::vector<std::uint8_t> bytes;
  const auto status = ReadFile(RegistryPath(context), &bytes);
  if (status == ReadStatus::absent) return Loaded({}, false);
  SblrPreparedStatementRegistrySnapshotV1 snapshot;
  if (status != ReadStatus::ok || !DecodeSnapshot(bytes, &snapshot)) {
    return Refused("CATALOG.SNAPSHOT_STALE",
                   "sblr.prepared_statement_registry.corrupt",
                   "prepared-statement registry is torn or noncanonical");
  }
  SblrPreparedStatementRegistryUuidV1 database{};
  SblrPreparedStatementRegistryUuidV1 session{};
  SblrPreparedStatementRegistryUuidV1 principal{};
  if (!CanonicalUuidBytes(context.database_uuid.canonical, &database) ||
      !CanonicalUuidBytes(context.session_uuid.canonical, &session) ||
      !CanonicalUuidBytes(context.principal_uuid.canonical, &principal) ||
      snapshot.database_uuid != database || snapshot.session_uuid != session ||
      snapshot.principal_uuid != principal) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.identity_hidden");
  }
  return Loaded(std::move(snapshot));
}

bool SamePreparedRecord(const SblrPreparedStatementRegistryRecordV1& left,
                        const SblrPreparedStatementRegistryRecordV1& right) {
  auto normalized_left = left;
  auto normalized_right = right;
  normalized_left.state = SblrPreparedStatementRegistryStateV1::active;
  normalized_right.state = SblrPreparedStatementRegistryStateV1::active;
  normalized_left.free_descriptor_sha256 = {};
  normalized_right.free_descriptor_sha256 = {};
  normalized_left.canonical_free_result_bytes.clear();
  normalized_right.canonical_free_result_bytes.clear();
  normalized_left.last_execution_uuid = {};
  normalized_right.last_execution_uuid = {};
  normalized_left.last_execution_receipt_uuid = {};
  normalized_right.last_execution_receipt_uuid = {};
  normalized_left.last_execution_transaction_uuid = {};
  normalized_right.last_execution_transaction_uuid = {};
  normalized_left.last_execution_generation = 0;
  normalized_right.last_execution_generation = 0;
  normalized_left.last_execution_terminal = false;
  normalized_right.last_execution_terminal = false;
  normalized_left.last_execution_final = false;
  normalized_right.last_execution_final = false;
  normalized_left.record_generation = 1;
  normalized_right.record_generation = 1;
  normalized_left.record_evidence_sha256 = {};
  normalized_right.record_evidence_sha256 = {};
  return EncodeRecord(normalized_left) == EncodeRecord(normalized_right);
}

bool WriteSnapshot(const EngineRequestContext& context,
                   SblrPreparedStatementRegistrySnapshotV1* snapshot) {
  snapshot->record_evidence_sha256 = {};
  const auto encoded = EncodeSnapshot(*snapshot);
  if (encoded.empty() || !ReplaceFile(RegistryPath(context), encoded)) {
    return false;
  }
  SblrPreparedStatementRegistrySnapshotV1 canonical;
  if (!DecodeSnapshot(encoded, &canonical)) return false;
  *snapshot = std::move(canonical);
  return true;
}

}  // namespace

SblrPreparedStatementRegistryResultV1 LoadSblrPreparedStatementRegistryV1(
    const EngineRequestContext& context) {
  std::lock_guard lock(g_registry_mutex);
  if (!HasAuthority(context)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.load_denied");
  }
  return LoadExact(context);
}

SblrPreparedStatementRegistryResultV1
ResolveActiveSblrPreparedStatementCapabilityV1(
    const EngineRequestContext& context,
    const std::string& prepared_statement_uuid,
    std::uint64_t prepared_generation) {
  std::lock_guard lock(g_registry_mutex);
  if (!HasAuthority(context)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.capability_denied");
  }
  SblrPreparedStatementRegistryUuidV1 prepared_uuid{};
  if (!CanonicalUuidBytes(prepared_statement_uuid, &prepared_uuid) ||
      prepared_generation == 0) {
    return Refused("SBLR.OPERAND.INVALID",
                   "sblr.prepared_statement_registry.capability_invalid");
  }
  auto loaded = LoadExact(context);
  if (!loaded.ok) return loaded;
  if (!loaded.found || loaded.snapshot.session_revoked) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.capability_hidden");
  }
  const SblrPreparedStatementRegistryRecordV1* match = nullptr;
  for (const auto& record : loaded.snapshot.records) {
    const bool statement_match = record.statement_uuid == prepared_uuid;
    const bool parameter_match =
        record.parameter_prepared_statement_uuid == prepared_statement_uuid;
    if (!statement_match && !parameter_match) continue;
    if (match != nullptr) {
      return Refused("CATALOG.SNAPSHOT_STALE",
                     "sblr.prepared_statement_registry.capability_ambiguous");
    }
    match = &record;
  }
  if (match == nullptr ||
      match->state != SblrPreparedStatementRegistryStateV1::active) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.capability_hidden");
  }
  if (match->prepared_generation != prepared_generation) {
    return Refused("MGA.TRANSACTION.STALE",
                   "sblr.prepared_statement_registry.capability_stale");
  }
  const auto record = *match;
  auto result = Loaded(std::move(loaded.snapshot));
  result.record = record;
  return result;
}

SblrPreparedStatementRegistryResultV1 PublishSblrPreparedStatementV1(
    const EngineRequestContext& context,
    const SblrPreparedStatementRegistryRecordV1& input) {
  std::lock_guard lock(g_registry_mutex);
  if (!HasAuthority(context)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.publish_denied");
  }
  if (!RecordSemanticValid(input) ||
      input.state != SblrPreparedStatementRegistryStateV1::active ||
      input.record_generation != 0 || NonZero(input.record_evidence_sha256)) {
    return Refused("SBLR.OPERAND.INVALID",
                   "sblr.prepared_statement_registry.record_invalid");
  }
  auto loaded = LoadExact(context);
  if (!loaded.ok) return loaded;
  auto snapshot = std::move(loaded.snapshot);
  if (!loaded.found) {
    if (!CanonicalUuidBytes(context.database_uuid.canonical,
                            &snapshot.database_uuid) ||
        !CanonicalUuidBytes(context.session_uuid.canonical,
                            &snapshot.session_uuid) ||
        !CanonicalUuidBytes(context.principal_uuid.canonical,
                            &snapshot.principal_uuid)) {
      return Refused("SBLR.OPERAND.INVALID",
                     "sblr.prepared_statement_registry.context_invalid");
    }
  }
  if (snapshot.session_revoked) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.session_revoked");
  }
  const auto existing = std::find_if(
      snapshot.records.begin(), snapshot.records.end(), [&](const auto& row) {
        return row.canonical_name == input.canonical_name;
      });
  if (existing != snapshot.records.end()) {
    if (existing->state == SblrPreparedStatementRegistryStateV1::active &&
        SamePreparedRecord(*existing, input)) {
      const auto replay_record = *existing;
      auto result = Loaded(std::move(snapshot));
      result.exact_replay = true;
      result.record = replay_record;
      return result;
    }
    return Refused("MGA.TRANSACTION.STALE",
                   "sblr.prepared_statement_registry.publication_conflict");
  }
  if (std::any_of(snapshot.records.begin(), snapshot.records.end(),
                  [&](const auto& row) {
                    return row.statement_uuid == input.statement_uuid;
                  })) {
    return Refused("MGA.TRANSACTION.STALE",
                   "sblr.prepared_statement_registry.identity_conflict");
  }
  if (snapshot.registry_generation == std::numeric_limits<std::uint64_t>::max()) {
    return Refused("SBLR.EXECUTION_FAILED",
                   "sblr.prepared_statement_registry.generation_exhausted");
  }
  ++snapshot.registry_generation;
  auto record = input;
  record.record_generation = snapshot.registry_generation;
  snapshot.records.push_back(record);
  if (!WriteSnapshot(context, &snapshot)) {
    return Refused("SBLR.EXECUTION_FAILED",
                   "sblr.prepared_statement_registry.publish_failed",
                   "durable prepared-statement publication failed");
  }
  auto result = Loaded(std::move(snapshot));
  const auto published = std::find_if(
      result.snapshot.records.begin(), result.snapshot.records.end(),
      [&](const auto& row) { return row.canonical_name == input.canonical_name; });
  if (published == result.snapshot.records.end()) {
    return Refused("SBLR.EXECUTION_FAILED",
                   "sblr.prepared_statement_registry.publish_lost");
  }
  result.record = *published;
  return result;
}

SblrPreparedStatementRegistryResultV1 FreeSblrPreparedStatementV1(
    const EngineRequestContext& context,
    const std::string& canonical_name,
    const SblrPreparedStatementRegistryUuidV1& statement_uuid,
    std::uint64_t prepared_generation,
    const SblrPreparedStatementRegistryHashV1& prepared_descriptor_sha256,
    const SblrPreparedStatementRegistryHashV1& free_descriptor_sha256,
    const std::vector<std::uint8_t>& canonical_free_result_bytes) {
  std::lock_guard lock(g_registry_mutex);
  if (!HasAuthority(context)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.free_denied");
  }
  if (!NoNul(canonical_name, 1024) || !NonZero(statement_uuid) ||
      prepared_generation == 0 || !NonZero(prepared_descriptor_sha256) ||
      !NonZero(free_descriptor_sha256) ||
      canonical_free_result_bytes.size() != 128) {
    return Refused("SBLR.OPERAND.INVALID",
                   "sblr.prepared_statement_registry.free_invalid");
  }
  auto loaded = LoadExact(context);
  if (!loaded.ok) return loaded;
  if (!loaded.found || loaded.snapshot.session_revoked) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.statement_hidden");
  }
  auto snapshot = std::move(loaded.snapshot);
  const auto found = std::find_if(
      snapshot.records.begin(), snapshot.records.end(), [&](const auto& row) {
        return row.canonical_name == canonical_name;
      });
  if (found == snapshot.records.end()) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.statement_hidden");
  }
  if (found->statement_uuid != statement_uuid ||
      found->prepared_generation != prepared_generation ||
      found->descriptor_sha256 != prepared_descriptor_sha256) {
    return Refused("MGA.TRANSACTION.STALE",
                   "sblr.prepared_statement_registry.free_stale");
  }
  if (found->state == SblrPreparedStatementRegistryStateV1::freed) {
    if (found->free_descriptor_sha256 != free_descriptor_sha256 ||
        found->canonical_free_result_bytes != canonical_free_result_bytes) {
      return Refused("MGA.TRANSACTION.STALE",
                     "sblr.prepared_statement_registry.free_replay_conflict");
    }
    const auto replay_record = *found;
    auto result = Loaded(std::move(snapshot));
    result.exact_replay = true;
    result.record = replay_record;
    return result;
  }
  if (found->state != SblrPreparedStatementRegistryStateV1::active ||
      snapshot.registry_generation == std::numeric_limits<std::uint64_t>::max()) {
    return Refused("MGA.TRANSACTION.STALE",
                   "sblr.prepared_statement_registry.free_terminal");
  }
  ++snapshot.registry_generation;
  found->state = SblrPreparedStatementRegistryStateV1::freed;
  found->free_descriptor_sha256 = free_descriptor_sha256;
  found->canonical_free_result_bytes = canonical_free_result_bytes;
  found->record_generation = snapshot.registry_generation;
  found->record_evidence_sha256 = {};
  if (!WriteSnapshot(context, &snapshot)) {
    return Refused("SBLR.EXECUTION_FAILED",
                   "sblr.prepared_statement_registry.free_publish_failed",
                   "durable prepared-statement revocation failed");
  }
  auto result = Loaded(std::move(snapshot));
  const auto terminal = std::find_if(
      result.snapshot.records.begin(), result.snapshot.records.end(),
      [&](const auto& row) { return row.canonical_name == canonical_name; });
  if (terminal == result.snapshot.records.end()) {
    return Refused("SBLR.EXECUTION_FAILED",
                   "sblr.prepared_statement_registry.free_publish_lost");
  }
  result.record = *terminal;
  return result;
}

SblrPreparedStatementRegistryResultV1 RevokeSblrPreparedStatementSessionV1(
    const EngineRequestContext& context) {
  std::lock_guard lock(g_registry_mutex);
  if (!HasAuthority(context)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.prepared_statement_registry.revoke_denied");
  }
  auto loaded = LoadExact(context);
  if (!loaded.ok || !loaded.found) return loaded;
  auto snapshot = std::move(loaded.snapshot);
  if (snapshot.session_revoked) {
    auto result = Loaded(std::move(snapshot));
    result.exact_replay = true;
    return result;
  }
  if (snapshot.registry_generation == std::numeric_limits<std::uint64_t>::max()) {
    return Refused("SBLR.EXECUTION_FAILED",
                   "sblr.prepared_statement_registry.generation_exhausted");
  }
  ++snapshot.registry_generation;
  snapshot.session_revoked = true;
  for (auto& record : snapshot.records) {
    record.state = SblrPreparedStatementRegistryStateV1::session_revoked;
    record.record_generation = snapshot.registry_generation;
    record.record_evidence_sha256 = {};
  }
  if (!WriteSnapshot(context, &snapshot)) {
    return Refused("SBLR.EXECUTION_FAILED",
                   "sblr.prepared_statement_registry.revoke_publish_failed",
                   "durable logical-session revocation failed");
  }
  return Loaded(std::move(snapshot));
}

}  // namespace scratchbird::engine::internal_api
