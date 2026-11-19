// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_LEGACY_CUT_TEST (owner 2026-11-16: "The moment you can create
// a valid database, the legacy support is dropped. All reads/writes/persistance
// etc must be to the valid database, and no other files should be created.";
// plan 38 step S3a; Q253, Q258 = 1).
//
// Every production create and open entry point, with its specific refusal:
//  create  EngineCreateLifecycle legacy format     DATABASE.CREATE_LEGACY_FORMAT_REFUSED
//          SBLR v1 / v2-legacy / bare op (both op names, no transaction)
//                                                  DATABASE.CREATE_LEGACY_FORMAT_REFUSED + detail
//  open    StartHostedEngine, sb_engine_open, the 14 lifecycle operations:
//          a legacy, junk, short or empty file     DATABASE.OPEN_NOT_A_VALID_DATABASE (bootstrap:/page_zero:)
//          a valid native file                     DATABASE.OPEN_NATIVE_RUNTIME_PENDING (open, attach, ABI)
//                                                  DATABASE.LIFECYCLE_OPERATION_UNAVAILABLE (the others)
//                                                  StartHostedEngine (S3b): serves it when a process crypto
//                                                  owner is supplied (native_serving_test); without one,
//                                                  DATABASE.OPEN_NATIVE_CRYPTO_OWNER_REQUIRED
//                                                  (Was: DATABASE.OPEN_NATIVE_RUNTIME_PENDING for the server)
//          a decryption key option                 DATABASE.OPEN_DECRYPTION_UNAVAILABLE
//          a prelocked server handoff              ARCH.DATABASE_OWNERSHIP_LEGACY_REFUSED
// Each refusal leaves its directory exactly as it was (names, sizes, SHA-256),
// creates no file, and a native file is owned again afterwards (ownership was
// released). S3a: ServedRouteOwnerFor resolves a NATIVE owner by its data-file
// identity, through the file's path and a hard link, never another file.
// The strace half (0 ".sb." syscalls, 0 lock files) is
// native_legacy_cut_strace.py, which runs this binary's single-step modes.

#include "lifecycle/engine_lifecycle_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "hash_digest.hpp"
#include "database_lifecycle.hpp"
#include "native_database_open_discovery.hpp"
#include "catalog_language.hpp"
#include "server/engine_host.hpp"
#include "server/database_ownership.hpp"
#include "scratchbird/engine/engine.h"
#include "uuid.hpp"
#include "memory.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/native_database_create_fixture.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace db = scratchbird::storage::database;
namespace srv = scratchbird::server;
namespace fs = std::filesystem;

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::exit(1);
  }
}

std::array<std::uint8_t, 16> Fixed(std::uint8_t n) {
  std::array<std::uint8_t, 16> id{};
  id[0] = 1; id[6] = 0x70; id[8] = 0x80; id[15] = n;
  return id;
}

// Names, sizes and SHA-256 of every entry: the "untouched" observation.
std::map<std::string, std::string> Snapshot(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    std::string value = "dir";
    if (entry.is_regular_file()) {
      std::ifstream in(entry.path(), std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), {});
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          reinterpret_cast<const p::byte*>(bytes.data()), bytes.size());
      Check(digest.ok(), "sha256");
      value = std::to_string(bytes.size()) + ":" + scratchbird::core::hash::HexLower(digest.digest);
    }
    out[entry.path().filename().string()] = value;
  }
  return out;
}

bool HasCode(const api::EngineApiResult& result, std::string_view code, std::string_view detail_part = {}) {
  for (const auto& diagnostic : result.diagnostics)
    if (diagnostic.code == code && diagnostic.detail.find(detail_part) != std::string::npos) return true;
  return false;
}

std::string Codes(const api::EngineApiResult& result) {
  std::string out;
  for (const auto& diagnostic : result.diagnostics) out += diagnostic.code + "[" + diagnostic.detail + "] ";
  return out;
}

api::EngineRequestContext Context(const fs::path& path) {
  api::EngineRequestContext context;
  context.request_id = "legacy-cut";
  context.database_path = path.string();
  context.security_context_present = true;
  context.statement_uuid = api::EngineUuid{Fixed(0x31)};
  context.server_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  return context;
}

void ConfigureMemory() {
  const auto configured = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "native_legacy_cut_test");
  Check(configured.ok(), "memory configuration");
}

