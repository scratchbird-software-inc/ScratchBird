// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_ddl_create_schema_execution_journal.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_store.hpp"
#include "ddl/create_api.hpp"
#include "hash_digest.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "security/authorization_api.hpp"
#include "storage/database/local_transaction_store.hpp"
#include "transaction/mga/transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::size_t kHeaderBytes = 224;
constexpr std::size_t kDescriptorBytes = 488;
constexpr std::size_t kResultBytes = 320;
constexpr std::size_t kRecordBytes =
    kHeaderBytes + kDescriptorBytes + kResultBytes;
constexpr std::string_view kRecordDomain =
    "ScratchBird.SblrDdlCreateSchemaExecutionJournalRecord.V1";
constexpr std::string_view kJournalTraceTag =
    "private_ddl_create_schema_execution_journal";
constexpr std::string_view kRecoveryTraceTag =
    "private_ddl_create_schema_authenticated_recovery";

std::mutex g_journal_mutex;
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

template <std::size_t N>
void Put(std::vector<std::uint8_t>* bytes,
         const std::array<std::uint8_t, N>& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void PutLe(std::vector<std::uint8_t>* bytes, std::uint64_t value,
           std::size_t count) {
  for (std::size_t index = 0; index != count; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
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
void Get(const std::uint8_t* bytes, std::array<std::uint8_t, N>* value) {
  std::copy_n(bytes, N, value->begin());
}

bool Zero(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

SblrDdlCreateSchemaJournalHashV1 Hash(const std::uint8_t* bytes,
                                      std::size_t size) {
  std::vector<std::uint8_t> material;
  if (size != 0) material.assign(bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

SblrDdlCreateSchemaJournalHashV1 RecordHash(
    const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> material(kRecordDomain.begin(),
                                     kRecordDomain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  return Hash(material.data(), material.size());
}

std::string UuidText(const SblrDdlCreateSchemaJournalUuidV1& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text.push_back('-');
    }
    text.push_back(kHex[value[index] >> 4U]);
    text.push_back(kHex[value[index] & 0x0fU]);
  }
  return text;
}

bool ParseUuidText(const std::string& text,
                   SblrDdlCreateSchemaJournalUuidV1* value) {
  if (value == nullptr || text.size() != 36 || text[8] != '-' ||
      text[13] != '-' || text[18] != '-' || text[23] != '-') {
    return false;
  }
  const auto nibble = [](char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
  };
  SblrDdlCreateSchemaJournalUuidV1 parsed{};
  std::size_t byte = 0;
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '-') {
      ++index;
      continue;
    }
    if (index + 1 >= text.size() || byte >= parsed.size()) return false;
    const auto high = nibble(text[index]);
    const auto low = nibble(text[index + 1]);
    if (high < 0 || low < 0) return false;
    parsed[byte++] = static_cast<std::uint8_t>((high << 4) | low);
    index += 2;
  }
  if (byte != parsed.size() || !NonZero(parsed)) return false;
  *value = parsed;
  return true;
}

bool DecodeDescriptor(
    const SblrDdlCreateSchemaJournalKeyV1& key,
    scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1* descriptor) {
  if (descriptor == nullptr || key.canonical_descriptor_bytes.size() !=
                                   kDescriptorBytes ||
      !NonZero(key.database_uuid) || !NonZero(key.recovery_uuid)) {
    return false;
  }
  std::string detail;
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 decoded;
  if (!scratchbird::engine::sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
          key.canonical_descriptor_bytes.data(),
          key.canonical_descriptor_bytes.size(), &decoded, &detail, true) ||
      scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaDescriptorV1(
          decoded, true) != key.canonical_descriptor_bytes ||
      decoded.database_uuid != key.database_uuid ||
      decoded.recovery_uuid != key.recovery_uuid) {
    return false;
  }
  *descriptor = decoded;
  return true;
}

bool HasAuthority(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1&
        descriptor) {
  return context.security_context_present &&
         context.statement_metadata_snapshot_engine_owned &&
         !context.database_path.empty() && !context.cluster_authority_available &&
         !context.cluster_transaction_active && !context.route_fence_present &&
         context.database_uuid.canonical == UuidText(descriptor.database_uuid) &&
         context.statement_receipt_uuid.canonical == UuidText(descriptor.receipt) &&
         context.transaction_uuid.canonical ==
             UuidText(descriptor.owning_transaction_uuid) &&
         context.local_transaction_id ==
             descriptor.owning_local_transaction_id &&
         context.statement_snapshot_uuid.canonical ==
             UuidText(descriptor.statement_snapshot_uuid) &&
         context.catalog_epoch_uuid.canonical ==
             UuidText(descriptor.catalog_epoch_uuid) &&
         context.catalog_generation_id == descriptor.catalog_generation &&
         context.security_epoch == descriptor.security_epoch &&
         context.authorization_context.present &&
         context.authorization_context.authority_uuid.canonical ==
             UuidText(descriptor.security_context_uuid) &&
         context.authorization_context.security_epoch ==
             descriptor.security_epoch &&
         context.transaction_policy_snapshot_uuid.canonical ==
             UuidText(descriptor.policy_snapshot_uuid) &&
         context.transaction_policy_snapshot_generation ==
             descriptor.policy_generation &&
         context.resource_admission_uuid.canonical ==
             UuidText(descriptor.resource_grant_uuid) &&
         context.resource_epoch == descriptor.resource_generation &&
         context.principal_uuid.canonical ==
             UuidText(descriptor.owner_principal_uuid) &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(),
                   kJournalTraceTag) != context.trace_tags.end();
}

SblrExecutorAvailabilityRowIdentity CreateSchemaExecutorIdentity() {
  return {kSblrDdlCreateSchemaExecutorId,
          kSblrDdlCreateSchemaOpcodeCode,
          kSblrDdlCreateSchemaOpcodeVersion,
          kSblrDdlCreateSchemaOperandDescriptorId,
          kSblrDdlCreateSchemaResultDescriptorId,
          kSblrDdlCreateSchemaResultDescriptorVersion};
}

bool RecoverySessionAuthenticated(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1&
        descriptor) {
  return context.security_context_present &&
         context.authorization_context.present &&
         !context.database_path.empty() &&
         context.database_uuid.canonical == UuidText(descriptor.database_uuid) &&
         context.principal_uuid.canonical ==
             UuidText(descriptor.owner_principal_uuid) &&
         !context.cluster_authority_available &&
         !context.cluster_transaction_active && !context.route_fence_present;
}

bool ResultMatches(
    const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1&
        descriptor,
    const SblrDdlCreateSchemaJournalSnapshotV1& snapshot,
    scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1* decoded_result);

bool NormalizeRecoveryPath(
    const scratchbird::engine::sblr::SblrDdlCreateSchemaRequestV1& request,
    std::string* canonical_path, std::string* leaf_name,
    SblrDdlCreateSchemaJournalHashV1* path_sha256) {
  if (canonical_path == nullptr || leaf_name == nullptr ||
      path_sha256 == nullptr || request.name_atoms.empty() ||
      request.name_atoms.size() > 3) {
    return false;
  }
  canonical_path->clear();
  leaf_name->clear();
  for (const auto& atom : request.name_atoms) {
    if (atom.raw_utf8.empty() || atom.raw_utf8.size() > 256 ||
        atom.raw_utf8.find('\0') != std::string::npos ||
        atom.raw_utf8.find('.') != std::string::npos) {
      return false;
    }
    std::string normalized = atom.raw_utf8;
    if (!atom.quoted) {
      std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                     [](unsigned char value) {
                       return value < 0x80
                                  ? static_cast<char>(std::tolower(value))
                                  : static_cast<char>(value);
                     });
    }
    if (!canonical_path->empty()) canonical_path->push_back('.');
    canonical_path->append(normalized);
    *leaf_name = std::move(normalized);
  }
  constexpr std::string_view kDomain =
      "ScratchBird.SblrDdlCreateSchemaNormalizedPath.V1";
  std::vector<std::uint8_t> material(kDomain.begin(), kDomain.end());
  material.insert(material.end(), canonical_path->begin(),
                  canonical_path->end());
  material.push_back(0);
  *path_sha256 =
      scratchbird::core::hash::ComputeSha256Digest(material).digest;
  return NonZero(*path_sha256);
}

