#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace hash = scratchbird::core::hash;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr const char* kLegacy = "67000000-696e-7436-b400-000000000000";
constexpr const char* kCanonicalBigintDescriptor =
    "019d0000-0000-7000-8000-00000000d711";
constexpr const char* kCanonical = "019d0000-0000-7000-8000-00000000d712";
constexpr const char* kCanonicalBoolean =
    "01000000-626f-7f6c-a561-6e0000000000";
constexpr const char* kLegacyInt32 = "66000000-696e-7433-b200-000000000000";
constexpr const char* kCanonicalInt32Descriptor =
    "019d0000-0000-7000-8000-00000000d716";
constexpr const char* kCanonicalInt32Type =
    "019d0000-0000-7000-8000-00000000d717";
constexpr const char* kCanonicalDecimalDescriptor =
    "a0000000-6465-7369-ad61-6c0000000000";
constexpr const char* kCanonicalDecimalType =
    "019d0000-0000-7000-8000-00000000d713";
constexpr const char* kCanonicalInt128Descriptor =
    "019d0000-0000-7000-8000-00000000d714";
constexpr const char* kCanonicalInt128Type =
    "019d0000-0000-7000-8000-00000000d715";
constexpr const char* kLegacyText =
    "2c010000-6368-7172-a163-746572000000";
constexpr const char* kCanonicalTextDescriptor =
    "019d0000-0000-7000-8000-00000000d718";
constexpr const char* kCanonicalTextType =
    "019d0000-0000-7000-8000-00000000d719";
constexpr const char* kCanonicalTextCodec =
    "019d0000-0000-7000-8000-00000000d71a";
constexpr const char* kDatatypeCatalogSnapshot =
    "019d0000-0000-7000-8000-00000000d701";
constexpr std::size_t kTextMigrationHeaderFields = 17;
constexpr std::size_t kTextMigrationFieldsPerRow = 25;
constexpr std::size_t kTextMigrationSingleRowFields =
    kTextMigrationHeaderFields + kTextMigrationFieldsPerRow;
constexpr std::string_view kContextualTextBlobKey =
    "column.0.contextual_text_descriptor_sidecar_v2";
constexpr std::string_view kContextualTextHashKey =
    "column.0.contextual_text_descriptor_sidecar_v2.sha256";
constexpr std::string_view kContextualTextSetSealKey =
    "contextual_text_descriptor_sidecar_set_v2.seal_sha256";

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}
void Require(bool value, const char* message) { if (!value) Fail(message); }

enum class FixtureMode {
  kNumericIdentityMigration,
  kTextIdentityMigration,
  kTextIdentityDdl,
};

FixtureMode ParseFixtureMode(const int argc, char* argv[]) {
  Require(argc == 2, "exact identity migration fixture mode required");
  const std::string_view mode(argv[1]);
  if (mode == "numeric-identity-migration") {
    return FixtureMode::kNumericIdentityMigration;
  }
  if (mode == "text-identity-migration") {
    return FixtureMode::kTextIdentityMigration;
  }
  if (mode == "text-identity-ddl") {
    return FixtureMode::kTextIdentityDdl;
  }
  Fail("unknown identity migration fixture mode");
}

bool ExactInvalidRequestDiagnostic(
    const api::EngineApiDiagnostic& diagnostic,
    const std::string_view detail) {
  return diagnostic.error &&
         diagnostic.code == "SB_ENGINE_API_INVALID_REQUEST" &&
         diagnostic.message_key == "engine.api.invalid_request" &&
         diagnostic.detail == detail;
}

bool ExactTextMigrationBatchInvalidDiagnostic(
    const api::EngineApiDiagnostic& diagnostic) {
  return ExactInvalidRequestDiagnostic(
      diagnostic,
      "mga.relation_metadata:text_migration_batch_invalid");
}

bool ExactSealedRelationDescriptorSnapshotConflictDiagnostic(
    const api::EngineApiDiagnostic& diagnostic) {
  return ExactInvalidRequestDiagnostic(
      diagnostic,
      "mga.relation_descriptor.load:"
      "sealed_relation_descriptor_snapshot_conflict");
}

void ConfigureMemoryFixture() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_sblr_alignment_ia01_bigint_identity_migration";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      std::move(policy),
      "sbsql_sblr_alignment_ia01_bigint_identity_migration");
  if (!configured.ok()) {
    std::cerr << configured.diagnostic.diagnostic_code << ' '
              << configured.diagnostic.message_key << '\n';
  }
  Require(configured.ok(), "default memory fixture configuration failed");
  Require(configured.fixture_mode,
          "default memory fixture mode was not active");
}

std::string Id(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1786830000000ull + salt);
  Require(generated.ok(), "uuid generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string table_uuid{Id(UuidKind::object, 20)};
  std::string column_uuid{Id(UuidKind::object, 21)};
  std::string int32_table_uuid{Id(UuidKind::object, 22)};
  std::string int32_column_a_uuid{Id(UuidKind::object, 23)};
  std::string int32_column_b_uuid{Id(UuidKind::object, 24)};
  std::string int32_conflict_table_uuid{Id(UuidKind::object, 25)};
  std::string int32_conflict_column_uuid{Id(UuidKind::object, 26)};
  std::string text_table_uuid{Id(UuidKind::object, 27)};
  std::string text_column_uuid;
  std::string text_conflict_table_uuid{Id(UuidKind::object, 28)};
  std::string text_conflict_column_uuid;
  std::string fresh_schema_uuid{Id(UuidKind::object, 29)};
  std::string fresh_text_table_uuid{Id(UuidKind::object, 34)};
  std::string rejected_text_table_uuid{Id(UuidKind::object, 35)};
  std::string fresh_non_text_table_uuid{Id(UuidKind::object, 39)};
  std::string text_semantic_table_uuid{Id(UuidKind::object, 40)};
  std::string text_semantic_column_uuid;
  std::string text_resource_table_uuid{Id(UuidKind::object, 44)};
  std::string baseline_schema_uuid{Id(UuidKind::object, 45)};
  std::string text_resource_column_uuid;
  std::string rejected_text_length_table_uuid{Id(UuidKind::object, 41)};
  std::string rejected_text_resource_table_uuid{Id(UuidKind::object, 42)};
  std::string wrong_receipt_text_table_uuid{Id(UuidKind::object, 43)};
  std::string fresh_resource_text_table_uuid{Id(UuidKind::object, 46)};
  std::string text_stale_column_table_uuid{Id(UuidKind::object, 49)};
  std::string text_stale_column_uuid;
  std::string text_resource_conflict_table_uuid{Id(UuidKind::object, 50)};
  std::string text_resource_conflict_column_uuid;
  std::string text_stale_charset_table_uuid{Id(UuidKind::object, 51)};
  std::string text_stale_charset_column_uuid;
  std::string text_stale_collation_table_uuid{Id(UuidKind::object, 52)};
  std::string text_stale_collation_column_uuid;
  std::string wrong_receipt_non_text_table_uuid{Id(UuidKind::object, 59)};
  std::string contradictory_non_text_table_uuid{Id(UuidKind::object, 60)};
  std::uint64_t resource_epoch = 0;
  std::string charset_uuid;
  std::uint64_t charset_generation = 0;
  std::string collation_uuid;
  std::uint64_t collation_generation = 0;
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    for (const char* suffix : {".sb.transaction_inventory", ".sb.mga_relation_metadata",
                               ".sb.mga_event_sequences", ".sb.mga_savepoints",
                               ".sb.mga_relation_descriptors",
                               ".sb.owner.lock", ".dirty.manifest", ".recovery.evidence"}) {
      std::filesystem::remove(path.string() + suffix, ignored);
    }
  }
};

std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto end = line.find('\t', start);
    fields.push_back(line.substr(
        start, end == std::string::npos ? line.size() - start : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return fields;
}

std::string JoinTabs(const std::vector<std::string>& fields) {
  std::string line;
  for (const auto& field : fields) {
    if (!line.empty()) line.push_back('\t');
    line += field;
  }
  return line;
}

std::string HexDecode(const std::string_view encoded) {
  if ((encoded.size() & 1u) != 0u) return {};
  const auto nibble = [](const char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t i = 0; i < encoded.size(); i += 2) {
    const int high = nibble(encoded[i]);
    const int low = nibble(encoded[i + 1]);
    if (high < 0 || low < 0) return {};
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

bool IsLowerHex(const std::string_view encoded) {
  for (const char value : encoded) {
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string HexEncodeLower(const std::string_view raw) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(raw.size() * 2);
  for (const unsigned char value : raw) {
    encoded.push_back(kHex[value >> 4]);
    encoded.push_back(kHex[value & 0x0f]);
  }
  return encoded;
}

std::vector<std::pair<std::string, std::string>>
DecodeCanonicalPairVector(const std::string_view encoded) {
  Require(!encoded.empty(), "sealed descriptor vector is empty");
  std::vector<std::pair<std::string, std::string>> pairs;
  std::size_t start = 0;
  while (start < encoded.size()) {
    const auto end = encoded.find('|', start);
    const auto part = encoded.substr(
        start, end == std::string_view::npos ? encoded.size() - start
                                             : end - start);
    const auto equals = part.find('=');
    Require(!part.empty() && equals != std::string_view::npos && equals != 0 &&
                part.find('=', equals + 1) == std::string_view::npos,
            "sealed descriptor pair framing is noncanonical");
    const auto key_hex = part.substr(0, equals);
    const auto value_hex = part.substr(equals + 1);
    Require((key_hex.size() & 1u) == 0u &&
                (value_hex.size() & 1u) == 0u && IsLowerHex(key_hex) &&
                IsLowerHex(value_hex),
            "sealed descriptor pair hex is noncanonical");
    pairs.emplace_back(HexDecode(key_hex), HexDecode(value_hex));
    if (end == std::string_view::npos) break;
    start = end + 1;
    Require(start < encoded.size(),
            "sealed descriptor vector has a trailing separator");
  }
  return pairs;
}

std::string EncodeCanonicalPairVector(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::string encoded;
  for (const auto& [key, value] : pairs) {
    if (!encoded.empty()) encoded.push_back('|');
    encoded += HexEncodeLower(key);
    encoded.push_back('=');
    encoded += HexEncodeLower(value);
  }
  return encoded;
}

std::string DecodeCanonicalHex(const std::string_view encoded) {
  Require((encoded.size() & 1u) == 0u && IsLowerHex(encoded),
          "hex field is noncanonical");
  return HexDecode(encoded);
}

std::uint64_t CanonicalU64(const std::string_view encoded) {
  Require(!encoded.empty() &&
              (encoded.size() == 1 || encoded.front() != '0'),
          "noncanonical unsigned decimal field");
  std::uint64_t value = 0;
  for (const char digit : encoded) {
    Require(digit >= '0' && digit <= '9',
            "noncanonical unsigned decimal digit");
    const auto next = static_cast<std::uint64_t>(digit - '0');
    Require(value <= (std::numeric_limits<std::uint64_t>::max() - next) / 10,
            "unsigned decimal field overflow");
    value = value * 10 + next;
  }
  Require(std::to_string(value) == encoded,
          "unsigned decimal field did not round trip");
  return value;
}

std::string CanonicalUuidBytes(const std::string_view uuid) {
  Require(uuid.size() == 36 && uuid[8] == '-' && uuid[13] == '-' &&
              uuid[18] == '-' && uuid[23] == '-',
          "UUID field shape is noncanonical");
  std::string hex;
  hex.reserve(32);
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    Require((uuid[index] >= '0' && uuid[index] <= '9') ||
                (uuid[index] >= 'a' && uuid[index] <= 'f'),
            "UUID field hex is noncanonical");
    hex.push_back(uuid[index]);
  }
  const auto raw = HexDecode(hex);
  Require(raw.size() == 16, "UUID field did not decode to 16 bytes");
  return raw;
}

void AppendU32Le(std::string* out, const std::uint32_t value) {
  Require(out != nullptr, "u32 output is missing");
  for (unsigned shift = 0; shift != 32; shift += 8) {
    out->push_back(static_cast<char>(value >> shift));
  }
}

void AppendU64Le(std::string* out, const std::uint64_t value) {
  Require(out != nullptr, "u64 output is missing");
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out->push_back(static_cast<char>(value >> shift));
  }
}

std::uint16_t ReadU16Le(const std::string_view bytes,
                        const std::size_t offset) {
  Require(offset <= bytes.size() && bytes.size() - offset >= 2,
          "u16 field is truncated");
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(bytes[offset]) |
      (static_cast<std::uint16_t>(
           static_cast<unsigned char>(bytes[offset + 1]))
       << 8));
}

std::uint32_t ReadU32Le(const std::string_view bytes,
                        const std::size_t offset) {
  Require(offset <= bytes.size() && bytes.size() - offset >= 4,
          "u32 field is truncated");
  std::uint32_t value = 0;
  for (unsigned byte = 0; byte != 4; ++byte) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[offset + byte]))
             << (byte * 8);
  }
  return value;
}