bool CreateNative(const fs::path& path) {
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.database_name = "legacy_cut_native";
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
  return made.ok;
}

// A real legacy (SBDBV001) file, written through the storage create that no
// production entry point reaches any more: the file the cut must refuse.
bool CreateLegacy(const fs::path& path) {
  const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto database = scratchbird::core::uuid::GenerateEngineIdentityV7(p::UuidKind::database, now);
  const auto filespace = scratchbird::core::uuid::GenerateEngineIdentityV7(p::UuidKind::filespace, now + 1);
  if (!database.ok() || !filespace.ok()) return false;
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database.value;
  create.filespace_uuid = filespace.value;
  create.creation_unix_epoch_millis = now;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = false;
  const auto made = db::CreateDatabaseFile(create);
  if (!made.ok()) std::cerr << made.diagnostic.diagnostic_code << '\n';
  return made.ok();
}

// One StartHostedEngine; the first diagnostic code (or "OK").
std::string HostedOpen(const fs::path& path, bool prelocked = false, bool embedded = false) {
  srv::ServerBootstrapConfig config;
  config.database_default_path = path;
  config.database_ownership_prelocked = prelocked;
  if (embedded) {
    config.database_ownership_owner_kind = "embedded";
    config.embedded_direct_mode = true;
    config.sbps_enabled = false;
  }
  const auto started = srv::StartHostedEngine(config);
  if (started.ok()) return "OK";
  std::string detail;
  for (const auto& [key, value] : started.diagnostics.front().fields)
    if (key == "detail") detail = value;
  return started.diagnostics.front().code + "|" + detail;
}

std::string AbiOpen(const fs::path& path, sb_engine_status_t* status_out) {
  const auto text = path.string();
  sb_engine_open_params_v1_t open{};
  open.struct_size = sizeof(open);
  open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open.database_path_utf8 = text.data();
  open.database_path_size = text.size();
  open.mode = SB_ENGINE_OPEN_NORMAL;
  sb_engine_handle_t engine = nullptr;
  sb_engine_result_t result = nullptr;
  *status_out = sb_engine_open(&open, &engine, &result);
  if (engine) (void)sb_engine_close(engine, nullptr);
  std::string code;
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (result && sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK &&
      diagnostics.diagnostic_count > 0)
    code.assign(diagnostics.diagnostics[0].symbolic_code.data, diagnostics.diagnostics[0].symbolic_code.size_bytes);
  if (result) (void)sb_engine_result_release(result);
  return code;
}

template <class TRequest, class TResult>
TResult RunLifecycle(TResult (*operation)(const TRequest&), const fs::path& path,
                     std::vector<std::string> options = {}) {
  TRequest request;
  request.context = Context(path);
  request.option_envelopes = std::move(options);
  return operation(request);
}

// All 14 existing-database lifecycle operations; true = the open family.
struct LifecycleCase {
  const char* name;
  bool open_family;
  api::EngineApiResult (*run)(const fs::path&, std::vector<std::string>);
};
#define SB_LC(name, open, Op) \
  LifecycleCase{name, open, [](const fs::path& path, std::vector<std::string> options) -> api::EngineApiResult { \
    return RunLifecycle<api::Op##Request, api::Op##Result>(&api::Op, path, std::move(options)); }}
const std::array<LifecycleCase, 14> kLifecycle{{
    SB_LC("open", true, EngineOpenLifecycle),
    SB_LC("attach", true, EngineAttachLifecycle),
    SB_LC("detach", false, EngineDetachLifecycle),
    SB_LC("shutdown", false, EngineShutdownLifecycle),
    SB_LC("enter_maintenance", false, EngineEnterMaintenanceLifecycle),
    SB_LC("exit_maintenance", false, EngineExitMaintenanceLifecycle),
    SB_LC("enter_restricted_open", false, EngineEnterRestrictedOpenLifecycle),
    SB_LC("exit_restricted_open", false, EngineExitRestrictedOpenLifecycle),
    SB_LC("inspect", false, EngineInspectLifecycle),
    SB_LC("verify", false, EngineVerifyLifecycle),
    SB_LC("repair", false, EngineRepairLifecycle),
    SB_LC("shutdown_force", false, EngineForceShutdownLifecycle),
    SB_LC("shutdown_acknowledge", false, EngineAcknowledgeShutdownLifecycle),
    SB_LC("drop", false, EngineDropLifecycle),
}};
#undef SB_LC

void ExpectNativeOwnable(const fs::path& path, const std::string& what) {
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only,
                    d::DatabaseFileOwnership::native_data_file).ok(),
        what + ": the file is natively ownable again (ownership was released)");
  Check(device.Close().ok(), "close");
}