bool ExactRecoveryPostcondition(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1&
        descriptor,
    const SblrDdlCreateSchemaJournalSnapshotV1& snapshot,
    const std::string& canonical_path, const std::string& leaf_name,
    std::uint64_t observer_transaction_id, std::string* detail) {
  const auto mismatch = [&](std::string value) {
    if (detail != nullptr) *detail = std::move(value);
    return false;
  };
  scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1 result;
  if (!ResultMatches(descriptor, snapshot, &result)) {
    return mismatch("journal_result_mismatch");
  }
  const auto schema_uuid = UuidText(descriptor.schema_uuid);
  const auto parent_uuid = NonZero(descriptor.parent_schema_uuid)
                               ? UuidText(descriptor.parent_schema_uuid)
                               : std::string{};
  const auto recovery_uuid = UuidText(descriptor.recovery_uuid);
  const std::vector<EngineLocalizedName> names{
      {"en", "primary", canonical_path, leaf_name, true}};
  auto expected_payload = SchemaTreePayload(parent_uuid, names, {});
  const std::array<std::string, 5> extensions{
      "catalog_ddl_mutation_audit=" + recovery_uuid,
      "catalog_ddl_recovery_operation=" + recovery_uuid,
      "catalog_ddl_result_row_uuid=" + UuidText(result.catalog_row_uuid),
      "catalog_ddl_mutation_uuid=" + UuidText(result.mutation_uuid),
      "catalog_ddl_statement_publication_barrier=" +
          UuidText(result.publication_barrier)};
  for (const auto& extension : extensions) {
    if (!expected_payload.empty()) expected_payload.push_back(';');
    expected_payload.append(extension);
  }

  const auto schemas = VisibleSchemaTreeRecords(context,
                                                 observer_transaction_id);
  std::size_t exact_schema_count = 0;
  for (const auto& schema : schemas) {
    if (schema.schema_uuid != schema_uuid) continue;
    if (schema.creator_tx != descriptor.owning_local_transaction_id ||
        schema.parent_schema_uuid != parent_uuid ||
        schema.default_name != leaf_name || schema.localized_names.size() != 1 ||
        schema.localized_names.front().language_tag != "en" ||
        schema.localized_names.front().name_class != "primary" ||
        schema.localized_names.front().path != canonical_path ||
        schema.localized_names.front().name != leaf_name ||
        !schema.localized_names.front().default_name ||
        !schema.localized_comments.empty() ||
        schema.payload != expected_payload || schema.state != "active") {
      return mismatch("schema_record_mismatch");
    }
    ++exact_schema_count;
  }
  if (exact_schema_count != 1) return mismatch("schema_record_count_mismatch");

  const auto loaded_names =
      LoadNameRegistryState(context, observer_transaction_id);
  if (!loaded_names.ok) return mismatch("name_registry_load_failed");
  const auto expected_path_lookup_key = NameRegistryLookupKey(
      canonical_path, context.identifier_profile_uuid, false);
  std::size_t exact_name_count = 0;
  for (const auto& name : loaded_names.state.entries) {
    if (name.object_uuid != schema_uuid || name.object_class != "schema" ||
        name.deleted) {
      continue;
    }
    if (name.creator_tx != descriptor.owning_local_transaction_id)
      return mismatch("name_registry_creator_transaction_mismatch");
    if (name.scope_uuid != parent_uuid)
      return mismatch("name_registry_scope_mismatch");
    if (name.parent_schema_uuid != parent_uuid)
      return mismatch("name_registry_parent_mismatch");
    if (name.language_tag != "en")
      return mismatch("name_registry_language_mismatch");
    if (name.name_class != "primary")
      return mismatch("name_registry_class_mismatch");
    if (name.raw_name_text != leaf_name)
      return mismatch("name_registry_raw_name_mismatch");
    if (name.display_name != leaf_name)
      return mismatch("name_registry_display_name_mismatch");
    if (name.full_path_lookup_key != expected_path_lookup_key)
      return mismatch("name_registry_path_mismatch");
    if (name.was_quoted)
      return mismatch("name_registry_quote_flag_mismatch");
    if (name.quote_style != "none")
      return mismatch("name_registry_quote_style_mismatch");
    if (name.requires_exact_match)
      return mismatch("name_registry_exact_match_mismatch");
    if (name.lifecycle_state != "active")
      return mismatch("name_registry_lifecycle_mismatch");
    ++exact_name_count;
  }
  return exact_name_count == 1
             ? true
             : mismatch("name_registry_record_count_mismatch");
}