std::uint64_t ReadU64Le(const std::string_view bytes,
                        const std::size_t offset) {
  Require(offset <= bytes.size() && bytes.size() - offset >= 8,
          "u64 field is truncated");
  std::uint64_t value = 0;
  for (unsigned byte = 0; byte != 8; ++byte) {
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes[offset + byte]))
             << (byte * 8);
  }
  return value;
}

void AppendCanonicalField(std::string* out,
                          const std::string_view key,
                          const std::string_view value) {
  Require(out != nullptr, "canonical field output missing");
  out->append(std::to_string(key.size()));
  out->push_back(':');
  out->append(key);
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string Sha256Tagged(const std::string_view payload) {
  const auto* bytes = reinterpret_cast<const scratchbird::core::platform::byte*>(
      payload.data());
  const auto digest = hash::ComputeSha256Digest(bytes, payload.size());
  Require(digest.ok(), "test SHA-256 failed");
  return "sha256:" + hash::HexLower(digest.digest);
}

std::string Sha256Raw(const std::string_view payload) {
  const auto* bytes = reinterpret_cast<const scratchbird::core::platform::byte*>(
      payload.data());
  const auto digest = hash::ComputeSha256Digest(bytes, payload.size());
  Require(digest.ok(), "test raw SHA-256 failed");
  return std::string(reinterpret_cast<const char*>(digest.digest.data()),
                     digest.digest.size());
}

std::string RecomputeTextBatchHash(
    const std::vector<std::string>& fields) {
  Require(fields.size() == kTextMigrationSingleRowFields && fields[16] == "1",
          "forged text batch shape changed");
  std::string payload;
  const auto field = [&](const std::string_view key,
                         const std::string_view value) {
    AppendCanonicalField(&payload, key, value);
  };
  field("format_version", fields[4]);
  field("seal_state", fields[5]);
  field("migration_id", fields[7]);
  field("creator_tx", fields[2]);
  field("event_sequence", fields[3]);
  field("transaction_uuid", fields[8]);
  field("datatype_catalog_snapshot_uuid", fields[13]);
  field("datatype_catalog_generation", fields[14]);
  field("datatype_registry_generation", fields[15]);
  field("prior_catalog_snapshot_uuid", fields[9]);
  field("new_catalog_snapshot_uuid", fields[10]);
  field("prior_catalog_generation", fields[11]);
  field("new_catalog_generation", fields[12]);
  field("mutation_count", fields[16]);
  const std::size_t base = kTextMigrationHeaderFields;
  for (const auto& [key, offset] :
       std::vector<std::pair<std::string_view, std::size_t>>{
           {"object_uuid", 0}, {"column_uuid", 1},
           {"old_descriptor_uuid", 2}, {"new_descriptor_uuid", 3},
           {"old_type_uuid", 4}, {"new_type_uuid", 5},
           {"new_codec_uuid", 6}, {"new_codec_id", 7},
           {"new_codec_version", 8}, {"new_codec_generation", 9},
           {"old_row_generation", 10}, {"new_row_generation", 11},
           {"decision_sha256", 12}}) {
    field(key, fields[base + offset]);
  }
  field("table_default_name", DecodeCanonicalHex(fields[base + 13]));
  field("table_columns",
        EncodeCanonicalPairVector(
            DecodeCanonicalPairVector(fields[base + 14])));
  field("relation_descriptor_uuid", fields[base + 16]);
  field("relation_descriptor_generation", fields[base + 17]);
  field("descriptor_field_count", fields[base + 18]);
  field("descriptor_field_bytes", fields[base + 19]);
  field("contextual_sidecar_count", fields[base + 20]);
  field("relation_descriptor_fields",
        EncodeCanonicalPairVector(
            DecodeCanonicalPairVector(fields[base + 15])));
  return Sha256Tagged(payload);
}

std::string RecomputeTextDecisionHash(
    const std::vector<std::string>& fields) {
  Require(fields.size() == kTextMigrationSingleRowFields && fields[16] == "1",
          "forged text decision shape changed");
  const std::size_t base = kTextMigrationHeaderFields;
  std::string payload;
  const auto field = [&](const std::string_view key,
                         const std::string_view value) {
    AppendCanonicalField(&payload, key, value);
  };
  field("migration_id", fields[7]);
  field("transaction_uuid", fields[8]);
  field("datatype_catalog_snapshot_uuid", fields[13]);
  field("datatype_catalog_generation", fields[14]);
  field("datatype_registry_generation", fields[15]);
  field("prior_catalog_snapshot_uuid", fields[9]);
  field("new_catalog_snapshot_uuid", fields[10]);
  field("prior_catalog_generation", fields[11]);
  field("new_catalog_generation", fields[12]);
  for (const auto& [key, offset] :
       std::vector<std::pair<std::string_view, std::size_t>>{
           {"object_uuid", 0}, {"column_uuid", 1},
           {"old_descriptor_uuid", 2}, {"new_descriptor_uuid", 3},
           {"old_type_uuid", 4}, {"new_type_uuid", 5},
           {"new_codec_uuid", 6}, {"new_codec_id", 7},
           {"new_codec_version", 8}, {"new_codec_generation", 9},
           {"old_row_generation", 10}, {"new_row_generation", 11}}) {
    field(key, fields[base + offset]);
  }
  field("relation_descriptor_uuid", fields[base + 16]);
  field("relation_descriptor_generation", fields[base + 17]);
  field("descriptor_field_count", fields[base + 18]);
  field("descriptor_field_bytes", fields[base + 19]);
  field("contextual_sidecar_count", fields[base + 20]);
  field("relation_descriptor_fields",
        EncodeCanonicalPairVector(
            DecodeCanonicalPairVector(fields[base + 15])));
  return Sha256Tagged(payload);
}

const std::string& UniquePairValue(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    const std::string_view key) {
  const std::string* found = nullptr;
  for (const auto& [candidate_key, candidate_value] : pairs) {
    if (candidate_key != key) continue;
    Require(found == nullptr, "sealed descriptor vector key is duplicated");
    found = &candidate_value;
  }
  Require(found != nullptr, "sealed descriptor vector key is missing");
  return *found;
}

void RequireExactTextMigrationSealedVector(
    const std::vector<std::string>& fields,
    const Fixture& fixture,
    const std::string_view expected_table_uuid,
    const std::string_view expected_column_uuid,
    const bool expect_contextual_sidecar) {
  constexpr std::size_t kRow = kTextMigrationHeaderFields;
  Require(fields.size() == kTextMigrationSingleRowFields &&
              fields[0] == "SBMGA1" &&
              fields[1] == "TEXT_IDENTITY_MIGRATION_BATCH" &&
              fields[4] == "datatype_text_identity_migration_v1" &&
              fields[5] == "sealed" &&
              fields[7] == "core.datatype.text.identity.v1" &&
              fields[13] == kDatatypeCatalogSnapshot && fields[14] == "1" &&
              fields[15] == "1" && fields[16] == "1" &&
              fields[kRow] == expected_table_uuid &&
              fields[kRow + 1] == expected_column_uuid &&
              fields[kRow + 2] == kLegacyText &&
              fields[kRow + 3] == kCanonicalTextDescriptor &&
              fields[kRow + 4] == kLegacyText &&
              fields[kRow + 5] == kCanonicalTextType &&
              fields[kRow + 6] == kCanonicalTextCodec &&
              fields[kRow + 7] == "datatype.text.utf8.v1" &&
              fields[kRow + 8] == "1" && fields[kRow + 9] == "1" &&
              fields[kRow + 21] == "0" && fields[kRow + 22].empty() &&
              fields[kRow + 23].empty() && fields[kRow + 24].empty(),
          "sealed TEXT migration row identity or 25-field shape changed");
  Require(CanonicalU64(fields[2]) != 0 && CanonicalU64(fields[3]) != 0 &&
              CanonicalU64(fields[kRow + 10]) != 0 &&
              CanonicalU64(fields[kRow + 11]) == CanonicalU64(fields[3]),
          "sealed TEXT migration owner generation is invalid");
  Require(fields[6].size() == 71 && fields[6].starts_with("sha256:") &&
              IsLowerHex(std::string_view(fields[6]).substr(7)) &&
              fields[kRow + 12].size() == 71 &&
              fields[kRow + 12].starts_with("sha256:") &&
              IsLowerHex(std::string_view(fields[kRow + 12]).substr(7)),
          "sealed TEXT migration hash carrier is noncanonical");

  const auto descriptor_fields =
      DecodeCanonicalPairVector(fields[kRow + 15]);
  Require(EncodeCanonicalPairVector(descriptor_fields) ==
              fields[kRow + 15] &&
              descriptor_fields.size() >= 4,
          "sealed descriptor vector did not round trip canonically");
  for (std::size_t left = 0; left < descriptor_fields.size(); ++left) {
    for (std::size_t right = left + 1; right < descriptor_fields.size();
         ++right) {
      Require(descriptor_fields[left].first != descriptor_fields[right].first,
              "sealed descriptor vector contains duplicate keys");
    }
  }

  const auto descriptor_generation = CanonicalU64(fields[kRow + 17]);
  const auto descriptor_field_count = CanonicalU64(fields[kRow + 18]);
  const auto descriptor_field_bytes = CanonicalU64(fields[kRow + 19]);
  const auto contextual_sidecar_count = CanonicalU64(fields[kRow + 20]);
  Require(fields[kRow + 16] ==
              UniquePairValue(descriptor_fields, "descriptor_uuid") &&
              fields[kRow] ==
                  UniquePairValue(descriptor_fields, "relation_uuid") &&
              fields[kRow + 17] ==
                  UniquePairValue(descriptor_fields,
                                  "descriptor_generation") &&
              fields[3] ==
                  UniquePairValue(descriptor_fields, "relation_generation") &&
              descriptor_generation != 0 &&
              descriptor_field_count == descriptor_fields.size() &&
              descriptor_field_bytes == fields[kRow + 15].size() &&
              contextual_sidecar_count ==
                  static_cast<std::uint64_t>(expect_contextual_sidecar),
          "sealed descriptor vector header is not exact");

  const std::size_t seal_index = descriptor_fields.size() - 1;
  const std::size_t contextual_suffix_index =
      seal_index - (expect_contextual_sidecar ? 2 : 0);
  for (std::size_t index = 0; index < contextual_suffix_index; ++index) {
    Require(descriptor_fields[index].first.find(
                "contextual_text_descriptor_sidecar_v2") ==
                std::string::npos,
            "base descriptor vector contains a contextual reserved key");
  }
  Require(descriptor_fields[seal_index].first == kContextualTextSetSealKey &&
              descriptor_fields[seal_index].second.size() == 32,
          "contextual descriptor final seal ordering changed");

  std::string descriptor_evidence;
  if (expect_contextual_sidecar) {
    const std::size_t blob_index = contextual_suffix_index;
    const std::size_t hash_index = contextual_suffix_index + 1;
    Require(descriptor_fields[blob_index].first == kContextualTextBlobKey &&
                descriptor_fields[hash_index].first == kContextualTextHashKey,
            "contextual descriptor blob/hash ordering changed");

    const std::string_view blob = descriptor_fields[blob_index].second;
    Require(blob.size() == 533 && blob.substr(0, 8) == "SBTLTD02" &&
                ReadU16Le(blob, 8) == 2 && ReadU16Le(blob, 10) == 512 &&
                ReadU32Le(blob, 12) == 533 && ReadU32Le(blob, 16) == 1 &&
                static_cast<unsigned char>(blob[21]) == 1 &&
                blob.substr(24, 16) ==
                    CanonicalUuidBytes(kCanonicalTextDescriptor) &&
                ReadU64Le(blob, 40) == 1 &&
                blob.substr(48, 16) ==
                    CanonicalUuidBytes(kCanonicalTextType) &&
                ReadU64Le(blob, 64) == 1 &&
                blob.substr(72, 16) ==
                    CanonicalUuidBytes(kCanonicalTextCodec) &&
                ReadU16Le(blob, 88) == 21 && ReadU16Le(blob, 90) == 1 &&
                ReadU64Le(blob, 96) == 1 && ReadU64Le(blob, 104) == 256 &&
                blob.substr(120, 16) ==
                    CanonicalUuidBytes(fixture.charset_uuid) &&
                ReadU64Le(blob, 136) == fixture.charset_generation &&
                blob.substr(144, 16) ==
                    CanonicalUuidBytes(fixture.collation_uuid) &&
                ReadU64Le(blob, 160) == fixture.collation_generation &&
                blob.substr(408, 16) ==
                    CanonicalUuidBytes(kDatatypeCatalogSnapshot) &&
                ReadU64Le(blob, 424) == 1 && ReadU64Le(blob, 432) == 1 &&
                ReadU64Le(blob, 440) == fixture.resource_epoch &&
                blob.substr(512, 21) == "datatype.text.utf8.v1",
            "stored contextual SBTLTD02 blob is not exact");

    std::string descriptor_hash_material =
        "ScratchBird.ContextualText.TextDescriptor.V2";
    descriptor_hash_material.push_back('\0');
    std::string canonical_blob(blob);
    std::fill(canonical_blob.begin() + 448, canonical_blob.begin() + 480,
              '\0');
    descriptor_hash_material += canonical_blob;
    descriptor_evidence = Sha256Raw(descriptor_hash_material);
    Require(descriptor_fields[hash_index].second.size() == 32 &&
                descriptor_fields[hash_index].second == descriptor_evidence &&
                blob.substr(448, 32) == descriptor_evidence,
            "stored contextual descriptor evidence hash is not exact");
  }

  std::vector<std::pair<std::string, std::string>> pre_seal_fields(
      descriptor_fields.begin(), descriptor_fields.begin() + seal_index);
  const auto pre_seal_serialization =
      EncodeCanonicalPairVector(pre_seal_fields);
  Require(pre_seal_serialization.size() + 172 == descriptor_field_bytes &&
              descriptor_fields[seal_index].second.size() == 32,
          "sealed descriptor count/byte equation changed");
  std::string seal_material =
      "ScratchBird.ContextualText.SealedRelationDescriptorSidecarSet.V2";
  seal_material.push_back('\0');
  AppendU64Le(&seal_material, CanonicalU64(fields[2]));
  AppendU64Le(&seal_material, CanonicalU64(fields[3]));
  seal_material += CanonicalUuidBytes(fields[kRow]);
  seal_material += CanonicalUuidBytes(fields[kRow + 16]);
  AppendU64Le(&seal_material, descriptor_generation);
  AppendU64Le(&seal_material, pre_seal_fields.size());
  AppendU64Le(&seal_material, pre_seal_serialization.size());
  seal_material += pre_seal_serialization;
  AppendU32Le(&seal_material,
              static_cast<std::uint32_t>(contextual_sidecar_count));
  if (expect_contextual_sidecar) {
    AppendU32Le(&seal_material, 0);
    seal_material += CanonicalUuidBytes(expected_column_uuid);
    seal_material += descriptor_evidence;
  }
  Require(descriptor_fields[seal_index].second == Sha256Raw(seal_material),
          "sealed descriptor complete-vector hash is not exact");

  Require(fields[kRow + 12] == RecomputeTextDecisionHash(fields) &&
              fields[6] == RecomputeTextBatchHash(fields),
          "sealed TEXT migration nested or outer hash changed");
}

Fixture MakeFixture(const std::string_view mode) {
  Fixture fixture;
  fixture.path = std::filesystem::temp_directory_path() /
      ("sb_bigint_identity_migration_" + std::string(mode) + "_" +
       std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".sbdb");
  db::DatabaseCreateConfig config;
  config.path = fixture.path.string();
  config.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1786830000001ull).value;
  config.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1786830000002ull).value;
  config.page_size = 16384;
  config.creation_unix_epoch_millis = 1786830000003ull;
  config.allow_minimal_resource_bootstrap = true;
  config.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  config.require_resource_seed_pack = true;
  config.allow_minimal_resource_bootstrap = false;
  config.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(config);
  if (!created.ok()) {
    std::cerr << "create_database_diagnostic code="
              << created.diagnostic.diagnostic_code
              << " key=" << created.diagnostic.message_key
              << " detail=";
    for (std::size_t index = 0;
         index < created.diagnostic.arguments.size(); ++index) {
      if (index != 0) std::cerr << ',';
      std::cerr << created.diagnostic.arguments[index].key << '='
                << created.diagnostic.arguments[index].value;
    }
    std::cerr << " remediation=" << created.diagnostic.remediation_hint
              << '\n';
  }
  Require(created.ok(), "database creation failed");
  fixture.resource_epoch = created.state.resource_seed_catalog.resource_epoch;
  for (const auto& charset : created.state.resource_seed_catalog.charsets) {
    if ((charset.canonical_name == "UTF8" ||
         charset.canonical_name == "UTF-8") &&
        !charset.default_collation_uuid.empty()) {
      fixture.charset_uuid = charset.resource_uuid;
      fixture.charset_generation = charset.family_epoch;
      fixture.collation_uuid = charset.default_collation_uuid;
      break;
    }
  }
  for (const auto& collation :
       created.state.resource_seed_catalog.collations) {
    if (collation.resource_uuid == fixture.collation_uuid) {
      fixture.collation_generation = collation.family_epoch;
      break;
    }
  }
  Require(fixture.resource_epoch != 0 && !fixture.charset_uuid.empty() &&
              fixture.charset_generation != 0 &&
              !fixture.collation_uuid.empty() &&
              fixture.collation_generation != 0,
          "canonical UTF8 resource authority unavailable");
  const auto inventory = db::PersistLocalTransactionInventoryToDatabase(
      fixture.path.string(),
      scratchbird::transaction::mga::MakeEmptyLocalTransactionInventory());
  Require(inventory.ok(), "transaction inventory initialization failed");
  fixture.database_uuid = uuid::UuidToString(config.database_uuid.value);
  return fixture;
}