// ---- create ----------------------------------------------------------------

template <class Descriptor>
void FillCommon(Descriptor& descriptor) {
  descriptor.operation_uuid = Fixed(0x50);
  descriptor.statement_receipt_uuid = Fixed(0x51);
  descriptor.owner_uuid = Fixed(0x54);
  descriptor.catalog_snapshot_uuid = Fixed(0x55);
  descriptor.catalog_generation = 71;
  descriptor.security_context_uuid = Fixed(0x56);
  descriptor.policy_snapshot_uuid = Fixed(0x57);
  descriptor.policy_generation = 72;
  descriptor.transaction_uuid = Fixed(0x58);
  descriptor.transaction_generation = 73;
  descriptor.durability_profile_uuid = Fixed(0x59);
  descriptor.resource_budget_uuid = Fixed(0x5a);
  descriptor.resource_budget_generation = 74;
  descriptor.executor_availability_generation = 1;
  descriptor.descriptor_evidence_uuid = Fixed(0x5b);
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(descriptor.option_vector);
  Check(hash.ok(), "option hash");
  descriptor.option_vector_sha256 = hash.digest;
}

sblr::SblrDispatchResult DispatchCreate(const fs::path& path, const std::string& operation_id,
                                        std::optional<sblr::SblrOperand> operand) {
  const auto* entry = sblr::LookupSblrOpcode("SBLR_LIFECYCLE_CREATE_DATABASE");
  Check(entry != nullptr, "the create opcode is registered");
  auto envelope = sblr::MakeSblrEnvelope(operation_id, "SBLR_LIFECYCLE_CREATE_DATABASE", "trace.legacy_cut");
  envelope.opcode_code = entry->code;
  envelope.parser_package_uuid = api::EngineUuid{Fixed(0x41)};
  envelope.registry_snapshot_uuid = api::EngineUuid{Fixed(0x42)};
  envelope.requires_security_context = entry->requires_security_context;
  envelope.requires_transaction_context = entry->requires_transaction_context;
  envelope.requires_cluster_authority = entry->requires_cluster_authority;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  if (operand) envelope.operands.push_back(std::move(*operand));
  api::EngineApiRequest options;
  options.option_envelopes = {"page_size:16384", "total_pages:512", "allow_minimal_resource_bootstrap:true"};
  // No transaction context: the refusal must be the format's own code.
  return sblr::DispatchSblrOperation({Context(path), envelope, options});
}