bool ResultMatches(
    const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1&
        descriptor,
    const SblrDdlCreateSchemaJournalSnapshotV1& snapshot,
    scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1* decoded_result) {
  if (snapshot.canonical_result_bytes.size() != kResultBytes) return false;
  std::string detail;
  scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1 result;
  if (!scratchbird::engine::sblr::DecodeSblrDdlCreateSchemaResultV1(
          snapshot.canonical_result_bytes.data(),
          snapshot.canonical_result_bytes.size(), &result, &detail) ||
      scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaResultV1(result) !=
          snapshot.canonical_result_bytes ||
      result.receipt != descriptor.receipt ||
      result.schema_uuid != descriptor.schema_uuid ||
      result.schema_generation != descriptor.schema_generation ||
      result.parent_schema_uuid != descriptor.parent_schema_uuid ||
      result.parent_namespace_generation !=
          descriptor.parent_namespace_generation ||
      result.database_uuid != descriptor.database_uuid ||
      result.owning_transaction_uuid != descriptor.owning_transaction_uuid ||
      result.owning_local_transaction_id !=
          descriptor.owning_local_transaction_id ||
      result.statement_snapshot_uuid != descriptor.statement_snapshot_uuid ||
      result.catalog_row_uuid != snapshot.catalog_row_uuid ||
      result.mutation_uuid != snapshot.mutation_uuid ||
      result.catalog_generation != descriptor.catalog_generation ||
      result.security_epoch != descriptor.security_epoch ||
      result.resource_generation != descriptor.resource_generation ||
      result.normalized_path_sha256 != descriptor.normalized_path_sha256 ||
      result.descriptor_evidence_sha256 != descriptor.evidence ||
      result.availability != descriptor.availability ||
      result.publication_barrier != snapshot.publication_barrier_uuid) {
    return false;
  }
  if (decoded_result != nullptr) *decoded_result = result;
  return true;
}

bool SnapshotValid(const SblrDdlCreateSchemaJournalSnapshotV1& snapshot) {
  if (snapshot.journal_generation == 0 ||
      (snapshot.state != SblrDdlCreateSchemaJournalStateV1::begun &&
       snapshot.state != SblrDdlCreateSchemaJournalStateV1::published) ||
      !NonZero(snapshot.catalog_row_uuid) || !NonZero(snapshot.mutation_uuid) ||
      !NonZero(snapshot.publication_barrier_uuid) ||
      snapshot.catalog_row_uuid == snapshot.mutation_uuid ||
      snapshot.catalog_row_uuid == snapshot.publication_barrier_uuid ||
      snapshot.mutation_uuid == snapshot.publication_barrier_uuid) {
    return false;
  }
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor;
  if (!DecodeDescriptor(snapshot.key, &descriptor) ||
      snapshot.catalog_row_uuid == descriptor.schema_uuid ||
      snapshot.catalog_row_uuid == descriptor.recovery_uuid ||
      snapshot.mutation_uuid == descriptor.schema_uuid ||
      snapshot.mutation_uuid == descriptor.recovery_uuid ||
      snapshot.publication_barrier_uuid == descriptor.schema_uuid ||
      snapshot.publication_barrier_uuid == descriptor.recovery_uuid ||
      !ResultMatches(descriptor, snapshot, nullptr)) {
    return false;
  }
  const auto descriptor_hash =
      Hash(snapshot.key.canonical_descriptor_bytes.data(),
           snapshot.key.canonical_descriptor_bytes.size());
  const auto result_hash = Hash(snapshot.canonical_result_bytes.data(),
                                snapshot.canonical_result_bytes.size());
  return (!NonZero(snapshot.canonical_descriptor_sha256) ||
          snapshot.canonical_descriptor_sha256 == descriptor_hash) &&
         (!NonZero(snapshot.canonical_result_sha256) ||
          snapshot.canonical_result_sha256 == result_hash);
}

std::vector<std::uint8_t> Encode(
    const SblrDdlCreateSchemaJournalSnapshotV1& snapshot) {
  if (!SnapshotValid(snapshot)) return {};
  const auto descriptor_hash =
      Hash(snapshot.key.canonical_descriptor_bytes.data(),
           snapshot.key.canonical_descriptor_bytes.size());
  const auto result_hash = Hash(snapshot.canonical_result_bytes.data(),
                                snapshot.canonical_result_bytes.size());

  std::vector<std::uint8_t> bytes{'C', 'S', 'E', 'J'};
  bytes.reserve(kRecordBytes);
  PutLe(&bytes, 1, 2);
  PutLe(&bytes, kHeaderBytes, 2);
  PutLe(&bytes, kRecordBytes, 4);
  PutLe(&bytes, static_cast<std::uint32_t>(snapshot.state), 4);
  Put(&bytes, snapshot.key.database_uuid);
  Put(&bytes, snapshot.key.recovery_uuid);
  Put(&bytes, snapshot.catalog_row_uuid);
  Put(&bytes, snapshot.mutation_uuid);
  Put(&bytes, snapshot.publication_barrier_uuid);
  PutLe(&bytes, snapshot.journal_generation, 8);
  PutLe(&bytes, kDescriptorBytes, 4);
  PutLe(&bytes, kResultBytes, 4);
  Put(&bytes, descriptor_hash);
  Put(&bytes, result_hash);
  bytes.insert(bytes.end(), 32, 0);
  bytes.insert(bytes.end(), 16, 0);
  bytes.insert(bytes.end(), snapshot.key.canonical_descriptor_bytes.begin(),
               snapshot.key.canonical_descriptor_bytes.end());
  bytes.insert(bytes.end(), snapshot.canonical_result_bytes.begin(),
               snapshot.canonical_result_bytes.end());
  if (bytes.size() != kRecordBytes) return {};
  const auto evidence = RecordHash(bytes);
  if (NonZero(snapshot.record_evidence_sha256) &&
      snapshot.record_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), bytes.begin() + 176);
  return bytes;
}