api::EngineRequestContext BaseContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.request_id = "ia01-bigint-identity-migration";
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.current_schema_uuid.canonical = fixture.baseline_schema_uuid;
  context.session_uuid.canonical = Id(UuidKind::object, 3);
  context.principal_uuid.canonical = Id(UuidKind::principal, 4);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = fixture.resource_epoch;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.name_resolution_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  return context;
}

api::EngineRequestContext Begin(api::EngineRequestContext context) {
  api::EngineBeginTransactionRequest request;
  request.context = context;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok && !begun.diagnostics.empty()) {
    std::cerr << begun.diagnostics.front().code << ':'
              << begun.diagnostics.front().message_key << ':'
              << begun.diagnostics.front().detail << '\n';
  }
  Require(begun.ok, "transaction begin failed");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  Require(api::EngineCommitTransaction(request).ok, "transaction commit failed");
}
void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  Require(api::EngineRollbackTransaction(request).ok, "transaction rollback failed");
}

api::CrudTableRecord LegacyTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "migration_target";
  table.columns.push_back({"id", "column_uuid=" + fixture.column_uuid +
      ";canonical=bigint;type_uuid=" + kLegacy + ";nullability=non_null"});
  return table;
}

api::MgaBigintIdentityMigrationRequest Migration(const Fixture& fixture,
                                                  std::uint64_t old_generation) {
  api::MgaBigintIdentityMigrationRequest request;
  request.prior_catalog_snapshot_uuid = Id(UuidKind::object, 30);
  request.new_catalog_snapshot_uuid = Id(UuidKind::object, 31);
  request.prior_catalog_generation = 7;
  request.new_catalog_generation = 8;
  request.rows.push_back({fixture.table_uuid, fixture.column_uuid, old_generation});
  return request;
}

api::CrudTableRecord LegacyInt32Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.int32_table_uuid;
  table.default_name = "int32_migration_target";
  const auto descriptor = [](const std::string& column_uuid,
                             const char* name) {
    return "column_uuid=" + column_uuid + ";canonical=" + name +
        ";datatype_descriptor_uuid=" + kLegacyInt32 +
        ";type_uuid=" + kLegacyInt32 +
        ";codec_id=datatype.int32.le.v1;nullability=non_null";
  };
  table.columns.push_back(
      {"a", descriptor(fixture.int32_column_a_uuid, "int")});
  table.columns.push_back(
      {"b", descriptor(fixture.int32_column_b_uuid, "int32")});
  return table;
}

api::CrudTableRecord ContradictoryInt32Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.int32_conflict_table_uuid;
  table.default_name = "int32_migration_conflict";
  table.columns.push_back({
      "id",
      "column_uuid=" + fixture.int32_conflict_column_uuid +
          ";canonical=int;datatype_descriptor_uuid=" +
          kCanonicalInt32Descriptor + ";type_uuid=" + kLegacyInt32 +
          ";codec_id=datatype.int32.le.v1;nullability=non_null"});
  return table;
}

api::MgaInt32IdentityMigrationRequest Int32Migration(
    const Fixture& fixture,
    std::uint64_t old_generation) {
  api::MgaInt32IdentityMigrationRequest request;
  request.prior_catalog_snapshot_uuid = Id(UuidKind::object, 32);
  request.new_catalog_snapshot_uuid = Id(UuidKind::object, 33);
  request.prior_catalog_generation = 11;
  request.new_catalog_generation = 12;
  request.rows.push_back(
      {fixture.int32_table_uuid, fixture.int32_column_a_uuid, old_generation});
  request.rows.push_back(
      {fixture.int32_table_uuid, fixture.int32_column_b_uuid, old_generation});
  return request;
}

api::CrudTableRecord LegacyTextTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.text_table_uuid;
  table.default_name = "text_migration_target";
  table.columns.push_back({
      "payload",
      "type=text;nullable=true;datatype_descriptor_uuid=" +
          std::string(kLegacyText) + ";type_uuid=" + kLegacyText});
  return table;
}

api::CrudTableRecord ContradictoryTextTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.text_conflict_table_uuid;
  table.default_name = "text_migration_conflict";
  table.columns.push_back({
      "payload",
      "type=text;nullable=true;datatype_descriptor_uuid=" +
          std::string(kLegacyText) + ";type_uuid=" + kLegacyText +
          ";codec_id=datatype.text.utf8.v1"});
  return table;
}

api::CrudTableRecord SemanticConflictTextTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.text_semantic_table_uuid;
  table.default_name = "text_semantic_migration_conflict";
  table.columns.push_back({
      "payload",
      "type=text;nullable=true;datatype_descriptor_uuid=" +
          std::string(kLegacyText) + ";type_uuid=" + kLegacyText +
          ";padding=space"});
  return table;
}

api::CrudTableRecord ResourceBoundLegacyTextTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.text_resource_table_uuid;
  table.default_name = "text_resource_migration_target";
  table.columns.push_back({
      "payload",
      "type=text;nullable=true;datatype_descriptor_uuid=" +
          std::string(kLegacyText) + ";type_uuid=" + kLegacyText +
          ";character_length=256;charset_uuid=" + fixture.charset_uuid +
          ";charset_generation=" +
          std::to_string(fixture.charset_generation) +
          ";collation_uuid=" + fixture.collation_uuid +
          ";collation_generation=" +
          std::to_string(fixture.collation_generation) +
          ";resource_epoch=" + std::to_string(fixture.resource_epoch)});
  return table;
}

api::CrudTableRecord StaleColumnLegacyTextTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.text_stale_column_table_uuid;
  table.default_name = "text_stale_column_migration_target";
  table.columns.push_back({
      "payload",
      "type=text;nullable=true;datatype_descriptor_uuid=" +
          std::string(kLegacyText) + ";type_uuid=" + kLegacyText});
  return table;
}

api::CrudTableRecord ResourceConflictLegacyTextTable(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.table_uuid = fixture.text_resource_conflict_table_uuid;
  table.default_name = "text_resource_migration_conflict";
  table.columns.push_back({
      "payload",
      "type=text;nullable=true;datatype_descriptor_uuid=" +
          std::string(kLegacyText) + ";type_uuid=" + kLegacyText +
          ";character_length=256;charset_uuid=" + fixture.charset_uuid +
          ";collation_uuid=" + fixture.collation_uuid});
  return table;
}

api::CrudTableRecord StaleResourceGenerationLegacyTextTable(
    const Fixture& fixture,
    const bool stale_charset) {
  api::CrudTableRecord table;
  table.table_uuid = stale_charset ? fixture.text_stale_charset_table_uuid
                                   : fixture.text_stale_collation_table_uuid;
  table.default_name = stale_charset
                           ? "text_stale_charset_generation_migration_conflict"
                           : "text_stale_collation_generation_migration_conflict";
  table.columns.push_back({
      "payload",
      "type=text;nullable=true;datatype_descriptor_uuid=" +
          std::string(kLegacyText) + ";type_uuid=" + kLegacyText +
          ";character_length=256;charset_uuid=" + fixture.charset_uuid +
          ";charset_generation=" +
          std::to_string(fixture.charset_generation +
                         (stale_charset ? 1 : 0)) +
          ";collation_uuid=" + fixture.collation_uuid +
          ";collation_generation=" +
          std::to_string(fixture.collation_generation +
                         (stale_charset ? 0 : 1)) +
          ";resource_epoch=" + std::to_string(fixture.resource_epoch)});
  return table;
}

api::MgaTextIdentityMigrationRequest TextMigration(
    const Fixture& fixture,
    std::uint64_t old_generation,
    bool conflict = false) {
  api::MgaTextIdentityMigrationRequest request;
  request.prior_catalog_snapshot_uuid = Id(UuidKind::object, 36);
  request.new_catalog_snapshot_uuid = Id(UuidKind::object, 37);
  request.prior_catalog_generation = 21;
  request.new_catalog_generation = 22;
  request.rows.push_back({
      conflict ? fixture.text_conflict_table_uuid : fixture.text_table_uuid,
      conflict ? fixture.text_conflict_column_uuid : fixture.text_column_uuid,
      old_generation});
  return request;
}

