// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_FAMILY_GUARDS_TEST (2026-12-04; owner: the database is ONE file, "no other files should be created", legacy
// ".sb." side files are drift). The receipt gate (native_receipt_gate) refuses the bridge consumers on a native receipt; the ALLOWED
// CopyStatementContextEngineContextV1 still hands out a native-marked EngineRequestContext that the server passes to INTERNAL APIs,
// and the engine's ".sb." families (about 60 files) used to build "<database>.sb...." paths with no native guard. Each family now
// refuses a native context at its ENTRY with ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED and a registry_family field (see
// native_legacy_registry_refusal.hpp, NativeFamilyRefusalAs). This test calls EVERY guarded entry point (the table is generated from the
// guarded functions' own signatures: tools/gen_calls.py; default-constructed arguments, because the guard runs BEFORE any validation)
// on a production-created native database (the real create path, the real HandleAuthHandoff session and transaction), with TWO native
// contexts: (A) the copy of an approved session's receipt context (the context the server copies at its 8 call sites) and (B) one
// built the way prepared_statement_registry_context (engine/public_abi.cpp) builds it from a session. It asserts, per entry that can
// carry a diagnostic, the exact code and the registry_family field; per family, that the data file is byte-identical and the directory
// holds only the data file; at the end that no file named ".sb." exists anywhere under the test's temp tree and Next is unchanged. A
// LEGACY context (no native facts, a path in the test's own other temp directory) is called through the same table and is never given
// the native refusal: the guard changes nothing for a legacy context.
// Modes (a child process each):
//   serve <path> <state> <database_hex> <bootstrap_hex> <service_hex>   states: fguard trace
//        prints "FGUARD_OK checks=N"; `trace` marks regions on stderr for native_family_guards_strace.py
// With no argument the driver runs everything; "size <bytes>" one page size.

#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/native_login_material.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "engine/internal_api/sblr_name_resolve_journal.hpp"
#include "security/authentication_api.hpp"
#include "catalog_language.hpp"
#include "catalog_records.hpp"
#include "catalog_authorization_policy.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_native_security_record.hpp"
#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "hash_digest.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "native_catalog_version_writer.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "filespace_page_zero.hpp"
#include "uuid.hpp"
#include "scratchbird/engine/engine.h"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/login_throttle.hpp"
#include "server/native_database_loop.hpp"
#include "server/process_crypto_owner.hpp"
#include "engine/sblr/sblr_session_setting_set_runtime.hpp"
#include "server_engine_bridge/diagnostic_fields.hpp"
#include "server_engine_bridge/prepared_metadata_binding.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "session_registry.hpp"
#include "ipc_server.hpp"
#include "wire/parser_server_ipc/sbps_statement_management_bind_codec.hpp"
#include "../support/catalog_change_event_fixture.hpp"
#include "../support/native_offline_file_reader.hpp"
#include "../support/owned_temp_directory.hpp"

#include "backup_archive/backup_archive_api.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "domain_support/domain_store.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "lifecycle/sequence_generator_lifecycle.hpp"
#include "management/memory_management_api.hpp"
#include "mga_relation_store/mga_bulk_import_publication.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_update_durable_store.hpp"
#include "nosql/document_api.hpp"
#include "nosql/document_path_physical_provider.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "notification/notification_api.hpp"
#include "query/optimizer_plan_lifecycle.hpp"
#include "sblr_access_cursor_open_coordinator.hpp"
#include "sblr_autonomous_frame_coordinator.hpp"
#include "sblr_catalog_epoch_check_journal.hpp"
#include "sblr_cursor_open_coordinator.hpp"
#include "sblr_database_attach_journal.hpp"
#include "sblr_error_vector_descriptor_registry.hpp"
#include "sblr_optimizer_stats_drop_journal.hpp"
#include "sblr_parameter_set_registry.hpp"
#include "sblr_parse_text_journal.hpp"
#include "sblr_prepared_coordination_registry.hpp"
#include "sblr_read_by_key_coordinator.hpp"
#include "sblr_read_range_coordinator.hpp"
#include "sblr_read_stream_coordinator.hpp"
#include "sblr_reservation_release_coordinator.hpp"
#include "sblr_result_set_pass_coordinator.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "sblr_source_map_descriptor_registry.hpp"
#include "sblr_temporary_instance_cleanup_coordinator.hpp"
#include "sblr_variable_descriptor_registry.hpp"
#include "sblr_variable_frame_coordinator.hpp"
#include "security/identity_api.hpp"
#include "security/protected_material_api.hpp"
#include "transaction/local_commit_publication.hpp"
#include "catalog/catalog_table_storage_creation.hpp"
#include "catalog/catalog_view_storage_creation.hpp"
#include "ddl/comment_api.hpp"
#include "dml/delete_binding_authority.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"
#include "mga_relation_store/mga_delete_durable_store.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace db = scratchbird::storage::database;
namespace pgs = scratchbird::storage::page;
namespace srv = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace bind = scratchbird::wire::sbps_statement_management;
namespace mem = scratchbird::core::memory;
namespace cat = scratchbird::core::catalog;
namespace off = scratchbird::tests::offline;
namespace fs = std::filesystem;

constexpr std::string_view kBootstrapPassword = "S4a-Bootstrap-Pw-7f3a91c2d8";
constexpr std::string_view kBootstrapName = "fixture_owner";

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::exit(1);
  }
}

std::string Hex(const p::Uuid& id) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : id.bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 15]);
  }
  return out;
}
p::Uuid FromHex(std::string_view hex) {
  p::Uuid id;
  Check(hex.size() == 32, "a 32-digit hex identity");
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<p::byte>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  return id;
}
p::Uuid NewId() {
  const auto id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(id.has_value(), "an identity");
  return *id;
}
void Mark(const std::string& name) {
  const std::string line = "MARK " + name + "\n";
  (void)!::write(2, line.data(), line.size());
}
std::vector<p::byte> Image(const fs::path& path) { return off::ReadAll(path); }

std::string_view View(sb_engine_string_view_t value) {
  return {value.data ? value.data : "", static_cast<std::size_t>(value.size_bytes)};
}
// Releases the result and returns the first diagnostic's symbolic code ("" when none).
std::string Code(sb_engine_result_t& result) {
  if (result == nullptr) return {};
  sb_engine_diagnostic_set_view_t diagnostics{};
  diagnostics.struct_size = sizeof(diagnostics);
  diagnostics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::string code;
  if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK && diagnostics.diagnostic_count != 0)
    code = std::string(View(diagnostics.diagnostics[0].symbolic_code));
  (void)sb_engine_result_release(result);
  result = nullptr;
  return code;
}

// ---- the handoff (the same frames the server's attach path runs) ---------------------------------------------------
void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}
void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}
std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid, std::string_view name, std::string_view password) {
  std::vector<std::uint8_t> out(connection_uuid.begin(), connection_uuid.end());
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, name);
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, password);
  PutString(&out, "native_receipt_gate_test");
  PutString(&out, "");
  return out;
}
sbps::Frame MakeFrame(sbps::MessageType type, std::vector<std::uint8_t> payload, const std::array<std::uint8_t, 16>& connection) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection;
  frame.header.payload_schema_id = 3001;
  frame.payload = std::move(payload);
  return frame;
}

struct Served {
  srv::HostedEngineResult started;
  srv::NativeDatabaseLoop* loop = nullptr;
  std::shared_ptr<const api::NativeServedDatabaseFacts> facts;
  fs::path path, directory;
  p::Uuid database, bootstrap, service;
} served;

struct Approved {
  std::unique_ptr<srv::ServerSessionRegistry> registry = std::make_unique<srv::ServerSessionRegistry>();
  srv::SessionOperationResult result;
  std::array<std::uint8_t, 16> connection{};
  srv::ServerNativeApproval approval;
  const srv::ServerSessionRecord* record = nullptr;
  p::Uuid session() const { return p::Uuid{approval.session_uuid}; }
};
std::unique_ptr<Approved> Approve() {
  auto out = std::make_unique<Approved>();
  out->connection = sbps::MakeUuidV7Bytes();
  const auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(out->connection, kBootstrapName, kBootstrapPassword), out->connection);
  srv::NativeLoginThrottle().Clear();
  out->result = srv::HandleAuthHandoff(out->registry.get(), served.started.state, frame);
  Check(out->result.accepted && out->result.native_approval.present, "the login is approved by the real handoff");
  out->approval = out->result.native_approval;
  const auto found = out->registry->sessions_by_uuid.find(p::Uuid{out->approval.session_uuid});
  Check(found != out->registry->sessions_by_uuid.end(), "the session is recorded in the registry");
  out->record = &found->second;
  const auto& rec = *out->record;
  Check(rec.native_session_approved && rec.native_served_facts == served.facts && rec.served_route_owner != nullptr &&
            rec.local_transaction_id == out->approval.transaction_order && rec.transaction_uuid == out->approval.transaction_uuid,
        "the record carries the facts, the served owner and the transaction the loop began");
  return out;
}
void Disconnect(Approved& a) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = a.connection;
  frame.header.session_uuid = a.approval.session_uuid;
  frame.payload.assign(a.approval.session_uuid.begin(), a.approval.session_uuid.end());
  PutString(&frame.payload, "parser_disconnect_notice");
  (void)srv::HandleDisconnectNotice(a.registry.get(), frame);
}

// ---- the public-ABI engine session: as the server's EnsureServerPublicAbiSessionForContext makes it ---------------------
struct PublicSession {
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  PublicSession() = default;
  PublicSession(const PublicSession&) = delete;
  PublicSession& operator=(const PublicSession&) = delete;
  ~PublicSession() {
    sb_engine_result_t ignored = nullptr;
    if (session) {
      sb_engine_session_end_params_v1_t end{};
      end.struct_size = sizeof(end);
      end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      (void)sb_engine_session_end(session, &end, &ignored);
      if (ignored) (void)sb_engine_result_release(ignored);
    }
    ignored = nullptr;
    if (engine) {
      (void)sb_engine_close(engine, &ignored);
      if (ignored) (void)sb_engine_result_release(ignored);
    }
  }
};
std::unique_ptr<PublicSession> OpenPublic(const std::shared_ptr<d::RouteOwnershipLease>& owner, const std::string& path_text,
                                          const std::shared_ptr<const api::NativeServedDatabaseFacts>& facts,
                                          const std::array<std::uint8_t, 16>& user, const std::array<std::uint8_t, 16>& session_uuid) {
  auto out = std::make_unique<PublicSession>();
  sb_engine_open_params_v1_t open{};
  open.struct_size = sizeof(open);
  open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open.database_path_utf8 = path_text.data();
  open.database_path_size = path_text.size();
  open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_result_t result = nullptr;
  auto status = bridge::OpenServedEngine(&open, owner, SB_DEFAULT_POLICY_PACK_ROOT, facts, &out->engine, &result);
  Check(status == SB_ENGINE_STATUS_OK && out->engine != nullptr, "the engine handle opens on the served native database: " + Code(result));
  sb_engine_session_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::memcpy(params.effective_user_uuid.bytes, user.data(), 16);
  std::memcpy(params.session_uuid.bytes, session_uuid.data(), 16);
  params.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  params.default_language_utf8 = "en";
  params.default_language_size = 2;
  result = nullptr;
  status = sb_engine_session_begin(out->engine, &params, &out->session, &result);
  Check(status == SB_ENGINE_STATUS_OK && out->session != nullptr, "the engine session begins: " + Code(result));
  return out;
}
std::unique_ptr<PublicSession> OpenPublicFor(const Approved& a) {
  const auto& rec = *a.record;
  return OpenPublic(rec.served_route_owner, rec.database_path, rec.native_served_facts, rec.effective_user_uuid, rec.session_uuid);
}