bool Decode(const std::vector<std::uint8_t>& bytes,
            SblrDdlCreateSchemaJournalSnapshotV1* snapshot) {
  if (snapshot == nullptr || bytes.size() != kRecordBytes ||
      !std::equal(bytes.begin(), bytes.begin() + 4, "CSEJ") ||
      GetLe(bytes.data() + 4, 2) != 1 ||
      GetLe(bytes.data() + 6, 2) != kHeaderBytes ||
      GetLe(bytes.data() + 8, 4) != kRecordBytes ||
      GetLe(bytes.data() + 104, 4) != kDescriptorBytes ||
      GetLe(bytes.data() + 108, 4) != kResultBytes ||
      !Zero(bytes.data() + 208, bytes.data() + kHeaderBytes)) {
    return false;
  }

  SblrDdlCreateSchemaJournalSnapshotV1 value;
  const auto state = GetLe(bytes.data() + 12, 4);
  if (state < 1 || state > 2) return false;
  value.state = static_cast<SblrDdlCreateSchemaJournalStateV1>(state);
  Get(bytes.data() + 16, &value.key.database_uuid);
  Get(bytes.data() + 32, &value.key.recovery_uuid);
  Get(bytes.data() + 48, &value.catalog_row_uuid);
  Get(bytes.data() + 64, &value.mutation_uuid);
  Get(bytes.data() + 80, &value.publication_barrier_uuid);
  value.journal_generation = GetLe(bytes.data() + 96, 8);
  Get(bytes.data() + 112, &value.canonical_descriptor_sha256);
  Get(bytes.data() + 144, &value.canonical_result_sha256);
  Get(bytes.data() + 176, &value.record_evidence_sha256);
  value.key.canonical_descriptor_bytes.assign(
      bytes.begin() + kHeaderBytes,
      bytes.begin() + kHeaderBytes + kDescriptorBytes);
  value.canonical_result_bytes.assign(
      bytes.begin() + kHeaderBytes + kDescriptorBytes, bytes.end());

  auto evidence_material = bytes;
  std::fill(evidence_material.begin() + 176,
            evidence_material.begin() + 208, 0);
  if (!SnapshotValid(value) ||
      value.record_evidence_sha256 != RecordHash(evidence_material) ||
      Encode(value) != bytes) {
    return false;
  }
  *snapshot = std::move(value);
  return true;
}

std::string Path(const EngineRequestContext& context,
                 const SblrDdlCreateSchemaJournalKeyV1& key) {
  return context.database_path +
         ".sb.sblr_ddl_create_schema_execution_journal.v1." +
         UuidText(key.recovery_uuid);
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
  if (end < 0 || static_cast<std::uint64_t>(end) != kRecordBytes) {
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
      metadata.st_size != static_cast<off_t>(kRecordBytes)) {
    ::close(fd);
    return ReadStatus::invalid;
  }
  bytes->resize(kRecordBytes);
  std::size_t offset = 0;
  while (offset != bytes->size()) {
    const auto count =
        ::read(fd, bytes->data() + offset, bytes->size() - offset);
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

enum class CreateStatus { created, exists, failed };

CreateStatus CreateFile(const std::string& path,
                        const std::vector<std::uint8_t>& bytes) {
#if defined(_WIN32)
  if (std::filesystem::exists(path)) return CreateStatus::exists;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return CreateStatus::failed;
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  output.flush();
  return output ? CreateStatus::created : CreateStatus::failed;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                          O_CLOEXEC | O_NOFOLLOW,
                        0600);
  if (fd < 0) return errno == EEXIST ? CreateStatus::exists
                                     : CreateStatus::failed;
  std::size_t offset = 0;
  bool ok = true;
  while (offset != bytes.size()) {
    const auto count =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  ok = ok && ::fsync(fd) == 0;
  if (::close(fd) != 0) ok = false;
  if (!ok) {
    (void)::unlink(path.c_str());
    return CreateStatus::failed;
  }
  return SyncParent(path) ? CreateStatus::created : CreateStatus::failed;
#endif
}

std::uint64_t ProcessOrdinal() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

bool ReplaceFile(const std::string& path,
                 const std::vector<std::uint8_t>& bytes) {
  const auto ordinal = g_temp_ordinal.fetch_add(1, std::memory_order_relaxed);
  const auto temp = path + ".tmp." + std::to_string(ProcessOrdinal()) + "." +
                    std::to_string(ordinal);
  if (CreateFile(temp, bytes) != CreateStatus::created) return false;
  std::error_code error;
  std::filesystem::rename(temp, path, error);
  if (error) {
    std::filesystem::remove(temp, error);
    return false;
  }
  return SyncParent(path);
}

class ScopedFileLock {
 public:
  ScopedFileLock() = default;
  ScopedFileLock(const ScopedFileLock&) = delete;
  ScopedFileLock& operator=(const ScopedFileLock&) = delete;
  ~ScopedFileLock() { Release(); }

  bool Acquire(const std::string& path) {
#if defined(_WIN32)
    (void)path;
    acquired_ = true;
    return true;
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                 0600);
    if (fd_ < 0) return false;
    struct stat metadata {};
    if (::fstat(fd_, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        ::flock(fd_, LOCK_EX) != 0) {
      Release();
      return false;
    }
    acquired_ = true;
    return true;
#endif
  }

 private:
  void Release() {
#if defined(_WIN32)
    acquired_ = false;
#else
    if (fd_ >= 0) {
      if (acquired_) (void)::flock(fd_, LOCK_UN);
      (void)::close(fd_);
    }
    fd_ = -1;
    acquired_ = false;
#endif
  }

  bool acquired_ = false;
#if !defined(_WIN32)
  int fd_ = -1;
#endif
};

SblrDdlCreateSchemaJournalResultV1 Refused(std::string code,
                                           std::string key,
                                           std::string detail = {}) {
  SblrDdlCreateSchemaJournalResultV1 result;
  result.diagnostic =
      Diagnostic(std::move(code), std::move(key), std::move(detail));
  return result;
}

SblrDdlCreateSchemaJournalResultV1 Loaded(
    SblrDdlCreateSchemaJournalSnapshotV1 snapshot) {
  SblrDdlCreateSchemaJournalResultV1 result;
  result.ok = true;
  result.found = true;
  result.diagnostic = OkDiagnostic();
  result.snapshot = std::move(snapshot);
  return result;
}

SblrDdlCreateSchemaJournalResultV1 LoadExact(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key) {
  std::vector<std::uint8_t> bytes;
  const auto status = ReadFile(Path(context, key), &bytes);
  if (status == ReadStatus::absent) {
    SblrDdlCreateSchemaJournalResultV1 result;
    result.ok = true;
    result.found = false;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  SblrDdlCreateSchemaJournalSnapshotV1 snapshot;
  if (status != ReadStatus::ok || !Decode(bytes, &snapshot)) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.execution_journal_corrupt",
        "durable CREATE SCHEMA recovery record is torn or noncanonical");
  }
  if (snapshot.key.database_uuid != key.database_uuid ||
      snapshot.key.recovery_uuid != key.recovery_uuid ||
      snapshot.key.canonical_descriptor_bytes !=
          key.canonical_descriptor_bytes) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.execution_journal_identity_conflict",
        "recovery operation is already bound to different CREATE SCHEMA authority");
  }
  return Loaded(std::move(snapshot));
}