api::MgaTextIdentityMigrationRequest TextMigrationFor(
    const Fixture& fixture,
    const std::string& object_uuid,
    const std::string& column_uuid,
    const std::uint64_t old_generation) {
  auto request = TextMigration(fixture, old_generation);
  request.rows.clear();
  request.rows.push_back({object_uuid, column_uuid, old_generation});
  return request;
}

api::EngineRequestContext BeginTextMigration(
    const Fixture& fixture,
    const api::MgaTextIdentityMigrationRequest& request) {
  auto context = BaseContext(fixture);
  context.catalog_generation_id = request.prior_catalog_generation;
  context.statement_metadata_snapshot_uuid.canonical =
      request.prior_catalog_snapshot_uuid;
  return Begin(std::move(context));
}

api::EngineLocalizedName PrimaryName(std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "primary";
  localized.name = std::move(name);
  localized.raw_name_text = localized.name;
  localized.display_name = localized.name;
  localized.default_name = true;
  return localized;
}

std::string VisibleDescriptor(const api::EngineRequestContext& context,
                              const Fixture& fixture,
                              const std::string& table_uuid = {}) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << "relation metadata recovery diagnostic: code="
              << loaded.diagnostic.code
              << " key=" << loaded.diagnostic.message_key
              << " detail=" << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "relation metadata recovery load failed");
  const auto newest = api::FindVisibleCrudTable(
      loaded.state.crud_metadata,
      table_uuid.empty() ? fixture.table_uuid : table_uuid,
      context.local_transaction_id);
  Require(newest.has_value() && !newest->columns.empty(),
          "visible table projection missing");
  return newest->columns.front().second;
}

std::uint64_t VisibleTableGeneration(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "visible table generation load failed");
  const auto table = api::FindVisibleCrudTable(
      loaded.state.crud_metadata, table_uuid, context.local_transaction_id);
  Require(table.has_value(), "visible table generation missing");
  return table->event_sequence;
}

std::vector<std::string> VisibleDescriptors(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "relation metadata recovery load failed");
  const auto newest = api::FindVisibleCrudTable(
      loaded.state.crud_metadata, table_uuid, context.local_transaction_id);
  Require(newest.has_value(), "visible table projection missing");
  std::vector<std::string> descriptors;
  for (const auto& [name, descriptor] : newest->columns) {
    (void)name;
    descriptors.push_back(descriptor);
  }
  return descriptors;
}

struct RefusalArtifactSnapshot {
  std::string metadata;
  std::string allocator;
  std::uint64_t max_event_sequence = 0;
};

RefusalArtifactSnapshot CaptureRefusalArtifacts(
    const api::EngineRequestContext& context,
    const Fixture& fixture) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "refusal artifact state load failed");
  return {
      ReadFileBytes(fixture.path.string() + ".sb.mga_relation_metadata"),
      ReadFileBytes(fixture.path.string() + ".sb.mga_event_sequences"),
      loaded.state.crud_metadata.max_event_sequence};
}

void RequireRefusalArtifactsUnchanged(
    const RefusalArtifactSnapshot& before,
    const api::EngineRequestContext& context,
    const Fixture& fixture,
    const char* message) {
  const auto after = CaptureRefusalArtifacts(context, fixture);
  Require(after.metadata == before.metadata &&
              after.allocator == before.allocator &&
              after.max_event_sequence == before.max_event_sequence,
          message);
}

std::string DescriptorField(const std::string_view descriptor,
                            const std::string_view key) {
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string_view::npos ? descriptor.size() - start
                                             : end - start);
    const auto separator = field.find('=');
    if (separator != std::string_view::npos &&
        field.substr(0, separator) == key) {
      return std::string(field.substr(separator + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return {};
}

std::string EnsureLegacyTextRelationDescriptor(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "legacy text metadata load failed");
  const auto table = api::FindVisibleCrudTable(
      loaded.state.crud_metadata, table_uuid, context.local_transaction_id);
  Require(table.has_value(), "legacy text table is not visible");
  api::MgaRelationStorageDescriptor descriptor;
  const auto ensured = api::EnsureMgaRelationStorageDescriptor(
      context, *table, {}, &descriptor);
  Require(!ensured.error && descriptor.columns.size() == 1,
          "legacy text relation descriptor seed failed");
  const auto& column = descriptor.columns.front();
  const std::string expected_encoded_descriptor =
      table->columns.front().second + ";column_uuid=" +
      column.column_uuid.canonical;
  Require(!CanonicalUuidBytes(column.column_uuid.canonical).empty() &&
              column.value_descriptor.descriptor_uuid.canonical ==
                  column.column_uuid.canonical &&
              DescriptorField(column.value_descriptor.encoded_descriptor,
                              "datatype_descriptor_uuid") == kLegacyText &&
              column.value_descriptor.encoded_descriptor ==
                  expected_encoded_descriptor,
          "legacy text relation descriptor seed failed");
  return column.column_uuid.canonical;
}
}  // namespace