// The context the server materializes for a native session (EngineContextForSessionProjection), as values: the identities of
// the session record and the generation the caller claims the catalog is at.
api::EngineRequestContext ContextFor(const std::string& path_text, const std::array<std::uint8_t, 16>& user,
                                     const std::array<std::uint8_t, 16>& session_uuid, const p::Uuid& transaction,
                                     std::uint64_t order, std::uint64_t generation) {
  api::EngineRequestContext context;
  context.database_path = path_text;
  context.database_uuid = served.facts->database_uuid;
  context.principal_uuid = api::EngineUuid{user};
  context.session_uuid = api::EngineUuid{session_uuid};
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.transaction_uuid = transaction;
  context.local_transaction_id = order;
  context.security_context_present = true;
  context.catalog_generation_id = generation;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid = NewId();
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = 1;
  context.authorization_context.policy_epoch = 1;
  context.authorization_context.catalog_generation_id = generation;
  context.current_package_uuid = NewId();
  return context;
}
api::EngineRequestContext ContextFor(const Approved& a, std::uint64_t generation) {
  const auto& rec = *a.record;
  return ContextFor(rec.database_path, rec.effective_user_uuid, rec.session_uuid, a.approval.transaction_uuid, a.approval.transaction_order, generation);
}

struct Acquired {
  sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
  std::string code;
  bridge::StatementContextReceiptHandle receipt{};
  bridge::StatementContextReceiptView view{};
  bool ok() const { return status == SB_ENGINE_STATUS_OK; }
};
// One acquire. The file's bytes are identical before and after it (the receipt is read only), success or refusal.
// `compare_bytes` false is for the strace state only: the test's own reads of the data path must not sit inside the traced
// region (they would hide an engine-side open of the data path); it compares once around the whole region instead.
Acquired Acquire(PublicSession& ps, const api::EngineRequestContext& context, const p::Uuid& transaction, bool compare_bytes = true) {
  const auto before = compare_bytes ? Image(served.path) : std::vector<p::byte>{};
  Acquired out;
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = transaction;
  sb_engine_result_t result = nullptr;
  out.status = bridge::AcquireStatementContextReceipt(ps.session, &request, &out.receipt, &out.view, &result);
  out.code = Code(result);
  if (compare_bytes)
    Check(Image(served.path) == before, "the acquire (" + (out.ok() ? std::string("ok") : out.code) + ") wrote nothing: the file's bytes are identical");
  return out;
}
void Release(Acquired& a) {
  if (a.receipt) Check(bridge::ReleaseStatementContextReceipt(a.receipt) == SB_ENGINE_STATUS_OK, "the receipt releases");
  a.receipt = {};
}
api::EngineRequestContext CopyContext(const Acquired& a) {
  api::EngineRequestContext copy;
  sb_engine_result_t result = nullptr;
  Check(bridge::CopyStatementContextEngineContextV1(a.receipt, &copy, &result) == SB_ENGINE_STATUS_OK, "the receipt's context copies: " + Code(result));
  return copy;
}

// ---- the offline ground truth ------------------------------------------------------------------------------------------
struct BoundMemory {
  mem::MemoryManager manager;
  mem::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding;
  db::NativeStorageMemory memory;
  static auto Policy(std::uint64_t bytes) {
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = policy.per_context_limit_bytes = 4 * bytes;
    policy.soft_limit_bytes = 0;
    return policy;
  }
  BoundMemory(const p::Uuid& database, std::uint64_t bytes) : manager(Policy(bytes)), binding{database, NewId(), NewId(), NewId()} {
    mem::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = mem::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = mem::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "native receipt test read";
    r.purpose = "the test's own offline names read";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{mem::HierarchicalMemoryScopeKind::process, {}, NewId().bytes},
                     {mem::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "native receipt test";
    for (const auto& scope : r.scope_chain) {
      mem::HierarchicalMemoryBudget b;
      b.scope = scope;
      b.hard_limit_bytes = bytes;
      b.provenance = r.provenance;
      Check(ledger.SetBudget(b).ok(), "offline read parent budget");
    }
    auto grant = mem::AcquireReservationBackedMemoryResource(r);
    Check(grant.ok(), "offline read memory grant");
    auto adopted = db::AdoptNativeStorageMemory(binding, grant.resource);
    Check(adopted.ok(), "offline read grant adoption");
    memory = std::move(adopted.memory);
  }
};
struct NameFact {
  p::Uuid object, scope, language;
  std::string text;
  std::optional<cat::CatalogNameEntry> entry;
  std::optional<cat::CatalogMetadataVersion> metadata;
};
struct DefinitionFact {
  p::Uuid object, row_uuid, version_uuid;
  cat::CatalogMetadataVersion metadata;
};
struct Offline {
  p::Uuid database, filespace;
  std::uint32_t page_size = 0;
  std::map<p::Uuid, DefinitionFact> security;  // role 4 rows by object
  std::vector<NameFact> names;  // live entries only
  std::vector<d::FilespaceRootReference> roots;
  std::uint64_t catalog_generation = 0;  // the maximum over the live entries (what the names source reports)
};
Offline ReadOffline(const fs::path& path) {
  Offline out;
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "the offline read opens the file read-only");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  out.page_size = size;
  out.database = zero.record->bootstrap.database_uuid;
  out.filespace = zero.record->bootstrap.filespace_uuid;
  out.roots = zero.record->roots;
  const auto profile = zero.record->bootstrap.page_size_profile_uuid;
  const auto budget = db::NativeHostedSourceBackingBytes(size);
  const db::NativeCatalogRelationMemoryLimits limits{profile, 1, db::NativeCatalogScanBasePages(size), db::kNativeCatalogScanRelationMaximumRows,
      db::kNativeCatalogScanRelationMaximumRows, db::NativeCatalogScanNavigationLimits(size),
      db::NativeNameTreeNavigationLimits(size, db::kNativeCatalogScanRelationMaximumRows)};
  BoundMemory workspace_memory(out.database, db::NativeCatalogVisibilityWorkspaceBytes(limits)), source_memory(out.database, 32ull * 1024 * 1024);
  auto prepared = db::PrepareNativeCatalogVisibilityRead(limits, workspace_memory.memory, workspace_memory.binding);
  Check(prepared.ok(), "the visibility workspace prepares");
  const std::array<d::NativeFilespaceDevice, 1> devices{{{out.filespace, profile, &device}}};
  auto source = db::AcquireNativeFixedRootMemoryLease(out.database, devices, out.filespace, {budget, 32ull * 1024 * 1024},
                                                      source_memory.memory, source_memory.binding);
  Check(source.ok(), "a fixed-root lease over the file");
  const auto& lease = *source.lease;
  {
    const auto security = db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(lease, 2, 4, budget, prepared.workspace);
    Check(security.ok(), "the security relation reads");
    for (const auto& row : security.rows) {
      if (row.metadata.record.header.deleted) continue;
      DefinitionFact f;
      f.object = row.metadata.record.header.object_uuid.value;
      f.row_uuid = row.metadata.record.header.row_uuid.value;
      f.version_uuid = row.version_uuid;
      f.metadata = cat::MaterializeCatalogMetadataVersion(row.metadata);
      out.security.emplace(f.object, std::move(f));
    }
  }
  const auto names = db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(lease, 2, 2, budget, prepared.workspace);
  Check(names.ok(), "the names relation reads");
  for (const auto& row : names.rows) {
    if (!row.name_payload || row.metadata.record.header.deleted || row.provisional) continue;
    const auto* e = std::get_if<cat::CatalogNameEntryView>(&*row.name_payload);
    if (!e) continue;
    NameFact f;
    f.object = e->object_uuid.value;
    f.scope = e->scope_uuid.value;
    f.language = e->language_uuid.value;
    f.text = std::string(e->raw_name_text);
    f.entry = cat::MaterializeCatalogNameEntry(*e);
    f.metadata = cat::MaterializeCatalogMetadataVersion(row.metadata);
    out.catalog_generation = std::max<std::uint64_t>(out.catalog_generation, f.entry->catalog_generation_id);
    out.names.push_back(std::move(f));
  }
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return out;
}


// A recursive listing of a directory: every entry's relative path and kind (files carry their size).
std::map<std::string, std::string> Listing(const fs::path& root) {
  std::map<std::string, std::string> out;
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
    const auto rel = fs::relative(it->path(), root).string();
    out[rel] = it->is_directory(error) ? "dir" : "file:" + std::to_string(it->file_size(error));
  }
  return out;
}
bool AnySideFile(const fs::path& root) {
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
    if (it->path().filename().string().find(".sb.") != std::string::npos) return true;
  return false;
}

struct Observation {
  std::vector<p::byte> image;
  std::map<std::string, std::string> listing;
};
Observation Before(bool compare) {
  Observation o;
  if (compare) {
    o.image = Image(served.path);
    o.listing = Listing(served.directory);
  }
  return o;
}

// ---- the families -----------------------------------------------------------------------------------------------------------
struct FamilyProbe {
  bool carried = false;  // the entry returned a diagnostic the test can read
  std::string code, family;
};
FamilyProbe FromDiagnostic(const api::EngineApiDiagnostic& diagnostic) {
  FamilyProbe out;
  out.carried = true;
  out.code = diagnostic.code;
  for (const auto& field : diagnostic.fields)
    if (field.key == "registry_family") out.family = field.value;
  return out;
}
template <class T>
concept HasDiagnostic = requires(const T& t) { t.diagnostic.code; t.diagnostic.fields; };
template <class T>
concept HasDiagnostics = requires(const T& t) { t.diagnostics.front().code; };
template <class F>
FamilyProbe ProbeOf(F&& call) {
  using R = std::invoke_result_t<F>;
  if constexpr (std::is_void_v<R>) {
    call();
    return {};
  } else {
    auto result = call();
    if constexpr (std::is_same_v<std::remove_cvref_t<R>, api::EngineApiDiagnostic>) return FromDiagnostic(result);
    else if constexpr (HasDiagnostic<std::remove_cvref_t<R>>) return FromDiagnostic(result.diagnostic);
    else if constexpr (HasDiagnostics<std::remove_cvref_t<R>>) return result.diagnostics.empty() ? FamilyProbe{} : FromDiagnostic(result.diagnostics.front());
    else return {};
  }
}
struct FamilyEntry {
  const char* family;
  const char* id;
  FamilyProbe (*call)(const api::EngineRequestContext&);
  bool legacy_ok;  // callable with default arguments on a LEGACY context (no raw-pointer argument a real caller supplies)
};
constexpr const char* kRefusal = "ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED";