bool NextDistinctUuid(
    std::string kind,
    std::unordered_set<std::string>* identities,
    SblrDdlCreateSchemaJournalUuidV1* value) {
  for (std::size_t attempt = 0; attempt != 64; ++attempt) {
    const auto candidate = GenerateCrudEngineUuid(kind);
    SblrDdlCreateSchemaJournalUuidV1 parsed{};
    if (!ParseUuidText(candidate, &parsed) ||
        !identities->insert(candidate).second) {
      continue;
    }
    *value = parsed;
    return true;
  }
  return false;
}

bool MakeBegunSnapshot(
    const SblrDdlCreateSchemaJournalKeyV1& key,
    SblrDdlCreateSchemaJournalSnapshotV1* snapshot) {
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor;
  if (snapshot == nullptr || !DecodeDescriptor(key, &descriptor)) return false;
  std::unordered_set<std::string> identities{
      UuidText(descriptor.receipt),
      UuidText(descriptor.schema_uuid),
      UuidText(descriptor.database_uuid),
      UuidText(descriptor.owning_transaction_uuid),
      UuidText(descriptor.statement_snapshot_uuid),
      UuidText(descriptor.catalog_epoch_uuid),
      UuidText(descriptor.security_context_uuid),
      UuidText(descriptor.policy_snapshot_uuid),
      UuidText(descriptor.resource_grant_uuid),
      UuidText(descriptor.owner_principal_uuid),
      UuidText(descriptor.binding_uuid),
      UuidText(descriptor.recovery_uuid),
  };
  if (NonZero(descriptor.parent_schema_uuid)) {
    identities.insert(UuidText(descriptor.parent_schema_uuid));
  }

  SblrDdlCreateSchemaJournalSnapshotV1 value;
  value.key = key;
  value.state = SblrDdlCreateSchemaJournalStateV1::begun;
  value.journal_generation = 1;
  if (!NextDistinctUuid("row", &identities, &value.catalog_row_uuid) ||
      !NextDistinctUuid("object", &identities, &value.mutation_uuid) ||
      !NextDistinctUuid("object", &identities,
                        &value.publication_barrier_uuid)) {
    return false;
  }

  scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1 result;
  result.receipt = descriptor.receipt;
  result.schema_uuid = descriptor.schema_uuid;
  result.schema_generation = descriptor.schema_generation;
  result.parent_schema_uuid = descriptor.parent_schema_uuid;
  result.parent_namespace_generation = descriptor.parent_namespace_generation;
  result.database_uuid = descriptor.database_uuid;
  result.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  result.owning_local_transaction_id = descriptor.owning_local_transaction_id;
  result.statement_snapshot_uuid = descriptor.statement_snapshot_uuid;
  result.catalog_row_uuid = value.catalog_row_uuid;
  result.mutation_uuid = value.mutation_uuid;
  result.catalog_generation = descriptor.catalog_generation;
  result.security_epoch = descriptor.security_epoch;
  result.resource_generation = descriptor.resource_generation;
  result.normalized_path_sha256 = descriptor.normalized_path_sha256;
  result.descriptor_evidence_sha256 = descriptor.evidence;
  result.availability = descriptor.availability;
  result.publication_barrier = value.publication_barrier_uuid;
  value.canonical_result_bytes =
      scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaResultV1(result);
  value.canonical_descriptor_sha256 = Hash(
      value.key.canonical_descriptor_bytes.data(),
      value.key.canonical_descriptor_bytes.size());
  value.canonical_result_sha256 =
      Hash(value.canonical_result_bytes.data(),
           value.canonical_result_bytes.size());
  if (!SnapshotValid(value)) return false;
  *snapshot = std::move(value);
  return true;
}