int main(const int argc, char* argv[]) {
  const auto mode = ParseFixtureMode(argc, argv);
  ConfigureMemoryFixture();
  auto fixture = MakeFixture(argv[1]);
  auto seed = Begin(BaseContext(fixture));
  Require(!api::AppendMgaTableMetadata(seed, LegacyTable(fixture)).error,
          "legacy metadata seed failed");
  Require(!api::AppendMgaTableMetadata(seed, LegacyInt32Table(fixture)).error,
          "legacy int32 metadata seed failed");
  Require(!api::AppendMgaTableMetadata(seed,
                                       ContradictoryInt32Table(fixture)).error,
          "contradictory int32 metadata seed failed");
  Require(!api::AppendMgaTableMetadata(seed, LegacyTextTable(fixture)).error,
          "legacy text metadata seed failed");
  Require(!api::AppendMgaTableMetadata(
               seed, ContradictoryTextTable(fixture)).error,
          "contradictory text metadata seed failed");
  Require(!api::AppendMgaTableMetadata(
               seed, SemanticConflictTextTable(fixture)).error,
          "semantic-conflict text metadata seed failed");
  Require(!api::AppendMgaTableMetadata(
               seed, ResourceBoundLegacyTextTable(fixture)).error,
          "resource-bound text metadata seed failed");
  Require(!api::AppendMgaTableMetadata(
               seed, StaleColumnLegacyTextTable(fixture)).error,
          "stale-column text metadata seed failed");
  Require(!api::AppendMgaTableMetadata(
               seed, ResourceConflictLegacyTextTable(fixture)).error,
          "resource-conflict text metadata seed failed");
  Require(!api::AppendMgaTableMetadata(
               seed, StaleResourceGenerationLegacyTextTable(fixture, true)).error,
          "stale-charset-generation text metadata seed failed");
  Require(!api::AppendMgaTableMetadata(
               seed, StaleResourceGenerationLegacyTextTable(fixture, false)).error,
          "stale-collation-generation text metadata seed failed");
  fixture.text_column_uuid = EnsureLegacyTextRelationDescriptor(
      seed, fixture.text_table_uuid);
  fixture.text_conflict_column_uuid = EnsureLegacyTextRelationDescriptor(
      seed, fixture.text_conflict_table_uuid);
  fixture.text_semantic_column_uuid = EnsureLegacyTextRelationDescriptor(
      seed, fixture.text_semantic_table_uuid);
  fixture.text_resource_column_uuid = EnsureLegacyTextRelationDescriptor(
      seed, fixture.text_resource_table_uuid);
  fixture.text_stale_column_uuid = EnsureLegacyTextRelationDescriptor(
      seed, fixture.text_stale_column_table_uuid);
  fixture.text_resource_conflict_column_uuid =
      EnsureLegacyTextRelationDescriptor(
          seed, fixture.text_resource_conflict_table_uuid);
  fixture.text_stale_charset_column_uuid = EnsureLegacyTextRelationDescriptor(
      seed, fixture.text_stale_charset_table_uuid);
  fixture.text_stale_collation_column_uuid =
      EnsureLegacyTextRelationDescriptor(
          seed, fixture.text_stale_collation_table_uuid);
  Commit(seed);

  // Advance only the catalog table row. The persisted physical column
  // descriptor intentionally remains at the prior generation for the focused
  // stale-column refusal below.
  auto stale_column_seed = Begin(BaseContext(fixture));
  Require(!api::AppendMgaTableMetadata(
               stale_column_seed, StaleColumnLegacyTextTable(fixture)).error,
          "stale-column successor metadata seed failed");
  Commit(stale_column_seed);

  if (mode == FixtureMode::kNumericIdentityMigration) {
  auto rollback_tx = Begin(BaseContext(fixture));
  auto rollback_request = Migration(fixture, 1);
  rollback_tx.statement_metadata_snapshot_uuid.canonical =
      rollback_request.prior_catalog_snapshot_uuid;
  const auto appended = api::AppendMgaBigintIdentityMigrationBatch(
      rollback_tx, rollback_request);
  Require(appended.ok && appended.migrated_row_count == 1 &&
              appended.decision_sha256.starts_with("sha256:"),
          "sealed migration append failed");
  Require(VisibleDescriptor(rollback_tx, fixture).find(kCanonical) != std::string::npos,
          "creator transaction did not see migration");
  Rollback(rollback_tx);

  auto after_rollback = Begin(BaseContext(fixture));
  Require(VisibleDescriptor(after_rollback, fixture).find(kLegacy) != std::string::npos,
          "rolled-back migration became visible");
  Rollback(after_rollback);

  // Recovery ignores a torn physical append because it has neither a complete
  // shape nor a seal/hash with publication authority.
  {
    std::ofstream out(fixture.path.string() + ".sb.mga_relation_metadata",
                      std::ios::app | std::ios::binary);
    out << "SBMGA1\tBIGINT_IDENTITY_MIGRATION_BATCH\t999\n";
  }
  auto torn_recovery = Begin(BaseContext(fixture));
  Require(VisibleDescriptor(torn_recovery, fixture).find(kLegacy) != std::string::npos,
          "torn migration append affected recovery visibility");
  Rollback(torn_recovery);

  auto commit_tx = Begin(BaseContext(fixture));
  auto commit_request = Migration(fixture, 1);
  commit_tx.statement_metadata_snapshot_uuid.canonical =
      commit_request.prior_catalog_snapshot_uuid;
  Require(api::AppendMgaBigintIdentityMigrationBatch(commit_tx, commit_request).ok,
          "committed migration append failed");
  Commit(commit_tx);

  auto recovered = Begin(BaseContext(fixture));
  const std::string descriptor = VisibleDescriptor(recovered, fixture);
  Require(descriptor.find(kCanonical) != std::string::npos &&
              descriptor.find(kLegacy) == std::string::npos,
          "committed migration did not recover canonically");
  const auto replay = api::AppendMgaBigintIdentityMigrationBatch(
      recovered, commit_request);
  Require(!replay.ok && replay.diagnostic.code == "DATATYPE.DESCRIPTOR_INVALID",
          "stale migration generation did not fail closed");
  Rollback(recovered);

  // The provisional INT identity used one UUID for both descriptor and type.
  // Its sealed migration replaces both fields together, including two
  // columns of the same table in one atomic catalog publication.
  auto cancelled = Begin(BaseContext(fixture));
  auto int32_request = Int32Migration(fixture, 2);
  cancelled.statement_metadata_snapshot_uuid.canonical =
      int32_request.prior_catalog_snapshot_uuid;
  std::uint32_t cancel_checks = 0;
  cancelled.query_cancellation_requested = [&cancel_checks]() {
    ++cancel_checks;
    return true;
  };
  const auto cancelled_result = api::AppendMgaInt32IdentityMigrationBatch(
      cancelled, int32_request);
  Require(!cancelled_result.ok &&
              cancelled_result.diagnostic.code == "PROCESS.CANCELLED" &&
              cancel_checks == 1,
          "pre-publication int32 migration cancellation was not fail-closed");
  Rollback(cancelled);

  auto int32_rollback = Begin(BaseContext(fixture));
  int32_rollback.statement_metadata_snapshot_uuid.canonical =
      int32_request.prior_catalog_snapshot_uuid;
  const auto int32_appended = api::AppendMgaInt32IdentityMigrationBatch(
      int32_rollback, int32_request);
  Require(int32_appended.ok && int32_appended.migrated_row_count == 2 &&
              int32_appended.decision_sha256.starts_with("sha256:"),
          "sealed int32 migration append failed");
  for (const auto& descriptor : VisibleDescriptors(
           int32_rollback, fixture.int32_table_uuid)) {
    Require(descriptor.find(kCanonicalInt32Descriptor) != std::string::npos &&
                descriptor.find(kCanonicalInt32Type) != std::string::npos &&
                descriptor.find(kLegacyInt32) == std::string::npos,
            "creator did not see both canonical int32 identities");
  }
  Rollback(int32_rollback);

  auto int32_after_rollback = Begin(BaseContext(fixture));
  for (const auto& descriptor : VisibleDescriptors(
           int32_after_rollback, fixture.int32_table_uuid)) {
    Require(descriptor.find(kLegacyInt32) != std::string::npos,
            "rolled-back int32 migration became visible");
  }
  Rollback(int32_after_rollback);

  // A torn record has no seal/hash authority and is ignored on recovery.
  {
    std::ofstream out(fixture.path.string() + ".sb.mga_relation_metadata",
                      std::ios::app | std::ios::binary);
    out << "SBMGA1\tINT32_IDENTITY_MIGRATION_BATCH\t999\n";
  }
  auto int32_commit = Begin(BaseContext(fixture));
  int32_commit.statement_metadata_snapshot_uuid.canonical =
      int32_request.prior_catalog_snapshot_uuid;
  Require(api::AppendMgaInt32IdentityMigrationBatch(
              int32_commit, int32_request).ok,
          "committed int32 migration append failed");
  Commit(int32_commit);

  auto int32_recovered = Begin(BaseContext(fixture));
  for (const auto& descriptor : VisibleDescriptors(
           int32_recovered, fixture.int32_table_uuid)) {
    Require(descriptor.find(kCanonicalInt32Descriptor) != std::string::npos &&
                descriptor.find(kCanonicalInt32Type) != std::string::npos &&
                descriptor.find(kLegacyInt32) == std::string::npos,
            "committed int32 migration did not recover canonically");
  }
  const auto int32_replay = api::AppendMgaInt32IdentityMigrationBatch(
      int32_recovered, int32_request);
  Require(!int32_replay.ok &&
              int32_replay.diagnostic.code == "DATATYPE.DESCRIPTOR_INVALID",
          "stale int32 migration replay did not fail closed");
  Rollback(int32_recovered);

  auto conflict_tx = Begin(BaseContext(fixture));
  auto conflict_request = Int32Migration(fixture, 3);
  conflict_request.rows.clear();
  conflict_request.rows.push_back({fixture.int32_conflict_table_uuid,
                                   fixture.int32_conflict_column_uuid, 3});
  conflict_tx.statement_metadata_snapshot_uuid.canonical =
      conflict_request.prior_catalog_snapshot_uuid;
  const auto conflict_result = api::AppendMgaInt32IdentityMigrationBatch(
      conflict_tx, conflict_request);
  Require(!conflict_result.ok &&
              conflict_result.diagnostic.code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "contradictory int32 descriptor/type carrier was accepted");
  Rollback(conflict_tx);
  return EXIT_SUCCESS;
  }

  // TEXT used the same provisional UUID for descriptor and type and carried
  // no codec authority.  The migration must publish the table row and the
  // complete relation-descriptor snapshot under one MGA-visible seal.
  if (mode == FixtureMode::kTextIdentityMigration) {
  const auto text_request = TextMigration(fixture, 4);

  auto text_wrong_receipt = BeginTextMigration(fixture, text_request);
  text_wrong_receipt.datatype_registry_generation = 2;
  const auto wrong_receipt_before = CaptureRefusalArtifacts(
      text_wrong_receipt, fixture);
  const auto wrong_receipt = api::AppendMgaTextIdentityMigrationBatch(
      text_wrong_receipt, text_request);
  Require(!wrong_receipt.ok &&
              wrong_receipt.diagnostic.code == "DATATYPE.DESCRIPTOR_INVALID",
          "wrong datatype receipt admitted text migration");
  RequireRefusalArtifactsUnchanged(
      wrong_receipt_before, text_wrong_receipt, fixture,
      "wrong datatype receipt changed migration artifacts");
  Rollback(text_wrong_receipt);

  auto text_missing_receipt = BeginTextMigration(fixture, text_request);
  text_missing_receipt.datatype_catalog_snapshot_uuid.canonical.clear();
  text_missing_receipt.datatype_catalog_generation = 0;
  text_missing_receipt.datatype_registry_generation = 0;
  const auto missing_receipt_before = CaptureRefusalArtifacts(
      text_missing_receipt, fixture);
  const auto missing_receipt = api::AppendMgaTextIdentityMigrationBatch(
      text_missing_receipt, text_request);
  Require(!missing_receipt.ok &&
              missing_receipt.diagnostic.code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "missing datatype receipt admitted text migration");
  RequireRefusalArtifactsUnchanged(
      missing_receipt_before, text_missing_receipt, fixture,
      "missing datatype receipt changed migration artifacts");
  Rollback(text_missing_receipt);

  auto text_stale_column = BeginTextMigration(fixture, text_request);
  const auto stale_column_request = TextMigrationFor(
      fixture, fixture.text_stale_column_table_uuid,
      fixture.text_stale_column_uuid,
      VisibleTableGeneration(text_stale_column,
                             fixture.text_stale_column_table_uuid));
  const auto stale_column_before = CaptureRefusalArtifacts(
      text_stale_column, fixture);
  const auto stale_column = api::AppendMgaTextIdentityMigrationBatch(
      text_stale_column, stale_column_request);
  Require(!stale_column.ok &&
              stale_column.diagnostic.code == "DATATYPE.DESCRIPTOR_INVALID",
          "stale TEXT row/column generation admitted migration");
  RequireRefusalArtifactsUnchanged(
      stale_column_before, text_stale_column, fixture,
      "stale TEXT row/column generation changed migration artifacts");
  Rollback(text_stale_column);

  auto text_duplicate = BeginTextMigration(fixture, text_request);
  auto duplicate_request = text_request;
  const auto duplicate_row = duplicate_request.rows.front();
  duplicate_request.rows.push_back(duplicate_row);
  const auto duplicate_before = CaptureRefusalArtifacts(
      text_duplicate, fixture);
  const auto duplicate = api::AppendMgaTextIdentityMigrationBatch(
      text_duplicate, duplicate_request);
  if (duplicate.ok ||
      duplicate.diagnostic.code != "CORE.AUTHORITY.CONFLICT") {
    std::cerr << "duplicate TEXT diagnostic: ok="
              << (duplicate.ok ? "true" : "false")
              << " code=" << duplicate.diagnostic.code
              << " key=" << duplicate.diagnostic.message_key
              << " detail=" << duplicate.diagnostic.detail;
    for (const auto& row : duplicate_request.rows) {
      std::cerr << " row=" << row.object_uuid << '/' << row.column_uuid
                << '/' << row.old_row_generation;
    }
    std::cerr << '\n';
  }
  Require(!duplicate.ok &&
              duplicate.diagnostic.code == "CORE.AUTHORITY.CONFLICT",
          "duplicate TEXT identity mapping admitted migration");
  RequireRefusalArtifactsUnchanged(
      duplicate_before, text_duplicate, fixture,
      "duplicate TEXT identity mapping changed migration artifacts");
  Rollback(text_duplicate);

  auto text_semantic = BeginTextMigration(fixture, text_request);
  const auto semantic_request = TextMigrationFor(
      fixture, fixture.text_semantic_table_uuid,
      fixture.text_semantic_column_uuid,
      VisibleTableGeneration(text_semantic, fixture.text_semantic_table_uuid));
  text_semantic.catalog_generation_id =
      semantic_request.prior_catalog_generation;
  text_semantic.statement_metadata_snapshot_uuid.canonical =
      semantic_request.prior_catalog_snapshot_uuid;
  const auto semantic_before = CaptureRefusalArtifacts(text_semantic, fixture);
  const auto semantic_result = api::AppendMgaTextIdentityMigrationBatch(
      text_semantic, semantic_request);
  Require(!semantic_result.ok &&
              semantic_result.diagnostic.code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "contradictory TEXT semantic carrier admitted migration");
  RequireRefusalArtifactsUnchanged(
      semantic_before, text_semantic, fixture,
      "contradictory TEXT semantic carrier changed migration artifacts");
  Rollback(text_semantic);

  auto text_resource_conflict = BeginTextMigration(fixture, text_request);
  const auto resource_conflict_request = TextMigrationFor(
      fixture, fixture.text_resource_conflict_table_uuid,
      fixture.text_resource_conflict_column_uuid,
      VisibleTableGeneration(text_resource_conflict,
                             fixture.text_resource_conflict_table_uuid));
  const auto resource_conflict_before = CaptureRefusalArtifacts(
      text_resource_conflict, fixture);
  const auto resource_conflict = api::AppendMgaTextIdentityMigrationBatch(
      text_resource_conflict, resource_conflict_request);
  Require(!resource_conflict.ok &&
              resource_conflict.diagnostic.code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "missing TEXT resource generations admitted migration");
  RequireRefusalArtifactsUnchanged(
      resource_conflict_before, text_resource_conflict, fixture,
      "missing TEXT resource generations changed migration artifacts");
  Rollback(text_resource_conflict);

  auto text_stale_charset = BeginTextMigration(fixture, text_request);
  const auto stale_charset_request = TextMigrationFor(
      fixture, fixture.text_stale_charset_table_uuid,
      fixture.text_stale_charset_column_uuid,
      VisibleTableGeneration(text_stale_charset,
                             fixture.text_stale_charset_table_uuid));
  const auto stale_charset_before = CaptureRefusalArtifacts(
      text_stale_charset, fixture);
  const auto stale_charset = api::AppendMgaTextIdentityMigrationBatch(
      text_stale_charset, stale_charset_request);
  Require(!stale_charset.ok &&
              stale_charset.diagnostic.code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "stale TEXT charset generation admitted migration");
  RequireRefusalArtifactsUnchanged(
      stale_charset_before, text_stale_charset, fixture,
      "stale TEXT charset generation changed migration artifacts");
  Rollback(text_stale_charset);

  auto text_stale_collation = BeginTextMigration(fixture, text_request);
  const auto stale_collation_request = TextMigrationFor(
      fixture, fixture.text_stale_collation_table_uuid,
      fixture.text_stale_collation_column_uuid,
      VisibleTableGeneration(text_stale_collation,
                             fixture.text_stale_collation_table_uuid));
  const auto stale_collation_before = CaptureRefusalArtifacts(
      text_stale_collation, fixture);
  const auto stale_collation = api::AppendMgaTextIdentityMigrationBatch(
      text_stale_collation, stale_collation_request);
  Require(!stale_collation.ok &&
              stale_collation.diagnostic.code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "stale TEXT collation generation admitted migration");
  RequireRefusalArtifactsUnchanged(
      stale_collation_before, text_stale_collation, fixture,
      "stale TEXT collation generation changed migration artifacts");
  Rollback(text_stale_collation);

  auto text_stale_resource = BeginTextMigration(fixture, text_request);
  const auto resource_request = TextMigrationFor(
      fixture, fixture.text_resource_table_uuid,
      fixture.text_resource_column_uuid,
      VisibleTableGeneration(text_stale_resource,
                             fixture.text_resource_table_uuid));
  text_stale_resource.catalog_generation_id =
      resource_request.prior_catalog_generation;
  text_stale_resource.statement_metadata_snapshot_uuid.canonical =
      resource_request.prior_catalog_snapshot_uuid;
  ++text_stale_resource.resource_epoch;
  const auto stale_resource_before = CaptureRefusalArtifacts(
      text_stale_resource, fixture);
  const auto stale_resource = api::AppendMgaTextIdentityMigrationBatch(
      text_stale_resource, resource_request);
  Require(!stale_resource.ok &&
              stale_resource.diagnostic.code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "stale TEXT resource epoch admitted migration");
  RequireRefusalArtifactsUnchanged(
      stale_resource_before, text_stale_resource, fixture,
      "stale TEXT resource epoch changed migration artifacts");
  Rollback(text_stale_resource);

  auto text_cancelled = BeginTextMigration(fixture, text_request);
  const auto text_cancelled_before = CaptureRefusalArtifacts(
      text_cancelled, fixture);
  std::uint32_t text_cancel_checks = 0;
  text_cancelled.query_cancellation_requested = [&text_cancel_checks]() {
    ++text_cancel_checks;
    return true;
  };
  const auto text_cancelled_result = api::AppendMgaTextIdentityMigrationBatch(
      text_cancelled, text_request);
  Require(!text_cancelled_result.ok &&
              text_cancelled_result.diagnostic.code == "PROCESS.CANCELLED" &&
              text_cancel_checks == 1,
          "pre-publication text migration cancellation was not fail-closed");
  RequireRefusalArtifactsUnchanged(
      text_cancelled_before, text_cancelled, fixture,
      "cancelled TEXT migration changed migration artifacts");
  Require(text_cancel_checks == 1,
          "cancelled TEXT migration was observed more than once");
  Rollback(text_cancelled);

  auto text_rollback = BeginTextMigration(fixture, text_request);
  const auto expected_first_text_event =
      CaptureRefusalArtifacts(text_rollback, fixture).max_event_sequence + 1;
  const auto text_appended = api::AppendMgaTextIdentityMigrationBatch(
      text_rollback, text_request);
  if (!text_appended.ok) {
    std::cerr << "sealed text migration diagnostic: code="
              << text_appended.diagnostic.code
              << " key=" << text_appended.diagnostic.message_key
              << " detail=" << text_appended.diagnostic.detail << '\n';
  }
  Require(text_appended.ok && text_appended.migrated_row_count == 1 &&
              text_appended.decision_sha256.starts_with("sha256:"),
          "sealed text migration append failed");
  const auto creator_text = VisibleDescriptor(
      text_rollback, fixture, fixture.text_table_uuid);
  Require(creator_text.find(kCanonicalTextDescriptor) != std::string::npos &&
              creator_text.find(kCanonicalTextType) != std::string::npos &&
              creator_text.find(kCanonicalTextCodec) != std::string::npos &&
              creator_text.find(kLegacyText) == std::string::npos,
          "creator did not see the canonical text identity tuple");
  const auto creator_relation = api::LoadMgaRelationStorageDescriptor(
      text_rollback, fixture.text_table_uuid);
  Require(creator_relation.ok && creator_relation.descriptor.columns.size() == 1 &&
              creator_relation.descriptor.relation_generation ==
                  expected_first_text_event &&
              creator_relation.descriptor.columns.front()
                      .value_descriptor.descriptor_uuid.canonical ==
                  fixture.text_column_uuid &&
              DescriptorField(
                  creator_relation.descriptor.columns.front()
                      .value_descriptor.encoded_descriptor,
                  "datatype_descriptor_uuid") == kCanonicalTextDescriptor &&
              DescriptorField(
                  creator_relation.descriptor.columns.front()
                      .value_descriptor.encoded_descriptor,
                  "codec_uuid") == kCanonicalTextCodec,
          "creator did not see the sealed text relation descriptor");
  Rollback(text_rollback);

  auto text_after_rollback = Begin(BaseContext(fixture));
  Require(VisibleDescriptor(text_after_rollback, fixture,
                            fixture.text_table_uuid)
              .find(kLegacyText) != std::string::npos,
          "rolled-back text migration became visible");
  const auto rolled_back_relation = api::LoadMgaRelationStorageDescriptor(
      text_after_rollback, fixture.text_table_uuid);
  Require(rolled_back_relation.ok &&
              rolled_back_relation.descriptor.columns.front()
                      .value_descriptor.descriptor_uuid.canonical ==
                  fixture.text_column_uuid &&
              DescriptorField(
                  rolled_back_relation.descriptor.columns.front()
                      .value_descriptor.encoded_descriptor,
                  "datatype_descriptor_uuid") == kLegacyText,
          "rolled-back text relation descriptor became visible");
  Rollback(text_after_rollback);

  // An incomplete record has neither a seal nor a complete replacement
  // descriptor and is ignored on recovery.
  {
    std::ofstream out(fixture.path.string() + ".sb.mga_relation_metadata",
                      std::ios::app | std::ios::binary);
    out << "SBMGA1\tTEXT_IDENTITY_MIGRATION_BATCH\t999\n";
  }
  auto text_commit = BeginTextMigration(fixture, text_request);
  Require(api::AppendMgaTextIdentityMigrationBatch(
              text_commit, text_request).ok,
          "committed text migration append failed");
  Commit(text_commit);

  auto text_recovered = BeginTextMigration(fixture, text_request);
  const auto recovered_text = VisibleDescriptor(
      text_recovered, fixture, fixture.text_table_uuid);
  Require(DescriptorField(recovered_text, "datatype_descriptor_uuid") ==
                  kCanonicalTextDescriptor &&
              DescriptorField(recovered_text, "type_uuid") ==
                  kCanonicalTextType &&
              DescriptorField(recovered_text, "codec_uuid") ==
                  kCanonicalTextCodec &&
              DescriptorField(recovered_text, "codec_id") ==
                  "datatype.text.utf8.v1" &&
              DescriptorField(recovered_text, "codec_version") == "1" &&
              DescriptorField(recovered_text, "codec_generation") == "1" &&
              DescriptorField(recovered_text, "null_encoding") == "1",
          "committed text migration did not recover the exact registry tuple");
  const auto recovered_relation = api::LoadMgaRelationStorageDescriptor(
      text_recovered, fixture.text_table_uuid);
  Require(recovered_relation.ok &&
              recovered_relation.descriptor.relation_generation > 4 &&
              recovered_relation.descriptor.columns.front()
                      .value_descriptor.encoded_descriptor == recovered_text,
          "committed sealed relation descriptor did not recover exactly");
  const auto text_replay_before = CaptureRefusalArtifacts(
      text_recovered, fixture);
  const auto text_replay = api::AppendMgaTextIdentityMigrationBatch(
      text_recovered, text_request);
  Require(!text_replay.ok &&
              text_replay.diagnostic.code == "DATATYPE.DESCRIPTOR_INVALID",
          "stale text migration replay did not fail closed");
  RequireRefusalArtifactsUnchanged(
      text_replay_before, text_recovered, fixture,
      "stale TEXT replay changed migration artifacts");
  Rollback(text_recovered);

  auto resource_commit = BeginTextMigration(fixture, resource_request);
  const auto resource_appended = api::AppendMgaTextIdentityMigrationBatch(
      resource_commit, resource_request);
  Require(resource_appended.ok &&
              resource_appended.migrated_row_count == 1,
          "resource-bound TEXT migration append failed");
  const auto creator_resource_text = VisibleDescriptor(
      resource_commit, fixture, fixture.text_resource_table_uuid);
  Require(DescriptorField(creator_resource_text, "charset_uuid") ==
                  fixture.charset_uuid &&
              DescriptorField(creator_resource_text,
                              "charset_generation") ==
                  std::to_string(fixture.charset_generation) &&
              DescriptorField(creator_resource_text, "collation_uuid") ==
                  fixture.collation_uuid &&
              DescriptorField(creator_resource_text,
                              "collation_generation") ==
                  std::to_string(fixture.collation_generation) &&
              DescriptorField(creator_resource_text, "resource_epoch") ==
                  std::to_string(fixture.resource_epoch),
          "resource-bound TEXT migration did not preserve exact authority");
  Commit(resource_commit);

  auto resource_recovered = Begin(BaseContext(fixture));
  const auto recovered_resource_relation =
      api::LoadMgaRelationStorageDescriptor(
          resource_recovered, fixture.text_resource_table_uuid);
  Require(recovered_resource_relation.ok &&
              recovered_resource_relation.descriptor.columns.size() == 1 &&
              recovered_resource_relation.descriptor.columns.front()
                      .value_descriptor.encoded_descriptor ==
                  creator_resource_text,
          "resource-bound TEXT authority changed after restart");
  Rollback(resource_recovered);

  auto stale_receipt_restart_context = BaseContext(fixture);
  stale_receipt_restart_context.datatype_registry_generation = 2;
  auto stale_receipt_restart = Begin(stale_receipt_restart_context);
  const auto stale_receipt_load =
      api::LoadMgaRelationStoreState(stale_receipt_restart);
  if (stale_receipt_load.ok ||
      !ExactTextMigrationBatchInvalidDiagnostic(
          stale_receipt_load.diagnostic)) {
    std::cerr << "stale_receipt_replay_diagnostic ok="
              << (stale_receipt_load.ok ? "true" : "false")
              << " local_tx=" << stale_receipt_restart.local_transaction_id
              << " code=" << stale_receipt_load.diagnostic.code
              << " key=" << stale_receipt_load.diagnostic.message_key
              << " detail=" << stale_receipt_load.diagnostic.detail << '\n';
  }
  Require(!stale_receipt_load.ok &&
              ExactTextMigrationBatchInvalidDiagnostic(
                  stale_receipt_load.diagnostic),
          "stale datatype receipt replayed sealed TEXT migration");
  Rollback(stale_receipt_restart);

  auto zero_receipt_restart_context = BaseContext(fixture);
  zero_receipt_restart_context.datatype_catalog_snapshot_uuid.canonical.clear();
  zero_receipt_restart_context.datatype_catalog_generation = 0;
  zero_receipt_restart_context.datatype_registry_generation = 0;
  auto zero_receipt_restart = Begin(zero_receipt_restart_context);
  const auto zero_receipt_load =
      api::LoadMgaRelationStoreState(zero_receipt_restart);
  Require(!zero_receipt_load.ok &&
              ExactTextMigrationBatchInvalidDiagnostic(
                  zero_receipt_load.diagnostic),
          "zero datatype receipt replayed sealed TEXT migration");
  Rollback(zero_receipt_restart);

  auto stale_resource_restart_context = BaseContext(fixture);
  ++stale_resource_restart_context.resource_epoch;
  auto stale_resource_restart = Begin(stale_resource_restart_context);
  const auto stale_resource_load =
      api::LoadMgaRelationStoreState(stale_resource_restart);
  Require(!stale_resource_load.ok &&
              ExactInvalidRequestDiagnostic(
                  stale_resource_load.diagnostic,
                  "mga.relation_metadata:"
                  "text_migration_relation_descriptor_conflict"),
          "stale resource receipt replayed resource-bound TEXT migration");
  Rollback(stale_resource_restart);

  auto text_conflict_request = TextMigration(fixture, 5, true);
  auto text_conflict = BeginTextMigration(fixture, text_conflict_request);
  const auto text_conflict_before = CaptureRefusalArtifacts(
      text_conflict, fixture);
  const auto text_conflict_result = api::AppendMgaTextIdentityMigrationBatch(
      text_conflict, text_conflict_request);
  Require(!text_conflict_result.ok &&
              (text_conflict_result.diagnostic.code ==
                   "DATATYPE.DESCRIPTOR_INVALID" ||
               text_conflict_result.diagnostic.code ==
                   "CORE.AUTHORITY.CONFLICT"),
          "partially canonical text carrier was accepted for migration");
  RequireRefusalArtifactsUnchanged(
      text_conflict_before, text_conflict, fixture,
      "contradictory TEXT identity carrier changed migration artifacts");
  Rollback(text_conflict);
  }

  // Fresh DDL must persist the exact distinct TEXT descriptor/type/codec row;
  // a caller-supplied contradictory codec is refused before any table event.
  if (mode == FixtureMode::kTextIdentityDdl) {
  auto fresh = Begin(BaseContext(fixture));
  api::EngineCreateSchemaRequest schema;
  schema.context = fresh;
  schema.target_object.uuid.canonical = fixture.fresh_schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(PrimaryName("text_identity_fresh"));
  Require(api::EngineCreateSchema(schema).ok,
          "fresh text schema creation failed");

  api::EngineCreateTableRequest fresh_table;
  fresh_table.context = fresh;
  fresh_table.target_schema = schema.target_object;
  fresh_table.requested_table_uuid.canonical =
      fixture.fresh_text_table_uuid;
  fresh_table.table_names.push_back(PrimaryName("fresh_text_table"));
  api::EngineColumnDefinition fresh_column;
  fresh_column.ordinal = 0;
  fresh_column.names.push_back(PrimaryName("payload"));
  fresh_column.descriptor.descriptor_kind = "scalar";
  fresh_column.descriptor.canonical_type_name = "text";
  fresh_column.descriptor.encoded_descriptor = "type=text";
  fresh_column.nullable = true;
  fresh_table.table_columns.push_back(fresh_column);
  Require(api::EngineCreateTable(fresh_table).ok,
          "fresh text table creation failed");
  const auto fresh_descriptor = VisibleDescriptor(
      fresh, fixture, fixture.fresh_text_table_uuid);
  Require(DescriptorField(fresh_descriptor, "datatype_descriptor_uuid") ==
                  kCanonicalTextDescriptor &&
              DescriptorField(fresh_descriptor, "datatype_descriptor_generation") ==
                  "1" &&
              DescriptorField(fresh_descriptor, "type_uuid") ==
                  kCanonicalTextType &&
              DescriptorField(fresh_descriptor, "type_generation") == "1" &&
              DescriptorField(fresh_descriptor, "codec_uuid") ==
                  kCanonicalTextCodec &&
              DescriptorField(fresh_descriptor, "codec_id") ==
                  "datatype.text.utf8.v1" &&
              DescriptorField(fresh_descriptor, "codec_version") == "1" &&
              DescriptorField(fresh_descriptor, "codec_generation") == "1" &&
              DescriptorField(fresh_descriptor, "null_encoding") == "1" &&
              DescriptorField(fresh_descriptor, "nullable") == "true" &&
              fresh_descriptor.find(kLegacyText) == std::string::npos,
          "fresh DDL did not persist canonical text authority");

  auto fresh_resource_table = fresh_table;
  fresh_resource_table.requested_table_uuid.canonical =
      fixture.fresh_resource_text_table_uuid;
  fresh_resource_table.table_names.clear();
  fresh_resource_table.table_names.push_back(
      PrimaryName("fresh_resource_text_table"));
  fresh_resource_table.table_columns.front().descriptor.encoded_descriptor =
      "type=text;character_length=256;charset_uuid=" +
      fixture.charset_uuid + ";collation_uuid=" + fixture.collation_uuid;
  Require(api::EngineCreateTable(fresh_resource_table).ok,
          "fresh resource-bound text table creation failed");
  const auto fresh_resource_descriptor = VisibleDescriptor(
      fresh, fixture, fixture.fresh_resource_text_table_uuid);
  Require(DescriptorField(fresh_resource_descriptor, "charset_uuid") ==
                  fixture.charset_uuid &&
              DescriptorField(fresh_resource_descriptor,
                              "charset_generation") ==
                  std::to_string(fixture.charset_generation) &&
              DescriptorField(fresh_resource_descriptor, "collation_uuid") ==
                  fixture.collation_uuid &&
              DescriptorField(fresh_resource_descriptor,
                              "collation_generation") ==
                  std::to_string(fixture.collation_generation) &&
              DescriptorField(fresh_resource_descriptor, "resource_epoch") ==
                  std::to_string(fixture.resource_epoch),
          "fresh DDL did not persist exact text resource authority");

  auto rejected_table = fresh_table;
  rejected_table.requested_table_uuid.canonical =
      fixture.rejected_text_table_uuid;
  rejected_table.table_names.clear();
  rejected_table.table_names.push_back(PrimaryName("rejected_text_table"));
  rejected_table.table_columns.front().descriptor.encoded_descriptor =
      "type=text;codec_uuid=" + Id(UuidKind::object, 38);
  const auto rejected_ddl_before = CaptureRefusalArtifacts(fresh, fixture);
  const auto rejected = api::EngineCreateTable(rejected_table);
  Require(!rejected.ok,
          "fresh DDL accepted a contradictory text codec UUID");
  RequireRefusalArtifactsUnchanged(
      rejected_ddl_before, fresh, fixture,
      "contradictory fresh TEXT codec changed DDL artifacts");
  const auto fresh_state = api::LoadMgaRelationStoreState(fresh);
  Require(fresh_state.ok &&
              !api::FindVisibleCrudTable(
                   fresh_state.state.crud_metadata,
                   fixture.rejected_text_table_uuid,
                   fresh.local_transaction_id).has_value(),
          "refused fresh text DDL published a table row");

  auto semantic_rejected_table = fresh_table;
  semantic_rejected_table.requested_table_uuid.canonical =
      fixture.rejected_text_length_table_uuid;
  semantic_rejected_table.table_names.clear();
  semantic_rejected_table.table_names.push_back(
      PrimaryName("semantic_rejected_text_table"));
  semantic_rejected_table.table_columns.front().descriptor.encoded_descriptor =
      "type=text;padding=space";
  const auto semantic_ddl_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(semantic_rejected_table).ok,
          "fresh DDL accepted contradictory TEXT padding semantics");
  RequireRefusalArtifactsUnchanged(
      semantic_ddl_before, fresh, fixture,
      "contradictory fresh TEXT semantics changed DDL artifacts");

  auto type_mismatch_table = fresh_table;
  type_mismatch_table.requested_table_uuid.canonical =
      Id(UuidKind::object, 53);
  type_mismatch_table.table_names.clear();
  type_mismatch_table.table_names.push_back(
      PrimaryName("type_mismatch_rejected_text_table"));
  type_mismatch_table.table_columns.front().descriptor.encoded_descriptor =
      "type=integer";
  const auto type_mismatch_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(type_mismatch_table).ok,
          "fresh DDL accepted TEXT with contradictory type field");
  RequireRefusalArtifactsUnchanged(
      type_mismatch_before, fresh, fixture,
      "contradictory fresh TEXT type changed DDL artifacts");

  auto canonical_mismatch_table = fresh_table;
  canonical_mismatch_table.requested_table_uuid.canonical =
      Id(UuidKind::object, 54);
  canonical_mismatch_table.table_names.clear();
  canonical_mismatch_table.table_names.push_back(
      PrimaryName("canonical_mismatch_rejected_text_table"));
  canonical_mismatch_table.table_columns.front().descriptor.encoded_descriptor =
      "canonical=integer";
  const auto canonical_mismatch_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(canonical_mismatch_table).ok,
          "fresh DDL accepted TEXT with contradictory canonical field");
  RequireRefusalArtifactsUnchanged(
      canonical_mismatch_before, fresh, fixture,
      "contradictory fresh TEXT canonical field changed DDL artifacts");

  auto dual_type_table = fresh_table;
  dual_type_table.requested_table_uuid.canonical = Id(UuidKind::object, 55);
  dual_type_table.table_names.clear();
  dual_type_table.table_names.push_back(
      PrimaryName("dual_type_rejected_text_table"));
  dual_type_table.table_columns.front().descriptor.encoded_descriptor =
      "type=text;canonical=text";
  const auto dual_type_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(dual_type_table).ok,
          "fresh DDL accepted dual TEXT type authority fields");
  RequireRefusalArtifactsUnchanged(
      dual_type_before, fresh, fixture,
      "dual fresh TEXT type authority changed DDL artifacts");

  auto nullable_mismatch_table = fresh_table;
  nullable_mismatch_table.requested_table_uuid.canonical =
      Id(UuidKind::object, 56);
  nullable_mismatch_table.table_names.clear();
  nullable_mismatch_table.table_names.push_back(
      PrimaryName("nullable_mismatch_rejected_text_table"));
  nullable_mismatch_table.table_columns.front().descriptor.encoded_descriptor =
      "type=text;nullable=false";
  const auto nullable_mismatch_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(nullable_mismatch_table).ok,
          "fresh DDL accepted TEXT nullable mismatch");
  RequireRefusalArtifactsUnchanged(
      nullable_mismatch_before, fresh, fixture,
      "fresh TEXT nullable mismatch changed DDL artifacts");

  auto duplicate_semantics_table = fresh_table;
  duplicate_semantics_table.requested_table_uuid.canonical =
      Id(UuidKind::object, 48);
  duplicate_semantics_table.table_names.clear();
  duplicate_semantics_table.table_names.push_back(
      PrimaryName("duplicate_semantics_rejected_text_table"));
  duplicate_semantics_table.table_columns.front().descriptor.encoded_descriptor =
      "type=text;type=text";
  const auto duplicate_ddl_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(duplicate_semantics_table).ok,
          "fresh DDL accepted duplicate TEXT semantic fields");
  RequireRefusalArtifactsUnchanged(
      duplicate_ddl_before, fresh, fixture,
      "duplicate fresh TEXT semantics changed DDL artifacts");

  auto zero_length_table = fresh_table;
  zero_length_table.requested_table_uuid.canonical =
      fixture.rejected_text_resource_table_uuid;
  zero_length_table.table_names.clear();
  zero_length_table.table_names.push_back(
      PrimaryName("zero_length_rejected_text_table"));
  zero_length_table.table_columns.front().descriptor.encoded_descriptor =
      "type=text;character_length=0";
  const auto zero_length_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(zero_length_table).ok,
          "fresh DDL accepted explicit zero TEXT character length");
  RequireRefusalArtifactsUnchanged(
      zero_length_before, fresh, fixture,
      "zero-length fresh TEXT refusal changed DDL artifacts");

  auto stale_resource_table = fresh_resource_table;
  stale_resource_table.requested_table_uuid.canonical =
      Id(UuidKind::object, 47);
  stale_resource_table.table_names.clear();
  stale_resource_table.table_names.push_back(
      PrimaryName("stale_resource_rejected_text_table"));
  stale_resource_table.context.resource_epoch = fixture.resource_epoch + 1;
  const auto stale_ddl_resource_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(stale_resource_table).ok,
          "fresh DDL accepted stale TEXT resource epoch");
  RequireRefusalArtifactsUnchanged(
      stale_ddl_resource_before, fresh, fixture,
      "stale fresh TEXT resource refusal changed DDL artifacts");

  auto stale_charset_generation_table = fresh_resource_table;
  stale_charset_generation_table.requested_table_uuid.canonical =
      Id(UuidKind::object, 57);
  stale_charset_generation_table.table_names.clear();
  stale_charset_generation_table.table_names.push_back(
      PrimaryName("stale_charset_generation_rejected_text_table"));
  stale_charset_generation_table.table_columns.front()
      .descriptor.encoded_descriptor +=
      ";charset_generation=" +
      std::to_string(fixture.charset_generation + 1);
  const auto stale_charset_ddl_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(stale_charset_generation_table).ok,
          "fresh DDL accepted stale TEXT charset generation");
  RequireRefusalArtifactsUnchanged(
      stale_charset_ddl_before, fresh, fixture,
      "stale fresh TEXT charset generation changed DDL artifacts");

  auto stale_collation_generation_table = fresh_resource_table;
  stale_collation_generation_table.requested_table_uuid.canonical =
      Id(UuidKind::object, 58);
  stale_collation_generation_table.table_names.clear();
  stale_collation_generation_table.table_names.push_back(
      PrimaryName("stale_collation_generation_rejected_text_table"));
  stale_collation_generation_table.table_columns.front()
      .descriptor.encoded_descriptor +=
      ";collation_generation=" +
      std::to_string(fixture.collation_generation + 1);
  const auto stale_collation_ddl_before =
      CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(stale_collation_generation_table).ok,
          "fresh DDL accepted stale TEXT collation generation");
  RequireRefusalArtifactsUnchanged(
      stale_collation_ddl_before, fresh, fixture,
      "stale fresh TEXT collation generation changed DDL artifacts");

  auto wrong_receipt_table = fresh_table;
  wrong_receipt_table.requested_table_uuid.canonical =
      fixture.wrong_receipt_text_table_uuid;
  wrong_receipt_table.table_names.clear();
  wrong_receipt_table.table_names.push_back(
      PrimaryName("wrong_receipt_rejected_text_table"));
  wrong_receipt_table.context.datatype_registry_generation = 2;
  const auto wrong_ddl_receipt_before = CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(wrong_receipt_table).ok,
          "fresh DDL accepted wrong datatype receipt");
  RequireRefusalArtifactsUnchanged(
      wrong_ddl_receipt_before, fresh, fixture,
      "wrong fresh TEXT receipt changed DDL artifacts");

  // Every scalar row admitted by the datatype identity registry is published
  // from the exact live receipt as one descriptor/type/codec tuple.  BOOLEAN
  // remains the sole admitted descriptor/type UUID alias.
  api::EngineCreateTableRequest non_text_table;
  non_text_table.context = fresh;
  non_text_table.target_schema = schema.target_object;
  non_text_table.requested_table_uuid.canonical =
      fixture.fresh_non_text_table_uuid;
  non_text_table.table_names.push_back(PrimaryName("fresh_non_text_table"));
  struct ExpectedRegistryIdentity {
    std::string name;
    std::string type;
    std::string descriptor_uuid;
    std::string type_uuid;
    std::string codec_id;
    std::string null_encoding;
  };
  const std::vector<ExpectedRegistryIdentity> expected_non_text{
      {"i32", "integer", kCanonicalInt32Descriptor, kCanonicalInt32Type,
       "datatype.int32.le.v1", "1"},
      {"i64", "bigint", kCanonicalBigintDescriptor, kCanonical,
       "datatype.int64.le.v1", "2"},
      {"flag", "boolean", kCanonicalBoolean, kCanonicalBoolean,
       "datatype.boolean.u8.v1", "1"},
      {"i128", "int128", kCanonicalInt128Descriptor, kCanonicalInt128Type,
       "datatype.int128.le.v1", "1"},
      {"amount", "decimal", kCanonicalDecimalDescriptor,
       kCanonicalDecimalType, "datatype.decimal.base1e9.le.v1", "2"},
  };
  for (const auto& expected : expected_non_text) {
    api::EngineColumnDefinition column;
    column.ordinal =
        static_cast<std::uint32_t>(non_text_table.table_columns.size());
    column.names.push_back(PrimaryName(expected.name));
    column.descriptor.descriptor_kind = "scalar";
    column.descriptor.canonical_type_name = expected.type;
    column.descriptor.encoded_descriptor = "type=" + expected.type;
    column.nullable = false;
    non_text_table.table_columns.push_back(std::move(column));
  }
  Require(api::EngineCreateTable(non_text_table).ok,
          "fresh non-text identity table creation failed");
  const auto non_text_descriptors = VisibleDescriptors(
      fresh, fixture.fresh_non_text_table_uuid);
  Require(non_text_descriptors.size() == expected_non_text.size(),
          "fresh non-text identity descriptor count changed");
  for (std::size_t index = 0; index < expected_non_text.size(); ++index) {
    const auto& descriptor = non_text_descriptors[index];
    const auto& expected = expected_non_text[index];
    Require(DescriptorField(descriptor, "type") == expected.type &&
                DescriptorField(descriptor, "nullable") == "false" &&
                DescriptorField(descriptor, "datatype_descriptor_uuid") ==
                    expected.descriptor_uuid &&
                DescriptorField(descriptor,
                                "datatype_descriptor_generation") == "1" &&
                DescriptorField(descriptor, "type_uuid") ==
                    expected.type_uuid &&
                DescriptorField(descriptor, "type_generation") == "1" &&
                DescriptorField(descriptor, "codec_uuid").empty() &&
                DescriptorField(descriptor, "codec_id") ==
                    expected.codec_id &&
                DescriptorField(descriptor, "codec_version") == "1" &&
                DescriptorField(descriptor, "codec_generation") == "1" &&
                DescriptorField(descriptor, "null_encoding") ==
                    expected.null_encoding,
            "fresh non-text DDL registry tuple changed");
  }
  const auto non_text_relation = api::LoadMgaRelationStorageDescriptor(
      fresh, fixture.fresh_non_text_table_uuid);
  Require(non_text_relation.ok &&
              non_text_relation.descriptor.columns.size() ==
                  expected_non_text.size(),
          "fresh non-text MGA descriptor count changed");
  for (std::size_t index = 0; index < expected_non_text.size(); ++index) {
    const auto& persisted = non_text_relation.descriptor.columns[index];
    const bool registry_authority_preserved =
        persisted.value_descriptor.descriptor_uuid.canonical ==
            persisted.column_uuid.canonical &&
        DescriptorField(persisted.value_descriptor.encoded_descriptor,
                        "datatype_descriptor_uuid") ==
            expected_non_text[index].descriptor_uuid &&
        persisted.value_descriptor.descriptor_kind ==
            "canonical_type_descriptor" &&
        persisted.value_descriptor.encoded_descriptor ==
            non_text_descriptors[index];
    if (!registry_authority_preserved) {
      std::cerr << "non-text-descriptor[" << index << "] value_uuid="
                << persisted.value_descriptor.descriptor_uuid.canonical
                << " column_uuid=" << persisted.column_uuid.canonical
                << " kind=" << persisted.value_descriptor.descriptor_kind
                << " datatype_uuid="
                << DescriptorField(persisted.value_descriptor.encoded_descriptor,
                                   "datatype_descriptor_uuid")
                << " expected_datatype_uuid="
                << expected_non_text[index].descriptor_uuid
                << " encoded_equal="
                << (persisted.value_descriptor.encoded_descriptor ==
                    non_text_descriptors[index])
                << "\n  persisted="
                << persisted.value_descriptor.encoded_descriptor
                << "\n  visible=" << non_text_descriptors[index]
                << '\n';
    }
    Require(registry_authority_preserved,
            "fresh non-text MGA descriptor lost registry authority");
  }

  auto wrong_non_text_receipt = non_text_table;
  wrong_non_text_receipt.requested_table_uuid.canonical =
      fixture.wrong_receipt_non_text_table_uuid;
  wrong_non_text_receipt.table_names.clear();
  wrong_non_text_receipt.table_names.push_back(
      PrimaryName("wrong_receipt_rejected_non_text_table"));
  wrong_non_text_receipt.context.datatype_registry_generation = 2;
  const auto wrong_non_text_receipt_before =
      CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(wrong_non_text_receipt).ok,
          "fresh non-text DDL accepted wrong datatype receipt");
  RequireRefusalArtifactsUnchanged(
      wrong_non_text_receipt_before, fresh, fixture,
      "wrong fresh non-text receipt changed DDL artifacts");

  auto contradictory_non_text = non_text_table;
  contradictory_non_text.requested_table_uuid.canonical =
      fixture.contradictory_non_text_table_uuid;
  contradictory_non_text.table_names.clear();
  contradictory_non_text.table_names.push_back(
      PrimaryName("contradictory_rejected_non_text_table"));
  contradictory_non_text.table_columns.front().descriptor.encoded_descriptor +=
      ";type_uuid=" + std::string(kCanonicalInt32Descriptor);
  const auto contradictory_non_text_before =
      CaptureRefusalArtifacts(fresh, fixture);
  Require(!api::EngineCreateTable(contradictory_non_text).ok,
          "fresh non-text DDL accepted contradictory registry tuple");
  RequireRefusalArtifactsUnchanged(
      contradictory_non_text_before, fresh, fixture,
      "contradictory fresh non-text tuple changed DDL artifacts");
  Commit(fresh);

  auto fresh_restart = Begin(BaseContext(fixture));
  Require(VisibleDescriptor(fresh_restart, fixture,
                            fixture.fresh_text_table_uuid) ==
              fresh_descriptor,
          "fresh text descriptor changed after restart");
  const auto fresh_relation = api::LoadMgaRelationStorageDescriptor(
      fresh_restart, fixture.fresh_text_table_uuid);
  Require(fresh_relation.ok && fresh_relation.descriptor.columns.size() == 1 &&
              fresh_relation.descriptor.columns.front()
                      .value_descriptor.descriptor_uuid.canonical ==
                  fresh_relation.descriptor.columns.front()
                      .column_uuid.canonical &&
              DescriptorField(
                  fresh_relation.descriptor.columns.front()
                      .value_descriptor.encoded_descriptor,
                  "datatype_descriptor_uuid") == kCanonicalTextDescriptor &&
              fresh_relation.descriptor.columns.front()
                      .value_descriptor.encoded_descriptor == fresh_descriptor,
          "fresh relation descriptor lost canonical text authority");
  Rollback(fresh_restart);
  return EXIT_SUCCESS;
  }

  // A later ordinary table event cannot cause descriptor loading to fall back
  // to the pre-migration provisional sidecar.
  auto later_metadata = Begin(BaseContext(fixture));
  const auto later_state = api::LoadMgaRelationStoreState(later_metadata);
  Require(later_state.ok, "later metadata setup load failed");
  const auto current_text_table = api::FindVisibleCrudTable(
      later_state.state.crud_metadata, fixture.text_table_uuid,
      later_metadata.local_transaction_id);
  Require(current_text_table.has_value(),
          "canonical TEXT table missing before later metadata test");
  auto renamed_text_table = *current_text_table;
  renamed_text_table.default_name = "text_migration_target_renamed";
  Require(!api::AppendMgaTableMetadata(
               later_metadata, renamed_text_table).error,
          "later ordinary TEXT metadata append failed");
  Commit(later_metadata);

  auto later_metadata_restart = Begin(BaseContext(fixture));
  const auto conflicted_descriptor = api::LoadMgaRelationStorageDescriptor(
      later_metadata_restart, fixture.text_table_uuid);
  if (conflicted_descriptor.ok ||
      !ExactSealedRelationDescriptorSnapshotConflictDiagnostic(
          conflicted_descriptor.diagnostic)) {
    std::cerr << "later_metadata_descriptor_diagnostic ok="
              << (conflicted_descriptor.ok ? "true" : "false")
              << " error="
              << (conflicted_descriptor.diagnostic.error ? "true" : "false")
              << " code=" << conflicted_descriptor.diagnostic.code
              << " key=" << conflicted_descriptor.diagnostic.message_key
              << " detail=" << conflicted_descriptor.diagnostic.detail;
    if (conflicted_descriptor.ok &&
        !conflicted_descriptor.descriptor.columns.empty()) {
      std::cerr << " descriptor_uuid="
                << conflicted_descriptor.descriptor.columns.front()
                       .value_descriptor.descriptor_uuid.canonical;
    }
    std::cerr << '\n';
  }
  Require(!conflicted_descriptor.ok &&
              ExactSealedRelationDescriptorSnapshotConflictDiagnostic(
                  conflicted_descriptor.diagnostic),
          "later metadata resurrected the provisional TEXT sidecar");
  Rollback(later_metadata_restart);

  // A complete-looking post-commit record with a modified seal hash is
  // corruption, not a torn append, and must fail closed on restart.
  const auto metadata_path =
      std::filesystem::path(fixture.path.string() +
                            ".sb.mga_relation_metadata");
  const auto clean_metadata_size = std::filesystem::file_size(metadata_path);
  std::string sealed_text_line;
  std::string sealed_resource_text_line;
  {
    std::ifstream in(metadata_path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      const auto fields = SplitTabs(line);
      if (fields.size() == kTextMigrationSingleRowFields &&
          fields[1] == "TEXT_IDENTITY_MIGRATION_BATCH") {
        if (fields[kTextMigrationHeaderFields] == fixture.text_table_uuid) {
          sealed_text_line = line;
        } else if (fields[kTextMigrationHeaderFields] ==
                   fixture.text_resource_table_uuid) {
          sealed_resource_text_line = line;
        }
      }
    }
  }
  RequireExactTextMigrationSealedVector(
      SplitTabs(sealed_text_line), fixture, fixture.text_table_uuid,
      fixture.text_column_uuid, false);
  RequireExactTextMigrationSealedVector(
      SplitTabs(sealed_resource_text_line), fixture,
      fixture.text_resource_table_uuid, fixture.text_resource_column_uuid,
      true);
  const auto seal = sealed_text_line.find("sha256:");
  Require(seal != std::string::npos && seal + 7 < sealed_text_line.size(),
          "sealed text migration evidence was not found");
  sealed_text_line[seal + 7] =
      sealed_text_line[seal + 7] == '0' ? '1' : '0';
  {
    std::ofstream out(metadata_path, std::ios::app | std::ios::binary);
    out << sealed_text_line << '\n';
  }
  auto corrupted_context = Begin(BaseContext(fixture));
  const auto corrupted = api::LoadMgaRelationStoreState(corrupted_context);
  Require(!corrupted.ok &&
              ExactInvalidRequestDiagnostic(
                  corrupted.diagnostic,
                  "mga.relation_metadata:"
                  "text_migration_batch_hash_mismatch"),
          "corrupt text migration seal was not refused on recovery");
  Rollback(corrupted_context);

  std::filesystem::resize_file(metadata_path, clean_metadata_size);
  std::string canonical_text_line;
  {
    std::ifstream in(metadata_path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      const auto fields = SplitTabs(line);
      if (fields.size() == kTextMigrationSingleRowFields &&
          fields[1] == "TEXT_IDENTITY_MIGRATION_BATCH" &&
          fields[kTextMigrationHeaderFields] == fixture.text_table_uuid) {
        canonical_text_line = std::move(line);
      }
    }
  }
  Require(!canonical_text_line.empty(),
          "canonical TEXT seal missing after corruption rollback");
  RequireExactTextMigrationSealedVector(
      SplitTabs(canonical_text_line), fixture, fixture.text_table_uuid,
      fixture.text_column_uuid, false);

  // Recomputing the outer batch hash cannot turn a forged datatype receipt
  // into migration authority.
  auto forged_receipt_fields = SplitTabs(canonical_text_line);
  forged_receipt_fields[15] = "2";
  forged_receipt_fields[6] = RecomputeTextBatchHash(forged_receipt_fields);
  {
    std::ofstream out(metadata_path, std::ios::app | std::ios::binary);
    out << JoinTabs(forged_receipt_fields) << '\n';
  }
  auto forged_receipt_context = Begin(BaseContext(fixture));
  const auto forged_receipt =
      api::LoadMgaRelationStoreState(forged_receipt_context);
  Require(!forged_receipt.ok &&
              ExactTextMigrationBatchInvalidDiagnostic(
                  forged_receipt.diagnostic),
          "rehashed forged datatype receipt replayed TEXT migration");
  Rollback(forged_receipt_context);
  std::filesystem::resize_file(metadata_path, clean_metadata_size);

  // A fully rehashed row projection still cannot inject a replacement that
  // is not the exact transform of the visible legacy table+column lineage.
  auto forged_lineage_fields = SplitTabs(canonical_text_line);
  forged_lineage_fields[kTextMigrationHeaderFields + 13] =
      api::EncodeCrudText("forged_table_name");
  forged_lineage_fields[kTextMigrationHeaderFields + 12] =
      RecomputeTextDecisionHash(forged_lineage_fields);
  forged_lineage_fields[6] = RecomputeTextBatchHash(forged_lineage_fields);
  {
    std::ofstream out(metadata_path, std::ios::app | std::ios::binary);
    out << JoinTabs(forged_lineage_fields) << '\n';
  }
  auto forged_lineage_context = Begin(BaseContext(fixture));
  const auto forged_lineage =
      api::LoadMgaRelationStoreState(forged_lineage_context);
  Require(!forged_lineage.ok &&
              ExactInvalidRequestDiagnostic(
                  forged_lineage.diagnostic,
                  "mga.relation_metadata:"
                  "text_migration_prior_relation_transition_invalid"),
          "rehashed forged TEXT lineage was accepted");
  Rollback(forged_lineage_context);
  return EXIT_SUCCESS;
}
