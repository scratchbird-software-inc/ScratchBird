// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "scratchbird/engine/engine.h"
#include "../support/owned_temp_directory.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/native_database_create_fixture.hpp"
#include "native_database_open_discovery.hpp"
#include "database_format.hpp"
#include "memory.hpp"
#include "../../src/server_engine_bridge/diagnostic_fields.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace disk = scratchbird::storage::disk;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
std::string_view View(sb_engine_string_view_t value) {
  return {value.data ? value.data : "", static_cast<std::size_t>(value.size_bytes)};
}
void Reject(const std::string& path, sb_engine_open_mode_t mode, std::string_view detail,
            std::string_view native_code = {}) {
  sb_engine_open_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.database_path_utf8 = path.data();
  params.database_path_size = path.size();
  params.mode = mode;
  sb_engine_handle_t engine = nullptr;
  sb_engine_result_t result = nullptr;
  const auto status = sb_engine_open(&params, &engine, &result);
  if (engine) Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK,
                    "refused source returned an uncloseable handle");
  Check(result != nullptr, "source refusal has no diagnostic result");
  sb_engine_diagnostic_set_view_t diagnostics{};
  diagnostics.struct_size = sizeof(diagnostics);
  diagnostics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  Check(sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK &&
        diagnostics.diagnostic_count == 1, "source refusal diagnostic vector missing");
  const auto& diagnostic = diagnostics.diagnostics[0];
  Check(status == SB_ENGINE_STATUS_INVALID_ARGUMENT && diagnostic.numeric_code == 1005 &&
        View(diagnostic.symbolic_code) == "ENGINE.OPEN.PATH_INVALID" &&
        View(diagnostic.message_key) == "engine.open.path_invalid" &&
        View(diagnostic.safe_detail) == detail, "source refusal diagnostic changed");
  if (!native_code.empty()) {
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::EngineDiagnosticSnapshot snapshot;
    Check(bridge::CopyEngineDiagnosticSnapshot(result, 0, &snapshot) &&
          bridge::IsConsistentEngineDiagnosticSnapshot(snapshot) && snapshot.native_source &&
          snapshot.native_source->record.diagnostic_code == native_code &&
          !snapshot.native_source->record.status.ok(),
          "original typed native refusal was flattened or replaced");
  }
  Check(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK, "open diagnostic release failed");
  Check(status != SB_ENGINE_STATUS_OK && !engine, "invalid supplied database source returned open success");
}
// SEARCH_KEY: SB_OPEN_MISSING_KEEPS_ABI_CONTRACT (owner Q272 = 1, 2026-11-18).
// A file that EXISTS but is not a valid native database is refused by the
// native discovery (SB_NATIVE_DATABASE_OPEN_DISCOVERY): DATABASE.OPEN_NOT_A_
// VALID_DATABASE under engine.open.native_discovery_refused, detail the
// bootstrap/page-zero error; a missing or unopenable file keeps the old
// contract (Reject above). (Was: every legacy-shaped file refused
// ENGINE.OPEN.PATH_INVALID with a legacy header detail.)
void RejectNative(const std::string& path, sb_engine_open_mode_t mode, std::string_view detail_prefix) {
  sb_engine_open_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.database_path_utf8 = path.data();
  params.database_path_size = path.size();
  params.mode = mode;
  sb_engine_handle_t engine = nullptr;
  sb_engine_result_t result = nullptr;
  const auto status = sb_engine_open(&params, &engine, &result);
  if (engine) Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK,
                    "refused source returned an uncloseable handle");
  Check(result != nullptr, "native discovery refusal has no diagnostic result");
  sb_engine_diagnostic_set_view_t diagnostics{};
  diagnostics.struct_size = sizeof(diagnostics);
  diagnostics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  Check(sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK &&
        diagnostics.diagnostic_count == 1, "native discovery refusal diagnostic vector missing");
  const auto& diagnostic = diagnostics.diagnostics[0];
  Check(status == SB_ENGINE_STATUS_INVALID_ARGUMENT && diagnostic.numeric_code == 1005 &&
        View(diagnostic.symbolic_code) == scratchbird::storage::database::kOpenNotAValidDatabaseCode &&
        View(diagnostic.message_key) == "engine.open.native_discovery_refused" &&
        View(diagnostic.safe_detail).starts_with(detail_prefix), "native discovery refusal diagnostic changed");
  Check(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK, "open diagnostic release failed");
  Check(status != SB_ENGINE_STATUS_OK && !engine, "invalid database file returned open success");
}
void Write(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.flush();
  Check(static_cast<bool>(out), "open admission fixture write failed");
}
}
int main() try {
  namespace db = scratchbird::storage::database;
  namespace memory = scratchbird::core::memory;
  Check(memory::ConfigureDefaultMemoryManagerForFixture(memory::DefaultLocalEngineMemoryPolicy(),
      "public_open_admission").ok(), "memory fixture configuration failed");
  scratchbird::tests::OwnedTempDirectory temporary;
  const auto missing = temporary.path() / "missing.sbdb";
  const auto corrupt = temporary.path() / "corrupt.sbdb";
  const auto directory = temporary.path() / "directory";
  std::filesystem::create_directory(directory);
  disk::SerializedDatabaseHeader junk{};
  Write(corrupt, junk);
  for (const auto mode : {SB_ENGINE_OPEN_NORMAL, SB_ENGINE_OPEN_READ_ONLY,
                         SB_ENGINE_OPEN_MAINTENANCE, SB_ENGINE_OPEN_VALIDATION_ONLY}) {
    Reject(missing.string(), mode, "database_source_unavailable", "SB-STORAGE-DISK-OPEN-MISSING");
    // Q272 = 1: an existing file that is no native database is the discovery's
    // refusal (was: Reject(..., "database_header_invalid")).
    RejectNative(corrupt.string(), mode, "bootstrap:");
    Reject(directory.string(), mode, "database_source_unavailable");
    if (mode != SB_ENGINE_OPEN_VALIDATION_ONLY)
      Reject({}, mode, "database_path_required");
  }
  // Linux pathname lookup reports ENAMETOOLONG. The throwing filesystem
  // lookup used by the device must not let an exception escape the C ABI.
  Reject((temporary.path() / std::string(512, 'x')).string(),
         SB_ENGINE_OPEN_VALIDATION_ONLY, "database_source_unavailable");
  // The valid database: created natively (SB_LEGACY_CUT_CREATE_REFUSED; was
  // the legacy CreateDatabaseFile). Its legacy-shaped siblings below are
  // written by hand from the legacy header codec: files that exist and are
  // not native databases.
  scratchbird::tests::NativeFixtureCreateRequest native_create;
  native_create.path = temporary.path() / "valid.sbdb";
  native_create.database_name = "open_admission";
  const auto created = scratchbird::tests::CreateNativeFixtureDatabase(native_create);
  if (!created.ok) std::cerr << created.diagnostic_code << ':' << created.diagnostic_detail << '\n';
  Check(created.ok, "valid open fixture database creation failed");
  db::DatabaseCreateConfig create;  // the legacy header codec's inputs only (no file is created from it)
  create.path = native_create.path.string();
  create.database_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, 1791800000000).value;
  create.creation_unix_epoch_millis = 1791800000000;
  auto nul_alias = create.path;
  nul_alias.push_back('\0');
  nul_alias += "other.sbdb";
  Reject(nul_alias, SB_ENGINE_OPEN_VALIDATION_ONLY, "database_path_contains_nul");
  disk::DatabaseHeader header;
  header.database_uuid = create.database_uuid.value;
  header.creation_unix_epoch_millis = create.creation_unix_epoch_millis;
  const auto encoded = disk::SerializeDatabaseHeader(header);
  Check(encoded.ok(), "header fixture encoding failed");
  // Q272 = 1: legacy-shaped files exist and are not native databases: the
  // discovery's refusal. (Was: database_extent_invalid, database_extent_invalid,
  // database_header_read_failed under ENGINE.OPEN.PATH_INVALID.)
  const auto truncated = temporary.path() / "header_only.sbdb";
  Write(truncated, encoded.serialized);
  RejectNative(truncated.string(), SB_ENGINE_OPEN_VALIDATION_ONLY, "bootstrap:");
  std::filesystem::resize_file(truncated, header.page_size + 1);
  RejectNative(truncated.string(), SB_ENGINE_OPEN_VALIDATION_ONLY, "bootstrap:");
  std::filesystem::resize_file(truncated, 64);
  RejectNative(truncated.string(), SB_ENGINE_OPEN_VALIDATION_ONLY, "bootstrap:");
  // A checksummed UUIDv4 header is user-data-shaped, not a system identity.
  auto wrong_identity = encoded.serialized;
  // Format-v1 UUID offset32, checksum offset72 (little endian). Mutate a
  // checksummed physical input, since the legitimate encoder refuses UUIDv4.
  wrong_identity[38] = static_cast<unsigned char>((wrong_identity[38] & 0x0f) | 0x40);
  const auto checksum = disk::ComputeDatabaseHeaderChecksum(wrong_identity);
  for (std::size_t i = 0; i < 8; ++i)
    wrong_identity[72 + i] = static_cast<unsigned char>(checksum >> (8 * i));
  const auto parsed_wrong_identity = disk::ParseDatabaseHeader(wrong_identity);
  Check(!parsed_wrong_identity.ok() && parsed_wrong_identity.diagnostic.diagnostic_code ==
      "SB-STORAGE-DATABASE-UUID-NOT-V7", "non-system source lacks exact native refusal");
  Write(truncated, wrong_identity);
  // Q272 = 1 (was: Reject "database_header_invalid" with the typed source
  // SB-STORAGE-DATABASE-UUID-NOT-V7; the legacy header is never read now).
  RejectNative(truncated.string(), SB_ENGINE_OPEN_VALIDATION_ONLY, "bootstrap:");
  sb_engine_open_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.database_path_utf8 = create.path.data();
  params.database_path_size = create.path.size();
  sb_engine_handle_t engine = nullptr;
  // A valid native database: discovered, then refused with the temporary
  // DATABASE.OPEN_NATIVE_RUNTIME_PENDING (SB_LEGACY_CUT_NATIVE_OPEN; native
  // serving is S3b-S3f), in every mode, with no handle. (Was: opened
  // successfully in every mode, a legacy database.)
  for (const auto mode : {SB_ENGINE_OPEN_NORMAL, SB_ENGINE_OPEN_READ_ONLY,
                         SB_ENGINE_OPEN_MAINTENANCE, SB_ENGINE_OPEN_VALIDATION_ONLY}) {
    params.mode = mode;
    engine = nullptr;
    sb_engine_result_t result = nullptr;
    const auto status = sb_engine_open(&params, &engine, &result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && !engine && result,
          "valid native database: NATIVE_RUNTIME_PENDING until S3b-S3f (was: open success)");
    sb_engine_diagnostic_set_view_t diagnostics{};
    diagnostics.struct_size = sizeof(diagnostics);
    diagnostics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    Check(sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK &&
          diagnostics.diagnostic_count == 1 &&
          View(diagnostics.diagnostics[0].symbolic_code) == scratchbird::storage::database::kOpenNativeRuntimePendingCode &&
          View(diagnostics.diagnostics[0].message_key) == "engine.open.native_runtime_pending",
          "valid native database: the pending refusal names its code");
    Check(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK, "pending diagnostic release failed");
  }
  // Existing ABI/framing fixtures deliberately use a headless validation-only
  // handle. It is not an opened node and must not enable the other open modes.
  params.database_path_utf8 = nullptr;
  params.database_path_size = 0;
  engine = nullptr;
  Check(sb_engine_open(&params, &engine, nullptr) == SB_ENGINE_STATUS_OK && engine,
        "headless structural validation compatibility lost");
  Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK, "headless handle close failed");
  temporary.Cleanup();
  std::cout << "public_open_admission=passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