SblrDdlCreateSchemaJournalResultV1 EnsureLocked(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key) {
  auto existing = LoadExact(context, key);
  if (!existing.ok || existing.found) return existing;

  SblrDdlCreateSchemaJournalSnapshotV1 snapshot;
  if (!MakeBegunSnapshot(key, &snapshot)) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.execution_journal_identity_unavailable");
  }
  const auto bytes = Encode(snapshot);
  if (bytes.empty()) {
    return Refused("SBLR.OPERAND_INVALID",
                   "sblr.ddl_create_schema.execution_journal_key_invalid");
  }
  const auto created = CreateFile(Path(context, key), bytes);
  if (created == CreateStatus::exists) return LoadExact(context, key);
  if (created != CreateStatus::created || !Decode(bytes, &snapshot)) {
    return Refused(
        "DDL.CREATE_SCHEMA_FAILED",
        "sblr.ddl_create_schema.execution_journal_intent_publish_failed",
        "durable CREATE SCHEMA intent publication failed");
  }
  return Loaded(std::move(snapshot));
}

SblrDdlCreateSchemaJournalResultV1 PublishLocked(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key) {
  auto existing = LoadExact(context, key);
  if (!existing.ok) return existing;
  if (!existing.found) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.execution_journal_identity_missing");
  }
  if (existing.snapshot.state ==
      SblrDdlCreateSchemaJournalStateV1::published) {
    existing.replayed_published_result = true;
    return existing;
  }
  if (existing.snapshot.journal_generation ==
      std::numeric_limits<std::uint64_t>::max()) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.execution_journal_generation_exhausted");
  }
  auto published = existing.snapshot;
  published.state = SblrDdlCreateSchemaJournalStateV1::published;
  ++published.journal_generation;
  published.record_evidence_sha256 = {};
  const auto bytes = Encode(published);
  if (bytes.empty() || !ReplaceFile(Path(context, key), bytes) ||
      !Decode(bytes, &published)) {
    return Refused(
        "DDL.CREATE_SCHEMA_FAILED",
        "sblr.ddl_create_schema.execution_journal_result_publish_failed",
        "durable CREATE SCHEMA CSRS publication failed");
  }
  return Loaded(std::move(published));
}

bool ValidateInputs(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key,
    scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1* descriptor,
    SblrDdlCreateSchemaJournalResultV1* refusal) {
  if (!DecodeDescriptor(key, descriptor)) {
    *refusal = Refused(
        "SBLR.OPERAND_INVALID",
        "sblr.ddl_create_schema.execution_journal_descriptor_invalid");
    return false;
  }
  if (!HasAuthority(context, *descriptor)) {
    *refusal = Refused(
        "SECURITY.ACCESS_DENIED",
        "sblr.ddl_create_schema.execution_journal_hidden");
    return false;
  }
  return true;
}

bool AcquireRecordLock(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key,
    ScopedFileLock* lock,
    SblrDdlCreateSchemaJournalResultV1* refusal) {
  if (lock->Acquire(Path(context, key) + ".lock")) return true;
  *refusal = Refused(
      "DDL.CREATE_SCHEMA_FAILED",
      "sblr.ddl_create_schema.execution_journal_lock_failed",
      "durable CREATE SCHEMA recovery identity could not be locked");
  return false;
}

}  // namespace

SblrDdlCreateSchemaJournalResultV1
LookupSblrDdlCreateSchemaExecutionJournalV1(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key) {
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor;
  SblrDdlCreateSchemaJournalResultV1 refusal;
  if (!ValidateInputs(context, key, &descriptor, &refusal)) return refusal;
  std::lock_guard guard(g_journal_mutex);
  ScopedFileLock file_lock;
  if (!AcquireRecordLock(context, key, &file_lock, &refusal)) return refusal;
  return LoadExact(context, key);
}

SblrDdlCreateSchemaJournalResultV1
EnsureSblrDdlCreateSchemaExecutionJournalV1(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key) {
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor;
  SblrDdlCreateSchemaJournalResultV1 refusal;
  if (!ValidateInputs(context, key, &descriptor, &refusal)) return refusal;
  std::lock_guard guard(g_journal_mutex);
  ScopedFileLock file_lock;
  if (!AcquireRecordLock(context, key, &file_lock, &refusal)) return refusal;
  return EnsureLocked(context, key);
}

SblrDdlCreateSchemaJournalResultV1
ExecuteSblrDdlCreateSchemaExecutionJournalV1(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key,
    const SblrDdlCreateSchemaMutationV1& mutation) {
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor;
  SblrDdlCreateSchemaJournalResultV1 refusal;
  if (!ValidateInputs(context, key, &descriptor, &refusal)) return refusal;
  if (!mutation) {
    return Refused(
        "SBLR.OPERAND_INVALID",
        "sblr.ddl_create_schema.execution_journal_callback_missing");
  }

  std::lock_guard guard(g_journal_mutex);
  ScopedFileLock file_lock;
  if (!AcquireRecordLock(context, key, &file_lock, &refusal)) return refusal;

  auto ensured = EnsureLocked(context, key);
  if (!ensured.ok) return ensured;
  if (ensured.snapshot.state ==
      SblrDdlCreateSchemaJournalStateV1::published) {
    ensured.replayed_published_result = true;
    return ensured;
  }

  scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1 planned_result;
  if (!ResultMatches(descriptor, ensured.snapshot, &planned_result)) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.execution_journal_planned_result_invalid");
  }

  EngineApiDiagnostic mutation_diagnostic;
  try {
    mutation_diagnostic = mutation(planned_result);
  } catch (const std::bad_alloc&) {
    mutation_diagnostic = Diagnostic(
        "ENGINE.RESOURCE.EXHAUSTED",
        "sblr.ddl_create_schema.execution_journal_mutation_allocation_failed");
  } catch (...) {
    mutation_diagnostic = Diagnostic(
        "DDL.CREATE_SCHEMA_FAILED",
        "sblr.ddl_create_schema.execution_journal_mutation_exception");
  }
  if (mutation_diagnostic.error) {
    SblrDdlCreateSchemaJournalResultV1 result;
    result.found = true;
    result.mutation_invoked = true;
    result.diagnostic = std::move(mutation_diagnostic);
    result.snapshot = std::move(ensured.snapshot);
    return result;
  }

  auto published = PublishLocked(context, key);
  published.mutation_invoked = true;
  return published;
}