void CreateRefusals(const fs::path& root) {
  const auto dir = root / "create";
  fs::create_directory(dir);
  const auto path = dir / "refused.sdb";
  // EngineCreateLifecycle, legacy format (the enum default), with and without
  // the management transaction the legacy create used to require.
  for (const bool with_transaction : {false, true}) {
    api::EngineCreateLifecycleRequest request;
    request.context = Context(path);
    request.database_name = "legacy_named";
    request.database_language = c::kCatalogLanguage_en;
    request.option_envelopes = {"allow_minimal_resource_bootstrap:true", "page_size:16384"};
    if (with_transaction) {
      request.context.transaction_uuid = api::EngineUuid{Fixed(0x60)};
      request.context.local_transaction_id = 1;
    }
    Check(request.database_format == api::EngineDatabaseFormat::legacy, "legacy is still the enum default");
    const auto refused = api::EngineCreateLifecycle(request);
    Check(!refused.ok && HasCode(refused, db::kCreateLegacyFormatRefusedCode, "legacy_database_format_refused"),
          "EngineCreateLifecycle refuses the legacy format: " + Codes(refused));
    Check(fs::is_empty(dir), "a refused legacy create creates no file");
  }
  // SBLR: v1, v2 naming the legacy format, and no descriptor, under both names.
  sblr::SblrLifecycleCreateDatabaseDescriptorV1 v1;
  v1.database_uuid = Fixed(0x52);
  v1.database_name_uuid = Fixed(0x53);
  FillCommon(v1);
  sblr::SblrLifecycleCreateDatabaseDescriptorV2 v2;
  FillCommon(v2);
  v2.database_language_uuid = c::kCatalogLanguage_en.bytes;
  v2.database_name = "legacy_named";
  v2.database_format = sblr::SblrLifecycleCreateDatabaseFormat::legacy;
  const auto operand = [](std::vector<std::uint8_t> body, const char* type) {
    sblr::SblrOperand o;
    o.type = type;
    o.name = "database";
    o.ordinal = 1;
    o.value_kind = sblr::SblrValueKind::lifecycle_create_database_descriptor;
    o.value_body = std::move(body);
    return o;
  };
  const std::array<std::pair<std::optional<sblr::SblrOperand>, const char*>, 3> shapes{{
      {operand(sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV1(v1), "lifecycle_create_database_descriptor.v1"),
       "create_descriptor_v1_refused"},
      {operand(sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(v2), "lifecycle_create_database_descriptor.v2"),
       "create_descriptor_v2_legacy_format_refused"},
      {std::nullopt, "create_descriptor_absent_refused"},
  }};
  // The one registered create operation id. ("lifecycle.create_database" is a
  // dispatch-table alias no envelope can carry: the validator refuses it as
  // operation_id_unknown, so the alias branch is reachable only by direct typed
  // callers and is covered by the EngineCreateLifecycle checks above.)
  {
    const char* op = "engine.op.lifecycle_create_database";
    for (const auto& [shape, detail] : shapes) {
      const auto refused = DispatchCreate(path, op, shape);
      // A bare operation never reaches dispatch: the envelope validator
      // (sblr_engine_envelope.cpp, "exactly one canonical SCDD v1 or v2
      // operand") refuses it first with its own specific code, no engine call,
      // no file. The v1 and v2-legacy shapes pass validation and are refused
      // by dispatch with the cut's code and their own detail.
      const bool expected = !shape
          ? HasCode(refused.api_result, "SB_SBLR_DISPATCH_ENVELOPE_REJECTED", "SCDD v1 or v2 operand")
          : HasCode(refused.api_result, db::kCreateLegacyFormatRefusedCode, detail);
      Check(!refused.api_result.ok && !refused.dispatched_to_api && expected,
            std::string("SBLR ") + op + " " + detail + ": " + Codes(refused.api_result));
      Check(fs::is_empty(dir), std::string("SBLR ") + detail + " creates no file");
    }
  }
  std::printf("create refusals: EngineCreateLifecycle x2, SBLR v1 / v2-legacy / bare; no file\n");
}

// ---- open ------------------------------------------------------------------