const std::vector<FamilyEntry>& FamilyTable() {
  static const std::vector<FamilyEntry> table = {
{"backup_archive_lifecycle", "EvaluateBackupArchiveLifecycleAdmission#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineApiRequest> a0{}; a0.context = c; scratchbird::engine::internal_api::BackupArchiveLifecycleOperation a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::EvaluateBackupArchiveLifecycleAdmission(a0, a1); }); }, true},
{"backup_archive_lifecycle", "EngineStartBackupForwardSession#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineStartBackupForwardSessionRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineStartBackupForwardSession(a0); }); }, true},
{"backup_archive_lifecycle", "EngineFinishBackupForwardSession#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineFinishBackupForwardSessionRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineFinishBackupForwardSession(a0); }); }, true},
{"backup_archive_lifecycle", "EngineStartLogicalBackup#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineStartLogicalBackupRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineStartLogicalBackup(a0); }); }, true},
{"backup_archive_lifecycle", "EngineRestoreLogicalBackup#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineRestoreLogicalBackupRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineRestoreLogicalBackup(a0); }); }, true},
{"backup_archive_lifecycle", "EngineUpdateBackupFromVerifiedCoverage#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineUpdateBackupFromVerifiedCoverageRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineUpdateBackupFromVerifiedCoverage(a0); }); }, true},
{"backup_archive_lifecycle", "EnginePackageDeltaStream#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EnginePackageDeltaStreamRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnginePackageDeltaStream(a0); }); }, true},
{"backup_archive_lifecycle", "EngineApplyDeltaStream#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineApplyDeltaStreamRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineApplyDeltaStream(a0); }); }, true},
{"backup_archive_lifecycle", "EngineStartPhysicalBackup#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineStartPhysicalBackupRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineStartPhysicalBackup(a0); }); }, true},
{"backup_archive_lifecycle", "EngineRestorePhysicalBackup#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineRestorePhysicalBackupRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineRestorePhysicalBackup(a0); }); }, true},
{"api_events", "LoadApiBehaviorState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadApiBehaviorState(c); }); }, true},
{"api_events", "VisibleApiBehaviorRecords#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; unsigned long a2{}; std::remove_cvref_t<scratchbird::engine::internal_api::EngineApiDiagnostic> a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::VisibleApiBehaviorRecords(c, a1, a2, a3); }); }, true},
{"api_events", "FindVisibleApiBehaviorRecord#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; std::remove_cvref_t<scratchbird::engine::internal_api::EngineApiDiagnostic> a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::FindVisibleApiBehaviorRecord(c, a1, a2, a3); }); }, true},
{"api_events", "AppendApiBehaviorEvent#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendApiBehaviorEvent(c, a1); }); }, true},
{"api_events", "PersistApiBehaviorRecord#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineApiRequest> a0{}; a0.context = c; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; bool a3{}; std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a4{}; bool a5{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PersistApiBehaviorRecord(a0, a1, a2, a3, a4, a5); }); }, true},
{"api_events", "PersistApiBehaviorRecordWithPayload#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineApiRequest> a0{}; a0.context = c; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; bool a3{}; std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a4{}; bool a5{}; std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PersistApiBehaviorRecordWithPayload(a0, a1, a2, a3, a4, a5, a6); }); }, true},
{"catalog_object_events", "EngineCatalogCreateObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogCreateObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogCreateObject(a0); }); }, true},
{"catalog_object_events", "EngineCatalogPublishComment#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCommentOnObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogPublishComment(a0); }); }, true},
{"catalog_object_events", "EngineCatalogApplyConstraintsToObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogApplyConstraintsRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogApplyConstraintsToObject(a0); }); }, true},
{"catalog_object_events", "EngineCatalogAlterObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogAlterObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogAlterObject(a0); }); }, true},
{"catalog_object_events", "EngineCatalogRenameObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogRenameObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogRenameObject(a0); }); }, true},
{"catalog_object_events", "EngineCatalogDropObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogDropObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogDropObject(a0); }); }, true},
{"catalog_object_events", "EngineCatalogLookupObjectByUuid#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogLookupObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogLookupObjectByUuid(a0); }); }, true},
{"catalog_object_events", "EngineCatalogResolveObjectName#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogResolveObjectNameRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogResolveObjectName(a0); }); }, true},
{"catalog_object_events", "EngineCatalogValidateMetadataCache#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCatalogValidateMetadataCacheRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCatalogValidateMetadataCache(a0); }); }, true},
{"name_events", "AppendNameRegistryEntry#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::NameRegistryEntry> a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendNameRegistryEntry(c, a1, a2); }); }, true},
{"name_events", "PersistNameRegistryEntriesForObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a3{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a4{}; std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::EngineLocalizedName, std::allocator<scratchbird::engine::internal_api::EngineLocalizedName> >> a5{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PersistNameRegistryEntriesForObject(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"name_events", "RetireNameRegistryEntriesForObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RetireNameRegistryEntriesForObject(c, a1, a2); }); }, true},
{"crud_events", "AppendCrudEvent#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendCrudEvent(c, a1); }); }, true},
{"serializable_access", "RecordSerializableSelectRead#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a1{}; scratchbird::core::platform::Uuid a2{}; std::remove_cvref_t<scratchbird::engine::internal_api::EnginePredicateEnvelope> a3{}; std::span<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const, 18446744073709551615ul> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::dml::RecordSerializableSelectRead(c, a1, a2, a3, a4); }); }, true},
{"serializable_access", "CheckSerializableInsertMutation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a1{}; scratchbird::core::platform::Uuid a2{}; std::span<scratchbird::engine::internal_api::EngineRowValue const, 18446744073709551615ul> a3{}; std::span<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const, 18446744073709551615ul> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::dml::CheckSerializableInsertMutation(c, a1, a2, a3, a4); }); }, true},
{"serializable_access", "RecordSerializableInsertMutation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a1{}; scratchbird::core::platform::Uuid a2{}; std::span<scratchbird::engine::internal_api::EngineRowValue const, 18446744073709551615ul> a3{}; std::span<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const, 18446744073709551615ul> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::dml::RecordSerializableInsertMutation(c, a1, a2, a3, a4); }); }, true},
{"serializable_access", "CheckSerializablePredicateMutation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a1{}; scratchbird::core::platform::Uuid a2{}; std::remove_cvref_t<scratchbird::engine::internal_api::EnginePredicateEnvelope> a3{}; bool a4{}; std::span<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const, 18446744073709551615ul> a5{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::dml::CheckSerializablePredicateMutation(c, a1, a2, a3, a4, a5); }); }, true},
{"serializable_access", "RecordSerializablePredicateMutation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > a1{}; scratchbird::core::platform::Uuid a2{}; std::remove_cvref_t<scratchbird::engine::internal_api::EnginePredicateEnvelope> a3{}; bool a4{}; std::span<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const, 18446744073709551615ul> a5{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::dml::RecordSerializablePredicateMutation(c, a1, a2, a3, a4, a5); }); }, true},
{"domain_catalog", "LoadDomainState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadDomainState(c); }); }, true},
{"domain_catalog", "AppendDomainEvent#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendDomainEvent(c, a1); }); }, true},
{"domain_catalog", "FindVisibleDomain#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::FindVisibleDomain(c, a1, a2); }); }, true},
{"domain_catalog", "BindDomainInnerBaseDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::DomainRecord> a1{}; scratchbird::engine::internal_api::DomainRecord* a2 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::BindDomainInnerBaseDescriptor(c, a1, a2); }); }, false},
{"domain_catalog", "ResolveDomainInheritedProfile#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ResolveDomainInheritedProfile(c, a1, a2); }); }, true},
{"domain_catalog", "AdmitDomainMutationChain#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AdmitDomainMutationChain(c, a1, a2); }); }, true},
{"domain_catalog", "DomainChainContainsUuid#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::DomainChainContainsUuid(c, a1, a2, a3); }); }, true},
{"domain_catalog", "ValidateDomainTypedValue#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineDescriptor> a1{}; std::remove_cvref_t<scratchbird::engine::internal_api::EngineTypedValue> a2{}; unsigned long a3{}; bool a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ValidateDomainTypedValue(c, a1, a2, a3, a4); }); }, true},
{"domain_catalog", "ApplyDomainRulesToCrudValues#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::vector<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > >, std::allocator<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > >> a1{}; std::remove_cvref_t<std::vector<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, scratchbird::engine::internal_api::CrudStoredValue>, std::allocator<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, scratchbird::engine::internal_api::CrudStoredValue> > >> a2{}; unsigned long a3{}; scratchbird::engine::internal_api::ConstraintDmlValidationCache* a4 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::ApplyDomainRulesToCrudValues(c, a1, a2, a3, a4); }); }, false},
{"domain_catalog", "ApplyDomainReadPoliciesToCrudValues#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::vector<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > >, std::allocator<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > >> a1{}; std::remove_cvref_t<std::vector<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, scratchbird::engine::internal_api::CrudStoredValue>, std::allocator<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, scratchbird::engine::internal_api::CrudStoredValue> > >> a2{}; unsigned long a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ApplyDomainReadPoliciesToCrudValues(c, a1, a2, a3); }); }, true},
{"executable_object_events", "ValidateExecutableObjectCurrentAdmission#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineExecutableObjectRecord> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ValidateExecutableObjectCurrentAdmission(c, a1); }); }, true},
{"executable_object_events", "LoadExecutableObjectLifecycleState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadExecutableObjectLifecycleState(c); }); }, true},
{"executable_object_events", "LoadExecutableObjectLifecycleStateForRuntimeDispatch#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadExecutableObjectLifecycleStateForRuntimeDispatch(c); }); }, true},
{"executable_object_events", "PreflightCreateExecutableObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCreateExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::PreflightCreateExecutableObject(a0); }); }, true},
{"executable_object_events", "EngineCreateExecutableObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCreateExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCreateExecutableObject(a0); }); }, true},
{"executable_object_events", "EngineAlterExecutableObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineAlterExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineAlterExecutableObject(a0); }); }, true},
{"executable_object_events", "EngineDropExecutableObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineDropExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineDropExecutableObject(a0); }); }, true},
{"executable_object_events", "EngineQuiesceExecutableObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineQuiesceExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineQuiesceExecutableObject(a0); }); }, true},
{"executable_object_events", "EngineUnloadExecutableObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineUnloadExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineUnloadExecutableObject(a0); }); }, true},
{"executable_object_events", "EngineBeginExecutableObjectInvocation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineBeginExecutableObjectInvocationRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineBeginExecutableObjectInvocation(a0); }); }, true},
{"executable_object_events", "EngineFinishExecutableObjectInvocation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineFinishExecutableObjectInvocationRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineFinishExecutableObjectInvocation(a0); }); }, true},
{"executable_object_events", "EngineInvokeExecutableObject#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineInvokeExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineInvokeExecutableObject(a0); }); }, true},
{"executable_object_events", "EngineFireExecutableEventTrigger#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineFireExecutableEventTriggerRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineFireExecutableEventTrigger(a0); }); }, true},
{"executable_object_events", "EngineInspectExecutableObjects#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineInspectExecutableObjectRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineInspectExecutableObjects(a0); }); }, true},
{"sequence_generator_events", "LoadSequenceGeneratorLifecycleState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadSequenceGeneratorLifecycleState(c); }); }, true},
{"sequence_generator_events", "EngineSequenceCreateGenerator#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceCreateGeneratorRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceCreateGenerator(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceAlterGenerator#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceAlterGeneratorRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceAlterGenerator(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceRestartGenerator#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceRestartGeneratorRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceRestartGenerator(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceDropGenerator#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceDropGeneratorRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceDropGenerator(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceAllocateValue#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceAllocateValueRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceAllocateValue(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceApplyMgaTransactionOutcome#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceApplyMgaTransactionOutcomeRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceApplyMgaTransactionOutcome(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceBindIdentityValue#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceBindIdentityValueRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceBindIdentityValue(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceRecoverGeneratorState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceRecoverGeneratorStateRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceRecoverGeneratorState(a0); }); }, true},
{"sequence_generator_events", "EngineSequenceEvaluateMgaRetention#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineSequenceEvaluateMgaRetentionRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineSequenceEvaluateMgaRetention(a0); }); }, true},
{"sys_memory_catalogs", "EnginePlanMemoryManagementOperation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineMemoryManagementRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnginePlanMemoryManagementOperation(a0); }); }, true},
{"mga_bulk_import_publication", "PrepareMgaBulkImportPublicationV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaBulkImportPublicationRecordV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PrepareMgaBulkImportPublicationV1(c, a1); }); }, true},
{"mga_bulk_import_publication", "PublishMgaBulkImportPublicationV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaBulkImportPublicationRecordV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishMgaBulkImportPublicationV1(c, a1); }); }, true},
{"mga_bulk_import_publication", "AbortMgaBulkImportPublicationV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaBulkImportPublicationRecordV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AbortMgaBulkImportPublicationV1(c, a1); }); }, true},
{"mga_bulk_import_publication", "RecoverMgaBulkImportPublicationV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::array<unsigned char, 32ul>> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverMgaBulkImportPublicationV1(c, a1); }); }, true},
{"mga_bulk_import_imported_rows", "StoreMgaBulkImportImportedRowEventsV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::MgaBulkImportImportedRowEventV1, std::allocator<scratchbird::engine::internal_api::MgaBulkImportImportedRowEventV1> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::StoreMgaBulkImportImportedRowEventsV1(c, a1); }); }, true},
{"mga_bulk_import_imported_rows", "RecoverMgaBulkImportImportedRowEventsV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::array<unsigned char, 32ul>> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverMgaBulkImportImportedRowEventsV1(c, a1); }); }, true},
{"mga_event_sequence_allocator", "ReserveEventSequenceRange#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; unsigned long a3{}; std::remove_cvref_t<std::function<unsigned long ()>> a4{}; std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >* a5 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReserveEventSequenceRange(c, a1, a2, a3, a4, a5); }); }, false},
{"mga_event_sequence_allocator", "AppendDeferredEventSequenceAllocatorLines#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > >* a1 = nullptr; scratchbird::engine::internal_api::MgaRelationHotAppendCounters* a2 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendDeferredEventSequenceAllocatorLines(c, a1, a2); }); }, false},
{"mga_event_sequence_allocator", "ScanNextRowEventSequence#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ScanNextRowEventSequence(c); }); }, true},
{"mga_event_sequence_allocator", "NextRowEventSequence#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::NextRowEventSequence(c); }); }, true},
{"mga_event_sequence_allocator", "ScanNextIndexEventSequence#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ScanNextIndexEventSequence(c); }); }, true},
{"mga_event_sequence_allocator", "NextIndexEventSequence#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::NextIndexEventSequence(c); }); }, true},
{"mga_event_sequence_allocator", "ScanNextMetadataEventSequence#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ScanNextMetadataEventSequence(c); }); }, true},
{"mga_event_sequence_allocator", "NextMetadataEventSequence#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::NextMetadataEventSequence(c); }); }, true},
{"mga_event_sequence_allocator", "ReserveMgaRowEventSequenceRangeForTesting#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { unsigned long a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReserveMgaRowEventSequenceRangeForTesting(c, a1); }); }, true},
{"mga_large_values", "ExpandVisibleMgaLargeValuesBounded#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::vector<scratchbird::engine::internal_api::CrudRowVersionRecord, std::allocator<scratchbird::engine::internal_api::CrudRowVersionRecord> >* a1 = nullptr; scratchbird::engine::internal_api::BoundedScopedRowReadControl* a2 = nullptr; unsigned long a3{}; std::remove_cvref_t<std::function<bool (unsigned long)>> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ExpandVisibleMgaLargeValuesBounded(c, a1, a2, a3, a4); }); }, false},
{"mga_large_values", "LoadVisibleMgaLargeValueReclaims#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadVisibleMgaLargeValueReclaims(c); }); }, true},
{"mga_large_values", "ClassifyMgaTemporaryLargeValueRecovery#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::set<scratchbird::core::platform::Uuid, std::less<scratchbird::core::platform::Uuid>, std::allocator<scratchbird::core::platform::Uuid> >> a1{}; std::remove_cvref_t<std::map<unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::less<unsigned long>, std::allocator<std::pair<unsigned long const, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > >> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ClassifyMgaTemporaryLargeValueRecovery(c, a1, a2); }); }, true},
{"mga_large_values", "ExpandMgaLargeValueLocators#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::vector<scratchbird::engine::internal_api::CrudRowVersionRecord, std::allocator<scratchbird::engine::internal_api::CrudRowVersionRecord> >* a1 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::ExpandMgaLargeValueLocators(c, a1); }); }, false},
{"mga_large_values", "PersistMgaLargeValuesForRow#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; bool a4{}; std::vector<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, scratchbird::engine::internal_api::CrudStoredValue>, std::allocator<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, scratchbird::engine::internal_api::CrudStoredValue> > >* a5 = nullptr; std::vector<scratchbird::engine::internal_api::EngineEvidenceReference, std::allocator<scratchbird::engine::internal_api::EngineEvidenceReference> >* a6 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::PersistMgaLargeValuesForRow(c, a1, a2, a3, a4, a5, a6); }); }, false},
{"mga_large_values", "PersistMgaLargeValuesForRows#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::MgaLargeValuePersistBatchRowInput, std::allocator<scratchbird::engine::internal_api::MgaLargeValuePersistBatchRowInput> >> a1{}; scratchbird::engine::internal_api::MgaLargeValuePersistBatchCounters* a2 = nullptr; std::vector<scratchbird::engine::internal_api::EngineEvidenceReference, std::allocator<scratchbird::engine::internal_api::EngineEvidenceReference> >* a3 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::PersistMgaLargeValuesForRows(c, a1, a2, a3); }); }, false},
{"mga_large_values", "AppendMgaLargeValueReclaimMarkersForRowVersion#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { unsigned long a1{}; std::remove_cvref_t<scratchbird::engine::internal_api::CrudRowVersionRecord> a2{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a3{}; std::set<scratchbird::core::platform::Uuid, std::less<scratchbird::core::platform::Uuid>, std::allocator<scratchbird::core::platform::Uuid> >* a4 = nullptr; unsigned long* a5 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaLargeValueReclaimMarkersForRowVersion(c, a1, a2, a3, a4, a5); }); }, false},
{"mga_relation_descriptors", "LoadDescriptorFieldsSnapshot#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadDescriptorFieldsSnapshot(c, a1); }); }, true},
{"mga_relation_descriptors", "LoadDescriptorFieldsByRelation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadDescriptorFieldsByRelation(c, a1); }); }, true},
{"mga_relation_metadata", "PersistDescriptorFields#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<std::vector<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > >, std::allocator<std::pair<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > >> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PersistDescriptorFields(c, a1, a2); }); }, true},
{"mga_relation_metadata", "LoadMgaMetadata#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::RelationReadSnapshot* a0 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadMgaMetadata(a0, c); }); }, false},
{"mga_relation_metadata", "LoadMgaMetadataSnapshot#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadMgaMetadataSnapshot(c); }); }, true},
{"mga_relation_metadata", "AppendMgaTableMetadata#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::CrudTableRecord> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaTableMetadata(c, a1); }); }, true},
{"mga_relation_metadata", "AppendMgaTableMetadataWithSealedContextualTextDescriptorV2#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::CrudTableRecord> a1{}; std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::CrudIndexRecord, std::allocator<scratchbird::engine::internal_api::CrudIndexRecord> >> a2{}; scratchbird::engine::internal_api::MgaRelationStorageDescriptor* a3 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaTableMetadataWithSealedContextualTextDescriptorV2(c, a1, a2, a3); }); }, false},
{"mga_relation_metadata", "AppendMgaConstraintMutationBatch#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaConstraintMutationBatch> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaConstraintMutationBatch(c, a1); }); }, true},
{"mga_relation_metadata", "AppendMgaBigintIdentityMigrationBatch#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaBigintIdentityMigrationRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaBigintIdentityMigrationBatch(c, a1); }); }, true},
{"mga_relation_metadata", "AppendMgaInt32IdentityMigrationBatch#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaInt32IdentityMigrationRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaInt32IdentityMigrationBatch(c, a1); }); }, true},
{"mga_relation_metadata", "AppendMgaTextIdentityMigrationBatch#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaTextIdentityMigrationRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaTextIdentityMigrationBatch(c, a1); }); }, true},
{"mga_relation_metadata", "AppendMgaIndexMetadata#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::CrudIndexRecord> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaIndexMetadata(c, a1); }); }, true},
{"mga_row_versions", "UpdateScopedRelationSummariesForStoreModule#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::map<scratchbird::core::platform::Uuid, scratchbird::engine::internal_api::ScopedRelationSummaryDelta, std::less<scratchbird::core::platform::Uuid>, std::allocator<std::pair<scratchbird::core::platform::Uuid const, scratchbird::engine::internal_api::ScopedRelationSummaryDelta> > >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::UpdateScopedRelationSummariesForStoreModule(c, a1); }); }, true},
{"mga_relation_metadata", "FilterMgaRelationMetadataForStoreModule#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::RelationReadSnapshot* a1 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::FilterMgaRelationMetadataForStoreModule(c, a1); }); }, false},
{"mga_relation_metadata", "FilterVisibleRetiredTemporaryMetadataForStoreModule#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::RelationReadSnapshot* a1 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::FilterVisibleRetiredTemporaryMetadataForStoreModule(c, a1); }); }, false},
{"mga_row_versions", "HasVisibleMgaDeferredConstraintMetadata#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::HasVisibleMgaDeferredConstraintMetadata(c); }); }, true},
{"mga_row_versions", "HasMgaTemporaryCleanupMetadataWork#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { bool a1{}; bool a2{}; bool a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::HasMgaTemporaryCleanupMetadataWork(c, a1, a2, a3); }); }, true},
{"mga_row_versions", "LoadMgaRelationStoreState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadMgaRelationStoreState(c); }); }, true},
{"mga_row_versions", "EnsureMgaRelationStorageDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::CrudTableRecord> a1{}; std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::CrudIndexRecord, std::allocator<scratchbird::engine::internal_api::CrudIndexRecord> >> a2{}; scratchbird::engine::internal_api::MgaRelationStorageDescriptor* a3 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnsureMgaRelationStorageDescriptor(c, a1, a2, a3); }); }, false},
{"mga_row_versions", "PrepareMgaHeapReadAuthoritiesForStoreModule#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::span<scratchbird::core::platform::Uuid const, 18446744073709551615ul> a1{}; scratchbird::transaction::mga::SnapshotVectorDescriptor const* a2 = nullptr; scratchbird::engine::internal_api::EngineDmlDeleteBindingAuthorityV1 const* a3 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::PrepareMgaHeapReadAuthoritiesForStoreModule(c, a1, a2, a3); }); }, false},
{"mga_row_versions", "PrepareMgaHeapReadAuthorityForStoreModule#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PrepareMgaHeapReadAuthorityForStoreModule(c, a1); }); }, true},
{"mga_relation_scope", "LoadDecodedScopedRowsForTable#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::vector<scratchbird::engine::internal_api::CrudRowVersionRecord, std::allocator<scratchbird::engine::internal_api::CrudRowVersionRecord> >* a2 = nullptr; bool* a3 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadDecodedScopedRowsForTable(c, a1, a2, a3); }); }, false},
{"mga_relation_scope", "LoadDecodedScopedRowsForTableBounded#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; scratchbird::engine::internal_api::BoundedScopedRowReadControl* a2 = nullptr; std::vector<scratchbird::engine::internal_api::CrudRowVersionRecord, std::allocator<scratchbird::engine::internal_api::CrudRowVersionRecord> >* a3 = nullptr; bool* a4 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadDecodedScopedRowsForTableBounded(c, a1, a2, a3, a4); }); }, false},
{"mga_savepoints", "ParseSavepointsBounded#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::BoundedScopedRowReadControl* a1 = nullptr; unsigned long a2{}; scratchbird::engine::internal_api::SavepointParsedState* a3 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::ParseSavepointsBounded(c, a1, a2, a3); }); }, false},
{"mga_savepoints", "ParseSavepoints#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ParseSavepoints(c); }); }, true},
{"mga_savepoints", "ObserveMgaSavepointMarker#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ObserveMgaSavepointMarker(c, a1, a2); }); }, true},
{"mga_savepoints", "ObserveUniqueMgaSavepointMarkerV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ObserveUniqueMgaSavepointMarkerV1(c, a1); }); }, true},
{"mga_savepoints", "ValidateMgaSavepointMarkerAuthority#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ValidateMgaSavepointMarkerAuthority(c); }); }, true},
{"mga_savepoints", "CurrentMgaSavepointAuthorityGeneration#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::CurrentMgaSavepointAuthorityGeneration(c); }); }, true},
{"mga_savepoints", "CreateMgaSavepointMarker#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CreateMgaSavepointMarker(c, a1); }); }, true},
{"mga_savepoints", "CreateMgaStatementSavepointMarker#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CreateMgaStatementSavepointMarker(c, a1); }); }, true},
{"mga_savepoints", "ValidateMgaStatementSavepointFinality#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ValidateMgaStatementSavepointFinality(c); }); }, true},
{"mga_savepoints", "ReleaseMgaSavepointMarker#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReleaseMgaSavepointMarker(c, a1); }); }, true},
{"mga_savepoints", "RollbackToMgaSavepointMarker#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RollbackToMgaSavepointMarker(c, a1); }); }, true},
{"mga_savepoints", "ValidateMgaSavepointExists#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ValidateMgaSavepointExists(c, a1, a2); }); }, true},
{"mga_savepoints", "ActiveMgaSavepointNames#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ActiveMgaSavepointNames(c); }); }, true},
{"mga_index_entries", "LoadMgaSecondaryIndexDeltaLedger#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadMgaSecondaryIndexDeltaLedger(c); }); }, true},
{"mga_index_entries", "AppendMgaSecondaryIndexDeltaLedgerEntries#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::MgaSecondaryIndexDeltaLedgerEntryInput, std::allocator<scratchbird::engine::internal_api::MgaSecondaryIndexDeltaLedgerEntryInput> >> a1{}; std::vector<scratchbird::engine::internal_api::EngineEvidenceReference, std::allocator<scratchbird::engine::internal_api::EngineEvidenceReference> >* a2 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::AppendMgaSecondaryIndexDeltaLedgerEntries(c, a1, a2); }); }, false},
{"mga_index_entries", "CommitMgaSecondaryIndexDeltaLedgerTransaction#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { unsigned long a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CommitMgaSecondaryIndexDeltaLedgerTransaction(c, a1); }); }, true},
{"mga_index_entries", "RollbackMgaSecondaryIndexDeltaLedgerTransaction#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { unsigned long a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RollbackMgaSecondaryIndexDeltaLedgerTransaction(c, a1); }); }, true},
{"mga_index_entries", "MergeMgaSecondaryIndexDeltasForIndex#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaSecondaryIndexDeltaMergeAgentRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::MergeMgaSecondaryIndexDeltasForIndex(c, a1); }); }, true},
{"mga_index_entries", "ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaSecondaryIndexDeltaRecoveryRepairRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(c, a1); }); }, true},
{"mga_index_entries", "CleanupMgaSecondaryIndexGarbageForIndex#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaSecondaryIndexGarbageCleanupRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CleanupMgaSecondaryIndexGarbageForIndex(c, a1); }); }, true},
{"mga_index_entries", "IndexedMgaRowsForPredicateForContext#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::RelationReadSnapshot> a0{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::engine::internal_api::EnginePredicateEnvelope> a2{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::IndexedMgaRowsForPredicateForContext(a0, a1, a2, c, a4); }); }, true},
{"mga_row_versions", "ClassifyMgaTemporaryRecoveryState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ClassifyMgaTemporaryRecoveryState(c); }); }, true},
{"mga_row_versions", "ApplyMgaTemporaryOnCommitActions#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { unsigned long a1{}; unsigned long* a2 = nullptr; unsigned long* a3 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::ApplyMgaTemporaryOnCommitActions(c, a1, a2, a3); }); }, false},
{"mga_row_versions", "ApplyMgaTemporarySessionCleanupActions#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { unsigned long a1{}; unsigned long* a2 = nullptr; unsigned long* a3 = nullptr; unsigned long* a4 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::ApplyMgaTemporarySessionCleanupActions(c, a1, a2, a3, a4); }); }, false},
{"mga_row_versions", "DropMgaTemporaryTable#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::DropMgaTemporaryTable(c, a1); }); }, true},
{"mga_durable_operations", "ReserveMgaDmlUpdateDurableOperationAuthorityV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateDurableAuthorityReservationRequestV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReserveMgaDmlUpdateDurableOperationAuthorityV1(c, a1); }); }, true},
{"mga_durable_operations", "AbandonMgaDmlUpdateDurableOperationAuthorityReservationV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateDurableOperationIdentityV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AbandonMgaDmlUpdateDurableOperationAuthorityReservationV1(c, a1); }); }, true},
{"mga_durable_operations", "RecoverMgaDmlUpdateDurableOperationChainV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateDurableOperationLookupV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverMgaDmlUpdateDurableOperationChainV1(c, a1); }); }, true},
{"mga_durable_operations", "RollbackMgaDmlUpdateStatementFromValidatedDurableAuthorityV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateValidatedDurableAuthorityHandleV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RollbackMgaDmlUpdateStatementFromValidatedDurableAuthorityV1(c, a1); }); }, true},
{"mga_durable_operations", "TruncateMgaDmlUpdateDurableExtentForTestingV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateDurableOperationLookupV1> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::TruncateMgaDmlUpdateDurableExtentForTestingV1(c, a1, a2); }); }, true},
{"mga_durable_operations", "QuarantineMgaDmlUpdateDurableOperationForTestingV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateDurableOperationLookupV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::QuarantineMgaDmlUpdateDurableOperationForTestingV1(c, a1); }); }, true},
{"mga_durable_operations", "DeleteMgaDmlUpdateDurableOperationForTestingV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateDurableOperationLookupV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::DeleteMgaDmlUpdateDurableOperationForTestingV1(c, a1); }); }, true},
{"mga_update_statement_savepoints", "CreateMgaDmlUpdateStatementSavepointAuthorityV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateStatementSavepointBindingV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CreateMgaDmlUpdateStatementSavepointAuthorityV1(c, a1); }); }, true},
{"mga_update_statement_savepoints", "CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateStatementSavepointBindingV1> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1(c, a1, a2, a3); }); }, true},
{"mga_update_statement_savepoints", "RollbackMgaDmlUpdateStatementSavepointAuthorityV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateStatementSavepointAuthorityV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RollbackMgaDmlUpdateStatementSavepointAuthorityV1(c, a1); }); }, true},
{"mga_update_statement_savepoints", "ReleaseMgaDmlUpdateStatementSavepointAuthorityV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::MgaDmlUpdateStatementSavepointAuthorityV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReleaseMgaDmlUpdateStatementSavepointAuthorityV1(c, a1); }); }, true},
{"mga_update_statement_savepoints", "ApplyDmlUpdateBinarySavepointRecordsForStoreModule#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::SavepointParsedState* a1 = nullptr; std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >* a2 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::ApplyDmlUpdateBinarySavepointRecordsForStoreModule(c, a1, a2); }); }, false},
{"nosql_document_provider", "EngineDocumentFind#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineDocumentFindRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineDocumentFind(a0); }); }, true},
{"nosql_document_provider", "EngineDocumentDelete#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineDocumentDeleteRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineDocumentDelete(a0); }); }, true},
{"nosql_document_provider", "EngineDocumentProviderCleanup#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { bool a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineDocumentProviderCleanup(c, a1); }); }, true},
{"document_path_provider", "DocumentPathProviderIdentityForContext#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { unsigned long a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::DocumentPathProviderIdentityForContext(c, a1, a2); }); }, true},
{"nosql_provider_generations", "PublishNoSqlProviderGeneration#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineNoSqlProviderGenerationMetadata> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishNoSqlProviderGeneration(c, a1); }); }, true},
{"nosql_provider_generations", "LoadNoSqlProviderGeneration#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::EngineNoSqlProviderFamily a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadNoSqlProviderGeneration(c, a1, a2, a3); }); }, true},
{"nosql_provider_generations", "LoadNoSqlProviderGeneration#1", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::EngineNoSqlProviderFamily a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadNoSqlProviderGeneration(c, a1, a2, a3); }); }, true},
{"nosql_provider_generations", "ValidateNoSqlProviderGeneration#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineNoSqlPhysicalProviderContract> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ValidateNoSqlProviderGeneration(c, a1); }); }, true},
{"nosql_provider_generations", "RepairNoSqlProviderGeneration#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineNoSqlProviderGenerationRepairRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RepairNoSqlProviderGeneration(c, a1); }); }, true},
{"nosql_provider_generations", "DropNoSqlProviderGeneration#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { scratchbird::engine::internal_api::EngineNoSqlProviderFamily a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::DropNoSqlProviderGeneration(c, a1, a2, a3); }); }, true},
{"nosql_provider_generations", "ListNoSqlProviderGenerations#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::ListNoSqlProviderGenerations(c); }); }, true},
{"nosql_provider_generations", "CleanupNoSqlProviderGenerations#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { bool a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CleanupNoSqlProviderGenerations(c, a1); }); }, true},
{"notification_events", "EngineCreateEventChannel#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCreateEventChannelRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCreateEventChannel(a0); }); }, true},
{"notification_events", "EngineAlterEventChannel#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineAlterEventChannelRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineAlterEventChannel(a0); }); }, true},
{"notification_events", "EngineDropEventChannel#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineDropEventChannelRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineDropEventChannel(a0); }); }, true},
{"notification_events", "EngineListenNotification#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineListenNotificationRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineListenNotification(a0); }); }, true},
{"notification_events", "EngineUnlistenNotification#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineUnlistenNotificationRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineUnlistenNotification(a0); }); }, true},
{"notification_events", "EngineNotifyEventChannel#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineNotifyEventChannelRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineNotifyEventChannel(a0); }); }, true},
{"notification_events", "EngineListEventSubscriptions#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineListEventSubscriptionsRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineListEventSubscriptions(a0); }); }, true},
{"notification_events", "EnginePollEventDelivery#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EnginePollEventDeliveryRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnginePollEventDelivery(a0); }); }, true},
{"notification_events", "EngineAcknowledgeEventDelivery#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineAcknowledgeEventDeliveryRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineAcknowledgeEventDelivery(a0); }); }, true},
{"notification_events", "EngineUnlistenSessionNotifications#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineUnlistenSessionNotificationsRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineUnlistenSessionNotifications(a0); }); }, true},
{"optimizer_plan_events", "LoadOptimizerPlanLifecycleState#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadOptimizerPlanLifecycleState(c); }); }, true},
{"optimizer_plan_events", "EngineOptimizerCachePlan#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineOptimizerCachePlanRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineOptimizerCachePlan(a0); }); }, true},
{"optimizer_plan_events", "EngineOptimizerValidateCachedPlan#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineOptimizerValidateCachedPlanRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineOptimizerValidateCachedPlan(a0); }); }, true},
{"optimizer_plan_events", "EngineOptimizerInvalidatePlanCache#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineOptimizerInvalidatePlanCacheRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineOptimizerInvalidatePlanCache(a0); }); }, true},
{"optimizer_plan_events", "EngineOptimizerRecoverPlanCache#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineOptimizerRecoverPlanCacheRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineOptimizerRecoverPlanCache(a0); }); }, true},
{"sblr_access_cursor_open", "CompileSblrAccessCursorOpenDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned int a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileSblrAccessCursorOpenDescriptor(c, a1, a2, a3, a4); }); }, true},
{"sblr_access_cursor_open", "ConsumeSblrAccessCursorOpenDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrAccessCursorOpenDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ConsumeSblrAccessCursorOpenDescriptor(c, a1); }); }, true},
{"sblr_access_cursor_open", "CompileSblrAccessCursorFetchDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned int a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileSblrAccessCursorFetchDescriptor(c, a1, a2, a3, a4); }); }, true},
{"sblr_access_cursor_open", "ConsumeSblrAccessCursorFetchDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrAccessCursorFetchDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ConsumeSblrAccessCursorFetchDescriptor(c, a1); }); }, true},
{"sblr_access_cursor_open", "CompileSblrAccessCursorCloseDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned int a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileSblrAccessCursorCloseDescriptor(c, a1, a2, a3, a4); }); }, true},
{"sblr_access_cursor_open", "ConsumeSblrAccessCursorCloseDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrAccessCursorCloseDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ConsumeSblrAccessCursorCloseDescriptor(c, a1); }); }, true},
{"sblr_access_cursor_open", "RecoverSblrAccessCursorOpenDescriptors#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrAccessCursorOpenDescriptors(c); }); }, true},
{"sblr_autonomous_frame", "PublishSblrAutonomousBodyFrameProjection#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrAutonomousBodyFrameProjectionV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrAutonomousBodyFrameProjection(c, a1); }); }, true},
{"sblr_autonomous_frame", "CompileAndPublishSblrAutonomousBodyFrameProjection#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileAndPublishSblrAutonomousBodyFrameProjection(c, a1, a2); }); }, true},
{"sblr_autonomous_frame", "RevokeSblrAutonomousBodyFrameProjection#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RevokeSblrAutonomousBodyFrameProjection(c, a1); }); }, true},
{"sblr_autonomous_frame", "ReserveSblrAutonomousFrame#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReserveSblrAutonomousFrame(c, a1, a2); }); }, true},
{"sblr_autonomous_frame", "FinalizeSblrAutonomousFrame#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; bool a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::FinalizeSblrAutonomousFrame(c, a1, a2, a3); }); }, true},
{"sblr_autonomous_frame", "RecoverSblrAutonomousFrameCoordinator#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrAutonomousFrameCoordinator(c); }); }, true},
{"sblr_catalog_epoch_check_journal", "LookupSblrCatalogEpochCheckJournalV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrCatalogEpochCheckJournalKeyV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LookupSblrCatalogEpochCheckJournalV1(c, a1); }); }, true},
{"sblr_catalog_epoch_check_journal", "EnsureSblrCatalogEpochCheckJournalV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrCatalogEpochCheckJournalKeyV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnsureSblrCatalogEpochCheckJournalV1(c, a1); }); }, true},
{"sblr_catalog_epoch_check_journal", "PublishSblrCatalogEpochCheckJournalResultV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrCatalogEpochCheckJournalKeyV1> a1{}; std::remove_cvref_t<std::vector<unsigned char, std::allocator<unsigned char> >> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrCatalogEpochCheckJournalResultV1(c, a1, a2); }); }, true},
{"sblr_cursor_open", "CompileAndPublishSblrExecutablePlanReceipt#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned char a3{}; unsigned char a4{}; unsigned int a5{}; unsigned long a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileAndPublishSblrExecutablePlanReceipt(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_cursor_open", "OpenSblrCursor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::OpenSblrCursor(c, a1, a2, a3, a4); }); }, true},
{"sblr_cursor_open", "RecoverSblrOpenCursors#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrOpenCursors(c); }); }, true},
{"sblr_cursor_open", "FetchSblrCursor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned long a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; unsigned long a5{}; unsigned int a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::FetchSblrCursor(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_cursor_open", "CloseSblrCursor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned long a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; unsigned long a5{}; unsigned char a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CloseSblrCursor(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_database_attach_journal", "LookupSblrDatabaseAttachJournalV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrDatabaseAttachJournalKeyV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LookupSblrDatabaseAttachJournalV1(c, a1); }); }, true},
{"sblr_database_attach_journal", "EnsureSblrDatabaseAttachJournalV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrDatabaseAttachJournalKeyV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnsureSblrDatabaseAttachJournalV1(c, a1); }); }, true},
{"sblr_database_attach_journal", "PublishSblrDatabaseAttachJournalResultV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrDatabaseAttachJournalKeyV1> a1{}; std::remove_cvref_t<std::vector<unsigned char, std::allocator<unsigned char> >> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrDatabaseAttachJournalResultV1(c, a1, a2); }); }, true},
{"sblr_error_vector_registry", "IssueSblrErrorVectorDescriptorV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::array<unsigned char, 16ul>> a1{}; std::remove_cvref_t<std::array<unsigned char, 16ul>> a2{}; unsigned long a3{}; std::remove_cvref_t<std::array<unsigned char, 16ul>> a4{}; unsigned long a5{}; std::vector<scratchbird::engine::sblr::SblrErrorVectorEntryV1, std::allocator<scratchbird::engine::sblr::SblrErrorVectorEntryV1> > a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::IssueSblrErrorVectorDescriptorV1(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_error_vector_registry", "LookupSblrErrorVectorDescriptorV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::array<unsigned char, 16ul>> a1{}; std::remove_cvref_t<std::array<unsigned char, 16ul>> a2{}; unsigned long a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LookupSblrErrorVectorDescriptorV1(c, a1, a2, a3); }); }, true},
{"sblr_error_vector_registry", "RevokeSblrErrorVectorDescriptorsV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<std::array<unsigned char, 16ul>> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RevokeSblrErrorVectorDescriptorsV1(c, a1); }); }, true},
{"sblr_error_vector_registry", "RecoverSblrErrorVectorDescriptorRegistryV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrErrorVectorDescriptorRegistryV1(c); }); }, true},
{"sblr_optimizer_stats_epoch", "InspectSblrOptimizerStatsEpochV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::InspectSblrOptimizerStatsEpochV1(c); }); }, true},
{"sblr_optimizer_stats_epoch", "PublishSblrOptimizerStatsDropV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrOptimizerStatsDropDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrOptimizerStatsDropV1(c, a1); }); }, true},
{"sblr_parameter_set", "IssueSblrParameterSet#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrParameterSetIssueRequest> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::IssueSblrParameterSet(c, a1); }); }, true},
{"sblr_parameter_set", "LoadSblrParameterSet#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadSblrParameterSet(c, a1); }); }, true},
{"sblr_parameter_set", "PublishSblrParameterBinding#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrParameterSetSnapshot> a1{}; std::remove_cvref_t<scratchbird::engine::internal_api::SblrParameterBindPublicationRequest> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrParameterBinding(c, a1, a2); }); }, true},
{"sblr_parameter_set", "LoadSblrParameterBinding#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LoadSblrParameterBinding(c, a1); }); }, true},
{"sblr_parameter_set", "InvalidateSblrParameterSet#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::InvalidateSblrParameterSet(c, a1, a2, a3, a4); }); }, true},
{"sblr_parameter_set", "RevalidateSblrParameterSet#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrParameterSetSnapshot> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a4{}; unsigned long a5{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a6{}; unsigned long a7{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a8{}; unsigned long a9{}; scratchbird::engine::internal_api::SblrParameterSetSnapshot* a10 = nullptr; return ProbeOf([&]{ return scratchbird::engine::internal_api::RevalidateSblrParameterSet(c, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10); }); }, false},
{"sblr_parse_text_journal", "LookupSblrParseTextJournalV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrParseTextJournalKeyV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LookupSblrParseTextJournalV1(c, a1); }); }, true},
{"sblr_parse_text_journal", "EnsureSblrParseTextJournalV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrParseTextJournalKeyV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnsureSblrParseTextJournalV1(c, a1); }); }, true},
{"sblr_parse_text_journal", "PublishSblrParseTextJournalResultV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::SblrParseTextJournalKeyV1> a1{}; std::remove_cvref_t<std::vector<unsigned char, std::allocator<unsigned char> >> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrParseTextJournalResultV1(c, a1, a2); }); }, true},
{"sblr_prepared_coordination", "BeginSblrPreparedCoordination#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::BeginSblrPreparedCoordination(c, a1); }); }, true},
{"sblr_prepared_coordination", "BeginSblrPreparedExecutionCoordination#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::BeginSblrPreparedExecutionCoordination(c, a1, a2); }); }, true},
{"sblr_prepared_coordination", "AcquireSblrPreparedCoordination#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AcquireSblrPreparedCoordination(c, a1, a2, a3); }); }, true},
{"sblr_prepared_coordination", "SealSblrPreparedCoordination#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a4{}; unsigned long a5{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::SealSblrPreparedCoordination(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_prepared_coordination", "RevokeSblrPreparedCoordination#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RevokeSblrPreparedCoordination(c, a1, a2, a3, a4); }); }, true},
{"sblr_prepared_coordination", "RecoverSblrPreparedCoordinationRegistry#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrPreparedCoordinationRegistry(c); }); }, true},
{"sblr_read_by_key", "CompileSblrReadByKeyDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned int a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileSblrReadByKeyDescriptor(c, a1, a2, a3, a4); }); }, true},
{"sblr_read_by_key", "ConsumeSblrReadByKeyDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrReadByKeyDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ConsumeSblrReadByKeyDescriptor(c, a1); }); }, true},
{"sblr_read_by_key", "RecoverSblrReadByKeyDescriptors#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrReadByKeyDescriptors(c); }); }, true},
{"sblr_read_range", "CompileSblrReadRangeDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned int a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileSblrReadRangeDescriptor(c, a1, a2, a3, a4); }); }, true},
{"sblr_read_range", "ConsumeSblrReadRangeDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrReadRangeDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ConsumeSblrReadRangeDescriptor(c, a1); }); }, true},
{"sblr_read_range", "RecoverSblrReadRangeDescriptors#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrReadRangeDescriptors(c); }); }, true},
{"sblr_read_stream", "CompileSblrReadStreamDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned int a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileSblrReadStreamDescriptor(c, a1, a2, a3, a4); }); }, true},
{"sblr_read_stream", "ConsumeSblrReadStreamDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrReadStreamDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ConsumeSblrReadStreamDescriptor(c, a1); }); }, true},
{"sblr_read_stream", "RecoverSblrReadStreamDescriptors#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrReadStreamDescriptors(c); }); }, true},
{"sblr_relation_reservation", "PublishSblrRelationReservation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned char a3{}; unsigned long a4{}; unsigned long a5{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrRelationReservation(c, a1, a2, a3, a4, a5); }); }, true},
{"sblr_relation_reservation", "CompileAndPublishSblrRelationReservation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileAndPublishSblrRelationReservation(c, a1, a2); }); }, true},
{"sblr_relation_reservation", "ReleaseSblrRelationReservation#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; unsigned long a5{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReleaseSblrRelationReservation(c, a1, a2, a3, a4, a5); }); }, true},
{"sblr_relation_reservation", "RecoverSblrRelationReservations#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrRelationReservations(c); }); }, true},
{"sblr_result_set_pass", "CompileSblrResultSetPassDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned int a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CompileSblrResultSetPassDescriptor(c, a1, a2, a3, a4); }); }, true},
{"sblr_result_set_pass", "ConsumeSblrResultSetPassDescriptor#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::sblr::SblrResultSetPassDescriptorV1> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ConsumeSblrResultSetPassDescriptor(c, a1); }); }, true},
{"sblr_result_set_pass", "RecoverSblrResultSetPassDescriptors#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrResultSetPassDescriptors(c); }); }, true},
{"sblr_savepoint_coordinator", "ReserveSblrSavepoint#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a2{}; unsigned long a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReserveSblrSavepoint(c, a1, a2, a3, a4); }); }, true},
{"sblr_savepoint_coordinator", "ActivateSblrSavepoint#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; unsigned long a5{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ActivateSblrSavepoint(c, a1, a2, a3, a4, a5); }); }, true},
{"sblr_savepoint_coordinator", "ReleaseSblrSavepoint#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned long a3{}; unsigned long a4{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a5{}; unsigned long a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::ReleaseSblrSavepoint(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_savepoint_coordinator", "RollbackToSblrSavepoint#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned long a3{}; unsigned long a4{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a5{}; unsigned long a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RollbackToSblrSavepoint(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_savepoint_coordinator", "RecoverSblrSavepointCoordinator#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrSavepointCoordinator(c); }); }, true},
{"sblr_source_map_registry", "IssueSblrSourceMapDescriptorV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<std::array<unsigned char, 32ul>> a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; unsigned long a4{}; std::vector<scratchbird::engine::sblr::SblrSourceMapEntryV1, std::allocator<scratchbird::engine::sblr::SblrSourceMapEntryV1> > a5{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::IssueSblrSourceMapDescriptorV1(c, a1, a2, a3, a4, a5); }); }, true},
{"sblr_source_map_registry", "LookupSblrSourceMapDescriptorV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<std::array<unsigned char, 32ul>> a4{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a5{}; unsigned long a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::LookupSblrSourceMapDescriptorV1(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_source_map_registry", "RevokeSblrSourceMapDescriptorsV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RevokeSblrSourceMapDescriptorsV1(c, a1); }); }, true},
{"sblr_source_map_registry", "RecoverSblrSourceMapDescriptorRegistryV1#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrSourceMapDescriptorRegistryV1(c); }); }, true},
{"sblr_temporary_instance", "PublishSblrTemporaryInstance#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; unsigned char a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrTemporaryInstance(c, a1, a2, a3); }); }, true},
{"sblr_temporary_instance", "CleanupSblrTemporaryInstance#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a3{}; unsigned long a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CleanupSblrTemporaryInstance(c, a1, a2, a3, a4); }); }, true},
{"sblr_temporary_instance", "RecoverSblrTemporaryInstances#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrTemporaryInstances(c); }); }, true},
{"sblr_variable_registry", "PublishSblrVariableFrame#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a4{}; unsigned long a5{}; std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::SblrVariableDemand, std::allocator<scratchbird::engine::internal_api::SblrVariableDemand> >> a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::PublishSblrVariableFrame(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_variable_registry", "AssignSblrVariable#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a4{}; unsigned long a5{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a6{}; unsigned long a7{}; unsigned long a8{}; scratchbird::engine::internal_api::SblrVariableValueState a9{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a10{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AssignSblrVariable(c, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10); }); }, true},
{"sblr_variable_registry", "AssignSblrVariableBatch#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a4{}; unsigned long a5{}; std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::SblrVariableAssignment, std::allocator<scratchbird::engine::internal_api::SblrVariableAssignment> >> a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AssignSblrVariableBatch(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_variable_registry", "RevokeSblrVariableFrame#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a4{}; unsigned long a5{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::RevokeSblrVariableFrame(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_variable_registry", "RecoverSblrVariableDescriptorRegistry#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrVariableDescriptorRegistry(c); }); }, true},
{"sblr_variable_frame_coordinator", "BeginSblrVariableFrame#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; unsigned long a2{}; std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::SblrVariableFrameDemand, std::allocator<scratchbird::engine::internal_api::SblrVariableFrameDemand> >> a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::BeginSblrVariableFrame(c, a1, a2, a3); }); }, true},
{"sblr_variable_frame_coordinator", "AcquireSblrVariableFrame#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AcquireSblrVariableFrame(c, a1, a2, a3); }); }, true},
{"sblr_variable_frame_coordinator", "AssignSblrVariableFrameValues#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a3{}; unsigned long a4{}; unsigned long a5{}; std::remove_cvref_t<std::vector<scratchbird::engine::internal_api::SblrVariableAssignment, std::allocator<scratchbird::engine::internal_api::SblrVariableAssignment> >> a6{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::AssignSblrVariableFrameValues(c, a1, a2, a3, a4, a5, a6); }); }, true},
{"sblr_variable_frame_coordinator", "CloseSblrVariableFrame#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::core::platform::Uuid> a1{}; std::remove_cvref_t<scratchbird::core::platform::Uuid> a2{}; unsigned long a3{}; std::remove_cvref_t<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >> a4{}; return ProbeOf([&]{ return scratchbird::engine::internal_api::CloseSblrVariableFrame(c, a1, a2, a3, a4); }); }, true},
{"sblr_variable_frame_coordinator", "RecoverSblrVariableFrameCoordinator#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RecoverSblrVariableFrameCoordinator(c); }); }, true},
{"api_events", "EngineCreateIdentity#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCreateIdentityRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCreateIdentity(a0); }); }, true},
{"protected_material_catalog", "EngineCreateProtectedMaterial#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineCreateProtectedMaterialRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineCreateProtectedMaterial(a0); }); }, true},
{"protected_material_catalog", "EngineAddProtectedMaterialVersion#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineAddProtectedMaterialVersionRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineAddProtectedMaterialVersion(a0); }); }, true},
{"protected_material_catalog", "EngineResolveProtectedMaterial#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineResolveProtectedMaterialRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineResolveProtectedMaterial(a0); }); }, true},
{"protected_material_catalog", "EngineReleaseProtectedMaterial#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineReleaseProtectedMaterialRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineReleaseProtectedMaterial(a0); }); }, true},
{"protected_material_catalog", "EnginePurgeProtectedMaterialVersion#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EnginePurgeProtectedMaterialVersionRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EnginePurgeProtectedMaterialVersion(a0); }); }, true},
{"protected_material_catalog", "EngineInspectProtectedMaterialCatalog#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineInspectProtectedMaterialCatalogRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineInspectProtectedMaterialCatalog(a0); }); }, true},
{"protected_material_catalog", "EngineExportProtectedMaterialPackage#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineExportProtectedMaterialPackageRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineExportProtectedMaterialPackage(a0); }); }, true},
{"protected_material_catalog", "EngineImportProtectedMaterialPackage#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe { std::remove_cvref_t<scratchbird::engine::internal_api::EngineImportProtectedMaterialPackageRequest> a0{}; a0.context = c; return ProbeOf([&]{ return scratchbird::engine::internal_api::EngineImportProtectedMaterialPackage(a0); }); }, true},
{"mga_transaction_publication", "RunLocalCommitPageBarrier#0", [](const scratchbird::engine::internal_api::EngineRequestContext& c) -> FamilyProbe {  return ProbeOf([&]{ return scratchbird::engine::internal_api::RunLocalCommitPageBarrier(c); }); }, true},

    {"mga_delete_operations", "MgaDmlDeleteDurableStoreV1::Open",
     [](const api::EngineRequestContext& c) -> FamilyProbe {
       api::EngineApiDiagnostic d;
       auto store = api::MgaDmlDeleteDurableStoreV1::Open(c, {}, 0, &d);
       auto p = FromDiagnostic(d);
       if (store) p.code = "OPENED";
       return p;
     }, true},
#define SB_HOT(ID, EXPR) {"mga_row_versions", "MgaRelationHotAppendContext::" ID, [](const api::EngineRequestContext& c) -> FamilyProbe { api::MgaRelationHotAppendContext h(c); return ProbeOf([&] { return h.EXPR; }); }, false},
    SB_HOT("PrepareRowVersionSequences", PrepareRowVersionSequences({}))
    SB_HOT("AppendRowVersions#2", AppendRowVersions(nullptr, nullptr))
    SB_HOT("AppendRowVersions#3", AppendRowVersions(nullptr, nullptr, nullptr))
    SB_HOT("AppendRowVersionsReadOnly#1", AppendRowVersionsReadOnly({}))
    SB_HOT("AppendRowVersionsReadOnly#2", AppendRowVersionsReadOnly({}, nullptr))
    SB_HOT("AppendRowVersionsReadOnlyScopedOnly", AppendRowVersionsReadOnlyScopedOnly({}, nullptr))
    SB_HOT("AppendRowVersionsReadOnlyScopedOnlyTyped", AppendRowVersionsReadOnlyScopedOnlyTyped({}, {}, {}))
    SB_HOT("AppendRowVersionsReadOnlyScopedOnlyCanonical", AppendRowVersionsReadOnlyScopedOnlyCanonical({}, {}, {}, {}))
    SB_HOT("AppendRowVersionIdentitiesReadOnlyScopedOnlyTyped", AppendRowVersionIdentitiesReadOnlyScopedOnlyTyped({}, {}, {}, {}, {}))
    SB_HOT("AppendRowVersionIdentitiesReadOnlyScopedOnlyNativePacket", AppendRowVersionIdentitiesReadOnlyScopedOnlyNativePacket({}, {}, {}, {}))
    SB_HOT("AppendIndexEntryBatches", AppendIndexEntryBatches({}))
    SB_HOT("AppendExactIndexEntryBatches", AppendExactIndexEntryBatches({}))
    SB_HOT("FlushIndexEntries", FlushIndexEntries())
    SB_HOT("Flush", Flush())
#undef SB_HOT
    // A request that PASSES the entry's own validation (tag, engine-owned snapshot, identities, mode, generations): without the guard it
    // reaches publish(), which opens "<database>.sb.sblr_relation_reservation.v2" for append and so CREATES a file next to the database.
    // This is the entry the mutation check removes the guard from: the refusal code, the directory listing and the ".sb." scan must all fire.
    {"sblr_relation_reservation", "PublishSblrRelationReservation#valid_request",
     [](const api::EngineRequestContext& c) -> FamilyProbe {
       api::EngineRequestContext k = c;
       k.trace_tags.push_back("private_transaction_relation_reservation");
       k.statement_metadata_snapshot_engine_owned = true;
       k.security_context_present = true;
       k.statement_uuid = NewId();
       if (k.transaction_uuid.is_nil()) k.transaction_uuid = NewId();
       return ProbeOf([&] { return api::PublishSblrRelationReservation(k, k.statement_uuid, NewId(), 1, 1, 1); });
     }, false},

  };
  return table;
}

// The context prepared_statement_registry_context (engine/public_abi.cpp) builds from a session: path, served owner, the handle's native
// marker, database, principal, session, and the registry trace tag. Rebuilt here from the same values.
api::EngineRequestContext PreparedLikeContext(const Approved& a) {
  const auto& rec = *a.record;
  api::EngineRequestContext context;
  context.database_path = rec.database_path;
  context.served_route_owner = rec.served_route_owner;
  context.native_served_facts = rec.native_served_facts;
  context.database_uuid = rec.native_served_facts->database_uuid;
  context.principal_uuid = api::EngineUuid{rec.effective_user_uuid};
  context.session_uuid = api::EngineUuid{rec.session_uuid};
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_prepared_statement_registry");
  return context;
}

// Entries that cannot carry a diagnostic (bool, integers, optional, vector, plain records): they return the empty answer and touch no
// file. The list is pinned: a NEW entry that cannot carry a diagnostic must be added here on purpose.
const std::set<std::string>& NoDiagnosticIds() {
  static const std::set<std::string> ids = {
    "AppendDeferredEventSequenceAllocatorLines#0",
    "ApplyDmlUpdateBinarySavepointRecordsForStoreModule#0",
    "CurrentMgaSavepointAuthorityGeneration#0",
    "DeleteMgaDmlUpdateDurableOperationForTestingV1#0",
    "DocumentPathProviderIdentityForContext#0",
    "DomainChainContainsUuid#0",
    "EngineDocumentProviderCleanup#0",
    "FilterMgaRelationMetadataForStoreModule#0",
    "FilterVisibleRetiredTemporaryMetadataForStoreModule#0",
    "FindVisibleApiBehaviorRecord#0",
    "FindVisibleDomain#0",
    "ListNoSqlProviderGenerations#0",
    "LoadDecodedScopedRowsForTable#0",
    "LoadDecodedScopedRowsForTableBounded#0",
    "LoadDescriptorFieldsByRelation#0",
    "LoadDescriptorFieldsSnapshot#0",
    "NextIndexEventSequence#0",
    "NextMetadataEventSequence#0",
    "NextRowEventSequence#0",
    "ParseSavepointsBounded#0",
    "QuarantineMgaDmlUpdateDurableOperationForTestingV1#0",
    "ScanNextIndexEventSequence#0",
    "ScanNextMetadataEventSequence#0",
    "ScanNextRowEventSequence#0",
    "TruncateMgaDmlUpdateDurableExtentForTestingV1#0",
    "UpdateScopedRelationSummariesForStoreModule#0",
    "VisibleApiBehaviorRecords#0"
  };
  return ids;
}

int FamilyGuardState(const std::string& state, const Offline& offline) {
  const bool trace = state == "trace";
  const bool compare = !trace;  // the strace state compares once around the whole region
  const std::uint64_t g0 = offline.catalog_generation;
  Check(g0 != 0, "the created file's names carry a catalog generation");
  Check(Listing(served.directory).size() == 1 && Listing(served.directory).count(served.path.filename().string()) == 1,
        "the database directory holds only the data file before the families run");

  if (trace) Mark("approve");
  auto s1 = Approve();
  auto ps1 = OpenPublicFor(*s1);
  auto c1 = ContextFor(*s1, g0);
  auto r1 = Acquire(*ps1, c1, s1->approval.transaction_uuid, compare);
  Check(r1.ok(), "a receipt is acquired for an approved session's active transaction: " + r1.code);
  // The two native contexts under test.
  const api::EngineRequestContext copied = CopyContext(r1);  // (A) what the server copies at its 8 call sites
  Check(copied.native_served_facts == served.facts, "(A) the copied context carries the native facts");
  const api::EngineRequestContext prepared = PreparedLikeContext(*s1);  // (B) built the way prepared_statement_registry_context builds it
  Check(prepared.native_served_facts == served.facts, "(B) the prepared-statement-style context carries the native facts");
  std::vector<p::byte> image_before;
  if (trace) {
    Mark("prepare");
    image_before = Image(served.path);
  }
  const auto listing_before = Listing(served.directory);
  const auto next_before = off::ReadFile(served.path, served.facts->page_size_bytes).counters.next_transaction;
  if (trace) Mark("families");

  // The entry whose unguarded call CREATES a file (see fg_manual): checked FIRST and with the file evidence before the code, so a removed
  // guard is reported as "a file appeared" (and by the strace / ".sb." scans), not only as a wrong code.
  for (const auto& entry : FamilyTable()) {
    if (std::string_view(entry.id) != "PublishSblrRelationReservation#valid_request") continue;
    for (const auto* context : {&copied, &prepared}) {
      const auto pre = Before(compare);
      const auto probe = entry.call(*context);
      if (compare) {
        Check(Image(served.path) == pre.image, "the create-capable entry wrote nothing to the data file");
        Check(Listing(served.directory) == pre.listing, "the create-capable entry created no file or directory entry next to the database");
        Check(!AnySideFile(served.directory.parent_path()), "the create-capable entry left no .sb. file");
      }
      Check(probe.carried && probe.code == kRefusal && probe.family == entry.family, "the create-capable entry is refused with the family refusal (got " + probe.code + ")");
    }
  }
  std::map<std::string, std::size_t> per_family;
  std::size_t calls = 0, carried = 0, empty_answers = 0;
  std::string previous_family;
  Observation before;
  const auto close_family = [&](const std::string& family) {
    if (family.empty() || !compare) return;
    Check(Image(served.path) == before.image, "family " + family + ": the file's bytes are identical");
    Check(Listing(served.directory) == before.listing, "family " + family + ": no file or directory entry appeared or changed");
  };
  for (const auto& entry : FamilyTable()) {
    if (previous_family != entry.family) {
      close_family(previous_family);
      before = Before(compare);
      previous_family = entry.family;
    }
    for (const auto* context : {&copied, &prepared}) {
      const auto probe = entry.call(*context);
      ++calls;
      ++per_family[entry.family];
      const bool no_diagnostic = NoDiagnosticIds().count(entry.id) != 0;
      if (no_diagnostic) {
        Check(!probe.carried, std::string(entry.id) + ": pinned as an entry that cannot carry a diagnostic, and it does not");
        ++empty_answers;
      } else if (std::getenv("SB_FGUARD_DISCOVER") != nullptr && !probe.carried) {
        std::printf("NODIAG %s\n", entry.id);  // discovery aid for pinning NoDiagnosticIds(); never set in ctest
      } else {
        Check(probe.carried, std::string(entry.id) + ": the entry returns a diagnostic");
        Check(probe.code == kRefusal, std::string(entry.id) + ": exact code " + kRefusal + " (got " + probe.code + ")");
        Check(probe.family == entry.family, std::string(entry.id) + ": registry_family = " + entry.family + " (got " + probe.family + ")");
        ++carried;
      }
    }
  }
  close_family(previous_family);

  if (trace) Mark("verify");
  Release(r1);
  if (trace) Check(Image(served.path) == image_before, "the file's bytes are identical after every family");
  Check(Listing(served.directory) == listing_before, "no file or directory entry appeared during the families");
  Check(!AnySideFile(served.directory.parent_path()), "no file named .sb. exists anywhere under the test's temp tree");
  Check(off::ReadFile(served.path, served.facts->page_size_bytes).counters.next_transaction == next_before,
        "the file's Next transaction is unchanged by the families");

  // ---- the legacy sample: a context with NO native facts is never given the native refusal (the guard changes nothing for it). The path is
  // a database that does not exist, in a directory of this test's own (outside the native database's tree); the families answer with
  // their own ordinary validation. An entry with a raw-pointer argument is exempt (default arguments would dereference it). The legacy sample
  // does not run in the strace state (a legacy context legitimately opens ".sb." files in its own directory).
  if (!trace) {
    scratchbird::tests::OwnedTempDirectory legacy_dir;
    api::EngineRequestContext legacy = ContextFor(((legacy_dir.path() / "legacy.sdb")).string(), s1->record->effective_user_uuid,
                                                  s1->record->session_uuid, s1->approval.transaction_uuid, s1->approval.transaction_order, g0);
    Check(!legacy.native_served_facts, "the legacy context carries no native facts");
    std::size_t legacy_calls = 0;
    for (const auto& entry : FamilyTable()) {
      if (!entry.legacy_ok) continue;
      const auto probe = entry.call(legacy);
      ++legacy_calls;
      Check(probe.code != kRefusal, std::string(entry.id) + ": a LEGACY context is not given the native refusal");
    }
    Check(legacy_calls > 100, "the legacy sample ran a meaningful number of entries");
    std::printf("FGUARD legacy_entries=%zu\n", legacy_calls);
    legacy_dir.Cleanup();
  }
  if (trace) Mark("disconnect");
  ps1.reset();
  Disconnect(*s1);
  std::printf("FGUARD entries=%zu calls=%zu families=%zu carried=%zu empty_answers=%zu\n", FamilyTable().size(), calls, per_family.size(), carried, empty_answers);
  return 0;
}


int ServeMode(const fs::path& path, const std::string& state) {
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !mem::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() || !crypto.Start(config, policy).ok()) {
    std::fprintf(stderr, "process setup failed\n");
    return 3;
  }
  config.database_default_path = path;
  served.path = path;
  served.directory = path.parent_path();
  const auto offline = ReadOffline(path);
  served.started = srv::StartHostedEngine(config, nullptr, &crypto);
  Check(served.started.ok(), "the server serves the file: " + (served.started.diagnostics.empty() ? std::string() : served.started.diagnostics.front().code));
  Check(!served.started.state.database_runtimes.empty() && served.started.state.database_runtimes.front() &&
            served.started.state.database_runtimes.front()->native_loop,
        "the hosted state holds the native loop");
  served.loop = served.started.state.database_runtimes.front()->native_loop.get();
  served.facts = srv::ServedNativeFactsFor(served.started.state, path.string());
  Check(served.facts != nullptr && served.facts->receipt_source != nullptr, "the loop published the facts with a receipt source");
  served.database = served.facts->database_uuid;
  srv::NativeLoginThrottle().Clear();
  const int rc = FamilyGuardState(state, offline);
  if (state == "trace") Mark("end");
  (void)served.loop->Close();
  served.facts.reset();
  served.started = {};
  if (rc == 0) std::cout << "FGUARD_OK checks=" << checks << " state=" << state << std::endl;
  return rc;
}

std::string CaptureOutput(const std::string& command, int* status) {
  std::string output;
  FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
  Check(pipe != nullptr, "popen");
  char chunk[512];
  std::size_t got;
  while ((got = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) output.append(chunk, got);
  const int raw = ::pclose(pipe);
  *status = raw == -1 ? -1 : (WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw));
  return output;
}
std::string Field(const std::string& text, std::string_view key) {
  const auto at = text.find(std::string(key) + "=");
  if (at == std::string::npos) return {};
  const auto from = at + key.size() + 1;
  const auto to = text.find_first_of(" \n", from);
  return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

void RunSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto dir = root / ("fguard" + std::to_string(page_size));
  fs::create_directory(dir);
  const auto path = dir / "gate.sdb";
  int status = 0;
  const std::string material = SB_NATIVE_LOGIN_MATERIAL_TEST_BIN;
  const auto created = CaptureOutput("'" + material + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
  Check(status == 0 && created.starts_with("CREATED "), tag + "create: " + created);
  const auto start = std::chrono::steady_clock::now();
  const auto served_out = CaptureOutput("'" + self + "' serve '" + path.string() + "' fguard " + Field(created, "database") + " " + Field(created, "bootstrap") + " " +
                                            Field(created, "service"),
                                        &status);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  Check(status == 0 && served_out.find("FGUARD_OK") != std::string::npos, tag + "serve child: " + served_out.substr(0, 3000));
  Check(served_out.find(kBootstrapPassword) == std::string::npos && created.find(kBootstrapPassword) == std::string::npos, tag + "no test password in any child output");
  Check(!AnySideFile(root), tag + "no file named .sb. under the driver's temp tree");
  const auto summary = served_out.find("FGUARD entries");
  std::printf("%sfguard: %.1f s: %s", tag.c_str(), seconds, summary == std::string::npos ? "\n" : served_out.substr(summary, served_out.find('\n', summary) - summary + 1).c_str());
  fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (argc == 3) RunSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])));
    else
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u}) RunSize(self, artifacts.path(), size);
    artifacts.Cleanup();
    std::printf("native_family_guards: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