SblrDdlCreateSchemaJournalResultV1
RecoverSblrDdlCreateSchemaExecutionJournalV1(
    const EngineRequestContext& authenticated_context,
    const scratchbird::engine::sblr::
        SblrDdlCreateSchemaRecoveryRequestV1& request) {
  const auto exact_request = scratchbird::engine::sblr::
      EncodeSblrDdlCreateSchemaRecoveryRequestV1(request);
  if (exact_request.empty()) {
    return Refused("SBLR.OPERAND_INVALID",
                   "sblr.ddl_create_schema.recovery_request_invalid",
                   "exact canonical CSRQ is required");
  }
  const auto& descriptor = request.operand_descriptor;
  if (!RecoverySessionAuthenticated(authenticated_context, descriptor)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.ddl_create_schema.recovery_hidden");
  }

  std::string canonical_path;
  std::string leaf_name;
  SblrDdlCreateSchemaJournalHashV1 normalized_path_sha256{};
  if (!NormalizeRecoveryPath(request.bind_request, &canonical_path, &leaf_name,
                             &normalized_path_sha256) ||
      normalized_path_sha256 != descriptor.normalized_path_sha256) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.ddl_create_schema.recovery_syntax_mismatch",
                   "CSRQ syntax does not reproduce the bound schema path");
  }

  EngineAuthorizeRequest authorize;
  authorize.context = authenticated_context;
  authorize.target_object.uuid = authenticated_context.database_uuid;
  authorize.target_object.object_kind = "database";
  authorize.required_right = "CATALOG_MUTATE";
  const auto authorized = EngineAuthorize(authorize);
  if (!authorized.ok || !authorized.authorized) {
    const auto detail = authorized.diagnostics.empty()
                            ? std::string{}
                            : authorized.diagnostics.front().detail;
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.ddl_create_schema.recovery_authorization_denied",
                   detail);
  }
  if (authenticated_context.query_cancellation_requested &&
      authenticated_context.query_cancellation_requested()) {
    return Refused("PROCESS.CANCELLED",
                   "sblr.ddl_create_schema.recovery_cancelled_before_inventory");
  }

  const auto inventory =
      scratchbird::storage::database::
          AcquireStrongLocalTransactionInventorySnapshot(
              authenticated_context.database_path);
  if (!inventory.ok()) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.recovery_inventory_unavailable",
        inventory.diagnostic.diagnostic_code.empty()
            ? inventory.diagnostic.remediation_hint
            : inventory.diagnostic.diagnostic_code);
  }
  const auto original_transaction =
      scratchbird::transaction::mga::LookupLocalTransaction(
          inventory.snapshot->inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(
              descriptor.owning_local_transaction_id));
  if (!original_transaction.ok() ||
      !original_transaction.entry.identity.transaction_uuid.valid() ||
      original_transaction.entry.identity.transaction_uuid.value.bytes !=
          descriptor.owning_transaction_uuid) {
    return Refused("MGA.TRANSACTION_INVALID",
                   "sblr.ddl_create_schema.recovery_transaction_invalid",
                   "owning transaction identity is absent or changed");
  }

  using TransactionState =
      scratchbird::transaction::mga::TransactionState;
  const auto state = original_transaction.entry.state;
  const bool active = state == TransactionState::active;
  const bool committed = state == TransactionState::committed;
  if (!active && !committed) {
    const bool unresolved =
        state == TransactionState::created ||
        state == TransactionState::preparing ||
        state == TransactionState::prepared ||
        state == TransactionState::committing ||
        state == TransactionState::limbo ||
        state == TransactionState::recovering;
    return Refused(
        unresolved ? "MGA.AUTHORITY_MISMATCH" : "MGA.TRANSACTION_INVALID",
        unresolved
            ? "sblr.ddl_create_schema.recovery_transaction_unresolved"
            : "sblr.ddl_create_schema.recovery_transaction_terminal",
        scratchbird::transaction::mga::TransactionStateName(state));
  }

  SblrDdlCreateSchemaJournalKeyV1 key;
  key.database_uuid = descriptor.database_uuid;
  key.recovery_uuid = descriptor.recovery_uuid;
  key.canonical_descriptor_bytes =
      scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaDescriptorV1(
          descriptor, true);
  if (key.canonical_descriptor_bytes.empty()) {
    return Refused("SBLR.OPERAND_INVALID",
                   "sblr.ddl_create_schema.recovery_descriptor_invalid");
  }

  EngineRequestContext operation_context = authenticated_context;
  operation_context.statement_transaction_inventory_snapshot =
      inventory.snapshot;
  operation_context.statement_receipt_uuid.canonical =
      UuidText(descriptor.receipt);
  operation_context.transaction_uuid.canonical =
      UuidText(descriptor.owning_transaction_uuid);
  operation_context.local_transaction_id =
      descriptor.owning_local_transaction_id;
  operation_context.snapshot_visible_through_local_transaction_id =
      original_transaction.entry.begin_visible_through_local_transaction_id;
  operation_context.statement_snapshot_uuid.canonical =
      UuidText(descriptor.statement_snapshot_uuid);
  operation_context.statement_metadata_snapshot_uuid =
      operation_context.statement_snapshot_uuid;
  operation_context.statement_metadata_snapshot_engine_owned = true;
  operation_context.catalog_epoch_uuid.canonical =
      UuidText(descriptor.catalog_epoch_uuid);
  operation_context.catalog_generation_id = descriptor.catalog_generation;
  operation_context.security_epoch = descriptor.security_epoch;
  operation_context.transaction_policy_snapshot_uuid.canonical =
      UuidText(descriptor.policy_snapshot_uuid);
  operation_context.transaction_policy_snapshot_generation =
      descriptor.policy_generation;
  operation_context.resource_admission_uuid.canonical =
      UuidText(descriptor.resource_grant_uuid);
  operation_context.resource_epoch = descriptor.resource_generation;
  operation_context.trace_tags.push_back(std::string(kJournalTraceTag));
  operation_context.trace_tags.push_back(std::string(kRecoveryTraceTag));

  if (active) {
    if (authenticated_context.catalog_generation_id !=
            descriptor.catalog_generation ||
        authenticated_context.security_epoch != descriptor.security_epoch ||
        authenticated_context.resource_epoch != descriptor.resource_generation) {
      return Refused("MGA.AUTHORITY_MISMATCH",
                     "sblr.ddl_create_schema.recovery_epoch_changed",
                     "catalog security or resource authority changed");
    }
    const auto availability = LoadCurrentSblrExecutorAvailabilitySnapshot(
        authenticated_context, CreateSchemaExecutorIdentity());
    if (!availability.ok || !availability.snapshot.installed ||
        availability.snapshot.generation != descriptor.availability) {
      return Refused(
          "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "sblr.ddl_create_schema.recovery_executor_unavailable",
          "current CREATE SCHEMA executor evidence changed");
    }
  }
  if (authenticated_context.query_cancellation_requested &&
      authenticated_context.query_cancellation_requested()) {
    return Refused("PROCESS.CANCELLED",
                   "sblr.ddl_create_schema.recovery_cancelled_before_lock");
  }

  std::lock_guard guard(g_journal_mutex);
  ScopedFileLock file_lock;
  SblrDdlCreateSchemaJournalResultV1 refusal;
  if (!AcquireRecordLock(authenticated_context, key, &file_lock, &refusal)) {
    return refusal;
  }
  auto journaled = LoadExact(authenticated_context, key);
  if (!journaled.ok) return journaled;

  const auto observer_transaction_id =
      committed && authenticated_context.local_transaction_id != 0
          ? authenticated_context.local_transaction_id
          : descriptor.owning_local_transaction_id;
  EngineRequestContext observer_context =
      committed ? authenticated_context : operation_context;
  observer_context.statement_transaction_inventory_snapshot = inventory.snapshot;

  if (journaled.found &&
      journaled.snapshot.state ==
          SblrDdlCreateSchemaJournalStateV1::published) {
    std::string postcondition_detail;
    if (!ExactRecoveryPostcondition(observer_context, descriptor,
                                    journaled.snapshot, canonical_path,
                                    leaf_name, observer_transaction_id,
                                    &postcondition_detail)) {
      return Refused(
          "MGA.AUTHORITY_MISMATCH",
          "sblr.ddl_create_schema.recovery_postcondition_mismatch",
          "published CSRS does not name the exact durable catalog state: " +
              postcondition_detail);
    }
    journaled.authenticated_recovery = true;
    journaled.postcondition_verified = true;
    journaled.replayed_published_result = true;
    return journaled;
  }
  if (committed) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.recovery_committed_result_missing",
        "committed operation has no exact published recovery result");
  }

  if (!journaled.found) {
    journaled = EnsureLocked(operation_context, key);
    if (!journaled.ok) return journaled;
  }
  scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1 planned_result;
  if (!ResultMatches(descriptor, journaled.snapshot, &planned_result)) {
    return Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.recovery_planned_result_invalid");
  }
  if (authenticated_context.query_cancellation_requested &&
      authenticated_context.query_cancellation_requested()) {
    return Refused("PROCESS.CANCELLED",
                   "sblr.ddl_create_schema.recovery_cancelled_before_mutation");
  }

  EngineCreateSchemaRequest create;
  create.context = operation_context;
  create.operation_id = "ddl.create_schema";
  create.target_database.uuid = operation_context.database_uuid;
  create.target_database.object_kind = "database";
  create.target_schema.uuid.canonical =
      NonZero(descriptor.parent_schema_uuid)
          ? UuidText(descriptor.parent_schema_uuid)
          : std::string{};
  create.target_schema.object_kind = "schema";
  create.target_object.uuid.canonical = UuidText(descriptor.schema_uuid);
  create.target_object.object_kind = "schema";
  create.localized_names.push_back(
      {"en", "primary", canonical_path, leaf_name, true});
  create.recovery_operation_uuid.canonical = UuidText(descriptor.recovery_uuid);
  create.requested_catalog_row_uuid.canonical =
      UuidText(planned_result.catalog_row_uuid);
  create.mutation_uuid.canonical = UuidText(planned_result.mutation_uuid);
  create.statement_publication_barrier_uuid.canonical =
      UuidText(planned_result.publication_barrier);
  create.option_envelopes.push_back(
      "catalog_ddl_mutation_audit:" +
      create.recovery_operation_uuid.canonical);
  const auto created = EngineCreateSchema(create);
  if (!created.ok || created.primary_object.object_kind != "schema" ||
      created.primary_object.uuid.canonical !=
          create.target_object.uuid.canonical ||
      created.catalog_row_uuid.canonical !=
          create.requested_catalog_row_uuid.canonical) {
    auto result = Refused(
        "DDL.CREATE_SCHEMA_FAILED",
        "sblr.ddl_create_schema.recovery_catalog_mutation_failed",
        created.diagnostics.empty() ? std::string{}
                                    : created.diagnostics.front().detail);
    result.found = true;
    result.mutation_invoked = true;
    result.authenticated_recovery = true;
    result.snapshot = std::move(journaled.snapshot);
    return result;
  }
  std::string postcondition_detail;
  if (!ExactRecoveryPostcondition(operation_context, descriptor,
                                  journaled.snapshot, canonical_path,
                                  leaf_name,
                                  descriptor.owning_local_transaction_id,
                                  &postcondition_detail)) {
    auto result = Refused(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.recovery_postcondition_mismatch",
        "catalog mutation did not publish the exact journal postcondition: " +
            postcondition_detail);
    result.found = true;
    result.mutation_invoked = true;
    result.authenticated_recovery = true;
    result.snapshot = std::move(journaled.snapshot);
    return result;
  }
  auto published = PublishLocked(operation_context, key);
  published.mutation_invoked = true;
  published.authenticated_recovery = published.ok;
  published.postcondition_verified = published.ok;
  return published;
}

}  // namespace scratchbird::engine::internal_api