void OpenRefusals(const fs::path& root) {
  // The native file, alone in its directory.
  const auto native_dir = root / "native";
  fs::create_directory(native_dir);
  const auto native = native_dir / "native.sdb";
  Check(CreateNative(native), "native create");
  // A legacy file (with its legacy side files), a junk file, a short file, an
  // empty file and a missing path, each where the observation can see them.
  const auto legacy_dir = root / "legacy";
  fs::create_directory(legacy_dir);
  const auto legacy = legacy_dir / "legacy.sdb";
  Check(CreateLegacy(legacy), "legacy fixture file");
  const auto other_dir = root / "other";
  fs::create_directory(other_dir);
  const auto junk = other_dir / "junk.sdb";
  const auto short_file = other_dir / "short.sdb";
  const auto empty = other_dir / "empty.sdb";
  const auto missing = other_dir / "missing.sdb";
  { std::ofstream(junk, std::ios::binary) << std::string(16384, 'x'); }
  { std::ofstream(short_file, std::ios::binary) << std::string(100, '\0'); }
  { std::ofstream(empty, std::ios::binary); }

  const auto native_before = Snapshot(native_dir);
  const auto legacy_before = Snapshot(legacy_dir);
  const auto other_before = Snapshot(other_dir);
  Check(native_before.size() == 1, "native create made exactly one file");

  // Discovery itself.
  {
    const auto ok = db::DiscoverNativeDatabaseAtPath(native.string());
    Check(ok.ok() && ok.page_zero->bootstrap.page_size_bytes == 16384, "discovery reads the native first block");
    const auto refused = db::DiscoverNativeDatabaseAtPath(legacy.string());
    Check(!refused.ok() && refused.code == db::kOpenNotAValidDatabaseCode &&
              refused.detail.starts_with("bootstrap:"),
          "discovery refuses the legacy file at its first block: " + refused.code + " " + refused.detail);
    const auto keyed = db::DiscoverNativeDatabaseAtPath(native.string(), true);
    Check(!keyed.ok() && keyed.code == db::kOpenDecryptionUnavailableCode, "a key refuses before any open");
  }

  // Lifecycle operations.
  for (const auto& op : kLifecycle) {
    const auto on_native = op.run(native, {});
    const auto expected = op.open_family ? db::kOpenNativeRuntimePendingCode : db::kLifecycleOperationUnavailableCode;
    Check(!on_native.ok && HasCode(on_native, expected),
          std::string(op.name) + " on a native file: " + Codes(on_native));
    const auto keyed = op.run(native, {"decryption_available:true"});
    Check(!keyed.ok && HasCode(keyed, db::kOpenDecryptionUnavailableCode),
          std::string(op.name) + " with a key: " + Codes(keyed));
    for (const auto& invalid : {legacy, junk, short_file, empty}) {
      const auto refused = op.run(invalid, {});
      Check(!refused.ok && HasCode(refused, db::kOpenNotAValidDatabaseCode),
            std::string(op.name) + " on " + invalid.filename().string() + ": " + Codes(refused));
    }
    const auto absent = op.run(missing, {});
    Check(!absent.ok && HasCode(absent, "SB-STORAGE-DISK-OPEN-MISSING"),
          std::string(op.name) + " on a missing path: " + Codes(absent));
  }
  std::printf("lifecycle: 14 operations x (native, key, legacy, junk, short, empty, missing)\n");

  // The server and embedded hosted open.
  {
    // S3b (thin-slice, plan 38): a valid native file now goes on to the
    // database loop, which needs the server process's crypto owner. A caller
    // that supplies none (this harness; the embedded entry) is refused with
    // its own code; serving with a crypto owner is native_serving_test.
    // (Was: DATABASE.OPEN_NATIVE_RUNTIME_PENDING for both; that code now stays
    // for the lifecycle operations and the engine-handle open, S3e-S3f.)
    const auto no_crypto = HostedOpen(native);
    Check(no_crypto.starts_with("DATABASE.OPEN_NATIVE_CRYPTO_OWNER_REQUIRED|no_process_crypto_owner_supplied"),
          "StartHostedEngine on a native file, no crypto owner: " + no_crypto);
    const auto embedded = HostedOpen(native, false, true);
    Check(embedded.starts_with("DATABASE.OPEN_NATIVE_CRYPTO_OWNER_REQUIRED|embedded_native_open_not_built"),
          "StartHostedEngine (embedded) on a native file: " + embedded);
    const auto refused = HostedOpen(legacy);
    Check(refused.starts_with(std::string(db::kOpenNotAValidDatabaseCode) + "|bootstrap:"),
          "StartHostedEngine on a legacy file: " + refused);
    const auto junk_refused = HostedOpen(junk);
    Check(junk_refused.starts_with(std::string(db::kOpenNotAValidDatabaseCode) + "|"),
          "StartHostedEngine on junk: " + junk_refused);
    const auto absent = HostedOpen(missing);
    Check(absent.starts_with("SERVER.ENGINE_HOST.DATABASE_NOT_FOUND|"), "StartHostedEngine on a missing path: " + absent);
    const auto prelocked = HostedOpen(native, true);
    Check(prelocked.starts_with("ARCH.DATABASE_OWNERSHIP_LEGACY_REFUSED|"), "prelocked handoff: " + prelocked);
    // A second owner is refused before discovery.
    auto held = srv::AcquireDatabaseOwnership({native, "server", {}, d::RouteStorageAccess::read_only,
                                               d::DatabaseFileOwnership::native_data_file});
    Check(held.acquired, "test holds native ownership");
    const auto owned = HostedOpen(native);
    Check(owned.starts_with("ARCH.DATABASE_MULTI_OWNER|"), "StartHostedEngine on an owned file: " + owned);
    held.lock->release();
  }
  std::printf("hosted open: native, embedded, legacy, junk, missing, prelocked, owned\n");

  // The engine ABI open.
  {
    sb_engine_status_t status{};
    const auto pending = AbiOpen(native, &status);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && pending == db::kOpenNativeRuntimePendingCode,
          "sb_engine_open on a native file: " + pending);
    const auto refused = AbiOpen(legacy, &status);
    Check(status == SB_ENGINE_STATUS_INVALID_ARGUMENT && refused == db::kOpenNotAValidDatabaseCode,
          "sb_engine_open on a legacy file: " + refused);
  }
  std::printf("sb_engine_open: native, legacy\n");

  Check(Snapshot(native_dir) == native_before, "the native directory is untouched (no file made, file unchanged)");
  Check(Snapshot(legacy_dir) == legacy_before, "the legacy file and its side files are untouched");
  Check(Snapshot(other_dir) == other_before, "junk/short/empty untouched; the missing path still missing");
  ExpectNativeOwnable(native, "after every refusal");
}

// ---- S3a: served owner by lease identity -------------------------------------

void ServedOwnerByLeaseIdentity(const fs::path& root) {
  const auto dir = root / "served";
  fs::create_directory(dir);
  const auto native = dir / "served.sdb";
  Check(CreateNative(native), "native create (served)");
  const auto link = dir / "served-link.sdb";
  fs::create_hard_link(native, link);
  const auto other = dir / "other.sdb";
  { std::ofstream(other, std::ios::binary) << "x"; }
  auto owned = srv::AcquireDatabaseOwnership({native, "server", {}, d::RouteStorageAccess::read_write,
                                              d::DatabaseFileOwnership::native_data_file});
  Check(owned.acquired && owned.lock && owned.lock->lock_path().empty(), "a native owner (no lock path)");
  srv::HostedEngineState state;
  state.database_ownership_locks.push_back(owned.lock);
  const auto lease = owned.lock->ServedStorageLease();
  Check(lease != nullptr, "the native owner publishes its lease");
  Check(srv::ServedRouteOwnerFor(state, native.string()) == lease, "the native owner resolves by its path");
  Check(srv::ServedRouteOwnerFor(state, link.string()) == lease,
        "the native owner resolves through a hard link (data-file identity, not path text)");
  Check(srv::ServedRouteOwnerFor(state, other.string()) == nullptr, "another file resolves no owner");
  Check(srv::ServedRouteOwnerFor(state, (dir / "absent.sdb").string()) == nullptr, "a missing path resolves no owner");
  {
    d::FileDevice device;
    Check(device.OpenServed(srv::ServedRouteOwnerFor(state, native.string()), native.string(),
                            d::FileOpenMode::open_existing_read_only).ok(),
          "the resolved native owner serves its own device");
    const auto discovered = db::DiscoverNativeDatabaseOnOpenDevice(device);
    Check(discovered.ok(), "page zero through the served device");
    Check(device.Close().ok(), "close served");
  }
  state.database_ownership_locks.clear();
  owned.lock->release();
  Check(srv::ServedRouteOwnerFor(state, native.string()) == nullptr, "released: no owner");
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) names.push_back(entry.path().filename().string());
  Check(names.size() == 3, "native ownership made no lock file");
  std::printf("served owner: path, hard link, other, absent, served device\n");
}

int Mode(int argc, char** argv) {
  const std::string mode = argv[1];
  if (argc != 3) return 2;
  const fs::path path = argv[2];
  if (mode == "create-native") return CreateNative(path) ? 0 : 1;
  if (mode == "create-legacy") return CreateLegacy(path) ? 0 : 1;
  if (mode == "hosted-open") { std::cout << HostedOpen(path) << '\n'; return 0; }
  if (mode == "embedded-open") { std::cout << HostedOpen(path, false, true) << '\n'; return 0; }
  if (mode == "abi-open") { sb_engine_status_t s{}; std::cout << AbiOpen(path, &s) << '\n'; return 0; }
  if (mode == "lifecycle-open") {
    const auto r = kLifecycle[0].run(path, {});
    std::cout << (r.diagnostics.empty() ? std::string("OK") : r.diagnostics.front().code) << '\n';
    return 0;
  }
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  ConfigureMemory();
  if (argc > 1) return Mode(argc, argv);
  scratchbird::tests::OwnedTempDirectory artifacts;
  CreateRefusals(artifacts.path());
  OpenRefusals(artifacts.path());
  ServedOwnerByLeaseIdentity(artifacts.path());
  artifacts.Cleanup();
  std::printf("native_legacy_cut: %d checks passed\n", checks);
  return 0;
}
