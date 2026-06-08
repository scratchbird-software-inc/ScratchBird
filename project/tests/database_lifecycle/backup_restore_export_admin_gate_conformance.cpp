// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "backup_archive/backup_archive_api.hpp"
#include "database_lifecycle.hpp"
#include "maintenance_coordinator.hpp"
#include "manager_control.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sbps.hpp"
#include "server_observability.hpp"
#include "uuid.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kDatabaseUuid = "019e3900-0000-7000-8000-000000000057";
constexpr std::string_view kFilespaceUuid = "019e3900-0000-7000-8000-000000000058";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_cbq057_backup_restore_export_admin.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for CBQ-057 gate");
  return std::filesystem::path(made);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  Require(static_cast<bool>(out), "file write failed");
}

std::uint64_t Fnv1a64(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string HexEncode(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() * 2);
  for (unsigned char c : text) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

std::string EncodePairs(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string encoded;
  for (const auto& [key, value] : fields) {
    if (!encoded.empty()) { encoded.push_back('|'); }
    encoded += HexEncode(key);
    encoded.push_back('=');
    encoded += HexEncode(value);
  }
  return encoded;
}

std::string RecordLine(const std::string& kind,
                       const std::vector<std::pair<std::string, std::string>>& fields) {
  return kind + "\t" + EncodePairs(fields) + "\n";
}

void WriteManifestWithChecksum(const std::filesystem::path& path, const std::string& body) {
  WriteFile(path, body + "CHECKSUM\t" + std::to_string(Fnv1a64(body)) + "\n");
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code_or_detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code_or_detail || diagnostic.detail == code_or_detail) return true;
    if (diagnostic.detail.find(code_or_detail) != std::string::npos) return true;
  }
  return false;
}

bool HasDiagnostic(const api::BackupArchiveLifecycleAdmission& admission,
                   std::string_view code_or_detail) {
  return admission.diagnostic.code == code_or_detail ||
         admission.diagnostic.detail.find(code_or_detail) != std::string::npos;
}

bool HasDiagnostic(const server::ServerManagementResponse& result,
                   std::string_view code_or_detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code_or_detail) return true;
    if (diagnostic.message_key.find(code_or_detail) != std::string::npos) return true;
    if (diagnostic.safe_message.find(code_or_detail) != std::string::npos) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view evidence_id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == evidence_id) return true;
  }
  return false;
}

bool HasEncodedManifestField(std::string_view manifest,
                             std::string_view key,
                             std::string_view value) {
  const std::string encoded = HexEncode(key) + "=" + HexEncode(value);
  return manifest.find(encoded) != std::string_view::npos;
}

api::EngineRequestContext EngineContext(const std::filesystem::path& database_path,
                                        std::uint64_t local_transaction_id = 0) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.security_context_present = true;
  context.local_transaction_id = local_transaction_id;
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

std::vector<std::string> BackupOptions() {
  return {std::string("filespace_uuid:") + std::string(kFilespaceUuid)};
}

std::vector<std::string> RestoreOptions() {
  auto options = BackupOptions();
  options.push_back("restore_inspection_open:true");
  options.push_back("recovery_classification:restore_inspection");
  options.push_back("target_database_open:false");
  return options;
}

void TestBackupRestoreEngineOwnedLifecycle(const std::filesystem::path& temp_dir) {
  const auto source_database = temp_dir / "source.sbdb";
  const auto restored_database = temp_dir / "restored.sbdb";
  const auto manifest = temp_dir / "backup" / "source.sbpb";
  const std::string source_image = "SBDB_IMAGE_UNDER_ENGINE_OWNED_BACKUP_PATH";
  WriteFile(source_database, source_image);

  api::EngineApiRequest lifecycle_request;
  lifecycle_request.context = EngineContext(source_database);
  auto admitted = api::EvaluateBackupArchiveLifecycleAdmission(
      lifecycle_request, api::BackupArchiveLifecycleOperation::physical_backup);
  Require(admitted.admitted && admitted.snapshot_hold_acquired && admitted.filespace_hold_acquired,
          "backup admission did not acquire engine-owned snapshot/filespace holds");

  lifecycle_request.option_envelopes = BackupOptions();
  lifecycle_request.option_envelopes.push_back("live_file_shortcut:true");
  const auto live_file = api::EvaluateBackupArchiveLifecycleAdmission(
      lifecycle_request, api::BackupArchiveLifecycleOperation::physical_backup);
  Require(!live_file.admitted && HasDiagnostic(live_file, "BACKUP_LIVE_FILE_SHORTCUT_FORBIDDEN"),
          "backup admission accepted live-file shortcut");

  lifecycle_request.option_envelopes = RestoreOptions();
  lifecycle_request.option_envelopes.push_back("authoritative_wal:true");
  const auto wal = api::EvaluateBackupArchiveLifecycleAdmission(
      lifecycle_request, api::BackupArchiveLifecycleOperation::delta_package);
  Require(!wal.admitted && HasDiagnostic(wal, "BACKUP_AUTHORITATIVE_WAL_FORBIDDEN"),
          "backup/archive admission accepted authoritative WAL semantics");

  api::EngineStartPhysicalBackupRequest backup;
  backup.context = EngineContext(source_database);
  backup.option_envelopes = BackupOptions();
  backup.option_envelopes.push_back("target_uri:" + manifest.string());
  const auto backup_result = api::EngineStartPhysicalBackup(backup);
  Require(backup_result.ok, "physical backup failed");
  Require(backup_result.image_bytes == source_image.size(), "physical backup image size mismatch");
  Require(HasEvidence(backup_result, "snapshot_hold"), "backup omitted snapshot hold evidence");
  Require(HasEvidence(backup_result, "filespace_hold"), "backup omitted filespace hold evidence");
  Require(HasEvidence(backup_result, "shutdown_blocker"), "backup omitted shutdown blocker evidence");
  Require(HasEvidence(backup_result, "drop_blocker"), "backup omitted drop blocker evidence");
  const auto manifest_text = ReadFile(manifest);
  Require(HasEncodedManifestField(manifest_text, "authoritative_wal", "false"),
          "backup manifest did not publish anti-WAL evidence");
  Require(HasEncodedManifestField(manifest_text, "filespace_uuid", std::string(kFilespaceUuid)),
          "backup manifest omitted filespace coverage proof");
  Require(HasEncodedManifestField(manifest_text, "coverage_contiguous", "true"),
          "backup manifest omitted contiguous coverage proof");

  api::EngineRestorePhysicalBackupRequest restore_without_inspection;
  restore_without_inspection.context = EngineContext(restored_database, 2);
  restore_without_inspection.option_envelopes = BackupOptions();
  restore_without_inspection.option_envelopes.push_back("source_manifest_uri:" + manifest.string());
  const auto refused_restore = api::EngineRestorePhysicalBackup(restore_without_inspection);
  Require(!refused_restore.ok &&
              HasDiagnostic(refused_restore, "RESTORE_INSPECTION_OPEN_REQUIRED"),
          "restore accepted a non-inspection open path");

  api::EngineRestorePhysicalBackupRequest restore;
  restore.context = EngineContext(restored_database, 3);
  restore.option_envelopes = RestoreOptions();
  restore.option_envelopes.push_back("source_manifest_uri:" + manifest.string());
  const auto restore_result = api::EngineRestorePhysicalBackup(restore);
  Require(restore_result.ok, "physical restore failed");
  Require(ReadFile(restored_database) == source_image,
          "physical restore did not install the validated engine-owned image");
  Require(HasEvidence(restore_result, "restore_inspection_open"),
          "restore omitted inspection-open evidence");
  Require(HasEvidence(restore_result, "restore_recovery_classification"),
          "restore omitted recovery-classification evidence");
}

void TestRestoreCoverageRefusal(const std::filesystem::path& temp_dir) {
  const auto restored_database = temp_dir / "missing_filespace_restore.sbdb";
  const auto image = temp_dir / "missing_filespace.image";
  const auto manifest = temp_dir / "missing_filespace.sbpb";
  WriteFile(image, "IMAGE");
  const std::string body =
      "SBPHYSICALBACKUP1\n" +
      RecordLine("META", {{"backup_uuid", "physical-backup-missing-filespace"},
                            {"manifest_version", "1"},
                            {"database_uuid", std::string(kDatabaseUuid)},
                            {"timeline_uuid", "timeline-local"},
                            {"fork_uuid", "fork-primary"},
                            {"key_lineage_id", "key-lineage-local"},
                            {"coverage_start_transaction_id", "0"},
                            {"coverage_end_transaction_id", "0"},
                            {"coverage_contiguous", "true"},
                            {"coverage_proof", "177"},
                            {"checksum_profile", "fnv1a64-manifest-body"},
                            {"signature_profile", "unsigned-local-manifest-proof-v1"},
                            {"finality_source", "local_mga_transaction_inventory"},
                            {"image_uri", image.string()},
                            {"image_bytes", "5"},
                            {"image_checksum", std::to_string(Fnv1a64("IMAGE"))},
                            {"authoritative_wal", "false"},
                            {"format", "physical_snapshot_v1"}});
  WriteManifestWithChecksum(manifest, body);

  api::EngineRestorePhysicalBackupRequest restore;
  restore.context = EngineContext(restored_database, 4);
  restore.option_envelopes = RestoreOptions();
  restore.option_envelopes.push_back("source_manifest_uri:" + manifest.string());
  const auto result = api::EngineRestorePhysicalBackup(restore);
  Require(!result.ok &&
              HasDiagnostic(result, "RESTORE_MANIFEST_COVERAGE_FIELD_MISSING:filespace_uuid"),
          "restore accepted a manifest with missing filespace coverage proof");
}

sblr::SblrDispatchResult DispatchEncoded(std::string operation_id,
                                         std::string opcode,
                                         api::EngineRequestContext context,
                                         api::EngineApiRequest api_request = {},
                                         bool requires_transaction = false) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "CBQ-057");
  envelope.requires_transaction_context = requires_transaction;
  const auto encoded = sblr::EncodeSblrEnvelope(envelope);
  return sblr::DecodeAndDispatchSblrOperation(encoded, std::move(context), std::move(api_request));
}

void TestSblrExportAndSupportBundleRoutes(const std::filesystem::path& temp_dir) {
  const auto database_path = temp_dir / "catalog_export.sbdb";
  WriteFile(database_path, "SBDB_CATALOG_EXPORT_ROUTE");

  auto export_context = EngineContext(database_path, 100);
  api::EngineApiRequest export_request;
  export_request.context = export_context;
  const auto export_result = DispatchEncoded("artifact.export_catalog",
                                            "SBLR_ARTIFACT_EXPORT_CATALOG",
                                            export_context,
                                            export_request,
                                            true);
  Require(export_result.accepted && export_result.envelope_validated &&
              export_result.dispatched_to_api && export_result.api_result.ok,
          "encoded SBLR catalog export route failed");
  Require(HasEvidence(export_result.api_result,
                      "catalog_artifact_format",
                      "sb.catalog.artifact.v1"),
          "catalog export omitted artifact format evidence");
  Require(HasEvidence(export_result.api_result, "git_runtime_authority", "false"),
          "catalog export did not reject source-tree/runtime authority");

  auto no_transaction = DispatchEncoded("artifact.export_catalog",
                                        "SBLR_ARTIFACT_EXPORT_CATALOG",
                                        EngineContext(database_path),
                                        {},
                                        false);
  Require(!no_transaction.api_result.ok &&
              HasDiagnostic(no_transaction.api_result,
                            "artifact.export_catalog:local_transaction_id_required"),
          "catalog export accepted missing transaction authority");

  api::EngineApiRequest support_request;
  support_request.context = EngineContext(database_path);
  support_request.option_envelopes.push_back("engine_authorized_support_export:true");
  const auto support_result = DispatchEncoded("management.prepare_support_bundle",
                                             "SBLR_MANAGEMENT_PREPARE_SUPPORT_BUNDLE",
                                             support_request.context,
                                             support_request);
  Require(support_result.accepted && support_result.api_result.ok,
          "encoded SBLR support-bundle preparation route failed");
  Require(HasEvidence(support_result.api_result,
                      "support_bundle_authority",
                      "engine.authorization.management.SUPPORT_EXPORT"),
          "support bundle route omitted engine authorization evidence");
  Require(HasEvidence(support_result.api_result,
                      "supportability_flush",
                      "required_before_export"),
          "support bundle route omitted flush-before-export evidence");

  api::EngineApiRequest missing_authority;
  missing_authority.context = EngineContext(database_path);
  const auto missing_authority_result = DispatchEncoded("management.prepare_support_bundle",
                                                       "SBLR_MANAGEMENT_PREPARE_SUPPORT_BUNDLE",
                                                       missing_authority.context,
                                                       missing_authority);
  Require(!missing_authority_result.api_result.ok &&
              HasDiagnostic(missing_authority_result.api_result,
                            "OPS.SUPPORT_BUNDLE.ENGINE_AUTHORIZATION_REQUIRED"),
          "support bundle route accepted missing engine authorization");

  api::EngineApiRequest protected_material;
  protected_material.context = EngineContext(database_path);
  protected_material.option_envelopes.push_back("engine_authorized_support_export:true");
  protected_material.option_envelopes.push_back("include_protected_material:true");
  const auto protected_result = DispatchEncoded("management.prepare_support_bundle",
                                               "SBLR_MANAGEMENT_PREPARE_SUPPORT_BUNDLE",
                                               protected_material.context,
                                               protected_material);
  Require(!protected_result.api_result.ok &&
              HasDiagnostic(protected_result.api_result,
                            "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN"),
          "support bundle route accepted protected material export");
}

struct DatabaseFixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string filespace_uuid;
};

DatabaseFixture CreateCleanDatabase(const std::filesystem::path& path, std::uint64_t now_millis) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now_millis).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now_millis + 1).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now_millis + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "database create failed for CBQ-057 gate");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "first open failed for CBQ-057 gate");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "clean shutdown mark failed for CBQ-057 gate");
  return {path,
          uuid::UuidToString(create.database_uuid.value),
          uuid::UuidToString(create.filespace_uuid.value)};
}

server::HostedEngineState EngineState(const DatabaseFixture& fixture) {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_path = fixture.path.string();
  database.database_uuid = fixture.database_uuid;
  database.database_open = true;
  database.write_admission_fenced = false;
  state.databases.push_back(std::move(database));
  return state;
}

std::array<std::uint8_t, 16> AddSession(server::ServerSessionRegistry* registry,
                                        const std::filesystem::path& path,
                                        std::string database_uuid,
                                        std::string_view principal,
                                        std::uint64_t local_transaction_id = 0) {
  server::ServerSessionRecord session;
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_claim = std::string(principal);
  session.database_path = path.string();
  session.database_uuid = std::move(database_uuid);
  session.effective_user_uuid = sbps::MakeUuidV7Bytes();
  session.local_transaction_id = local_transaction_id;
  registry->sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;
  registry->auth_contexts_by_uuid[server::UuidBytesToText(session.auth_context_uuid)] = session;
  return session.session_uuid;
}

sbps::Frame ManagementFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view operation_key,
                            std::string_view mode = {},
                            std::string_view target_uuid = {}) {
  server::ServerManagementRequest request;
  request.operation_key = std::string(operation_key);
  request.mode = std::string(mode);
  request.target_uuid = std::string(target_uuid);
  request.audit_reason = "cbq057_backup_restore_export_admin_gate";
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kManagementRequest);
  frame.header.payload_schema_id = sbps::kSchemaManagementRequestV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = server::EncodeServerManagementRequestForTest(request);
  return frame;
}

server::ServerManagementContext ServerContext(server::ServerBootstrapConfig* config,
                                              server::ServerLifecycleArtifacts* artifacts,
                                              server::HostedEngineState* engine_state,
                                              server::ServerSessionRegistry* registry,
                                              server::ParserPackageRegistry* parser_registry,
                                              server::ServerListenerOrchestrator* listeners,
                                              server::ServerMaintenanceCoordinator* coordinator,
                                              server::ServerObservabilityState* observability) {
  server::ServerManagementContext context;
  context.config = config;
  context.artifacts = artifacts;
  context.engine_state = engine_state;
  context.session_registry = registry;
  context.parser_registry = parser_registry;
  context.listener_orchestrator = listeners;
  context.maintenance_coordinator = coordinator;
  context.observability = observability;
  return context;
}

server::ServerBootstrapConfig Config(const std::filesystem::path& temp_dir,
                                     const std::filesystem::path& database_path) {
  server::ServerBootstrapConfig config;
  config.database_default_path = database_path;
  config.sbps_enabled = true;
  config.metrics_enabled = true;
  config.control_dir = temp_dir / "control";
  config.data_dir = temp_dir / "data";
  config.log_file = (config.control_dir / "sb_server.log").string();
  return config;
}

server::ServerLifecycleArtifacts Artifacts(std::uint64_t generation, std::string state) {
  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = generation;
  artifacts.state = std::move(state);
  artifacts.pid_file = "[path-redacted]";
  artifacts.lifecycle_state_file = "[path-redacted]";
  artifacts.lifecycle_journal_file = "[path-redacted]";
  return artifacts;
}

void TestServerAdminManagementRoutes(const std::filesystem::path& temp_dir) {
  const auto fixture = CreateCleanDatabase(temp_dir / "admin_route.sbdb", 1779900001000);
  auto config = Config(temp_dir, fixture.path);
  auto artifacts = Artifacts(5701, "cbq057-admin-ready");
  auto engine_state = EngineState(fixture);
  server::ParserPackageRegistry parser_registry;
  parser_registry.entries.push_back({});
  server::ServerListenerOrchestrator listeners;
  listeners.profiles.push_back({});
  server::ServerSessionRegistry registry;
  const auto admin = AddSession(&registry, fixture.path, fixture.database_uuid, "admin");
  const auto auditor = AddSession(&registry, fixture.path, fixture.database_uuid, "auditor");
  auto coordinator = server::BuildMaintenanceCoordinator(config, artifacts);
  auto observability =
      server::InitializeServerObservability(config, artifacts, engine_state, parser_registry, listeners);
  auto context = ServerContext(&config,
                               &artifacts,
                               &engine_state,
                               &registry,
                               &parser_registry,
                               &listeners,
                               &coordinator,
                               &observability);

  const auto missing_session = server::HandleServerManagementRequest(
      context, ManagementFrame({}, "begin_backup_fence"));
  Require(missing_session.error &&
              HasDiagnostic(missing_session, "PARSER_SERVER_IPC.SESSION_NOT_BOUND"),
          "backup fence route accepted a missing session");

  const auto denied = server::HandleServerManagementRequest(
      context, ManagementFrame(auditor, "begin_backup_fence"));
  Require(denied.error && HasDiagnostic(denied, "SECURITY.ACCESS_DENIED"),
          "backup fence route accepted a non-admin control session");

  const auto begin_backup = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "begin_backup_fence"));
  Require(begin_backup.accepted && !begin_backup.error && coordinator.backup_fence_active,
          "admin backup fence begin route failed");
  const auto end_backup = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "end_backup_fence"));
  Require(end_backup.accepted && !end_backup.error && !coordinator.backup_fence_active,
          "admin backup fence end route failed");

  const auto begin_restore = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "begin_restore_fence"));
  Require(begin_restore.accepted && !begin_restore.error && coordinator.restore_fence_active &&
              coordinator.attach_admission_fenced && coordinator.write_admission_fenced &&
              coordinator.sblr_admission_fenced,
          "admin restore fence begin route failed to fence runtime admission");
  const auto end_restore = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "end_restore_fence"));
  Require(end_restore.accepted && !end_restore.error && !coordinator.restore_fence_active &&
              !coordinator.attach_admission_fenced && !coordinator.write_admission_fenced &&
              !coordinator.sblr_admission_fenced,
          "admin restore fence end route failed to clear runtime admission fences");

  const auto support_export = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "export_server_support_bundle"));
  Require(support_export.accepted && !support_export.error,
          "admin support bundle export route failed");
  const std::string support_payload(support_export.payload.begin(), support_export.payload.end());
  Require(Contains(support_payload, "engine.authorization.management.SUPPORT_EXPORT"),
          "server support bundle route omitted support-export authority");
  Require(Contains(support_payload, "support_bundle_uuid"),
          "server support bundle route omitted export record");
  Require(!Contains(support_payload, temp_dir.string()),
          "server support bundle route leaked local temp path");

  const auto repair_refused = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "repair"));
  Require(repair_refused.error && HasDiagnostic(repair_refused, "ENGINE.DBLC_REPAIR_REFUSED"),
          "admin repair route accepted missing repair plan");
}

void TestServerCreateOpenAttachDetachRoutes(const std::filesystem::path& temp_dir) {
  const auto path = temp_dir / "created_by_admin.sbdb";
  auto config = Config(temp_dir, path);
  auto artifacts = Artifacts(5702, "cbq057-create-ready");
  server::HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  server::ParserPackageRegistry parser_registry;
  server::ServerListenerOrchestrator listeners;
  server::ServerSessionRegistry registry;
  const auto admin = AddSession(&registry, path, {}, "admin");
  auto coordinator = server::BuildMaintenanceCoordinator(config, artifacts);
  auto observability =
      server::InitializeServerObservability(config, artifacts, engine_state, parser_registry, listeners);
  auto context = ServerContext(&config,
                               &artifacts,
                               &engine_state,
                               &registry,
                               &parser_registry,
                               &listeners,
                               &coordinator,
                               &observability);

  const auto created = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "create", "allow_minimal_resource_bootstrap:true"));
  Require(created.accepted && !created.error && std::filesystem::exists(path),
          "admin create database route failed");

  const auto opened = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "open"));
  Require(opened.accepted && !opened.error, "admin open database route failed");

  const auto attached = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "attach"));
  Require(attached.accepted && !attached.error, "admin attach database route failed");

  const auto detached = server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "detach"));
  Require(detached.accepted && !detached.error, "admin detach database route failed");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestBackupRestoreEngineOwnedLifecycle(temp_dir);
  TestRestoreCoverageRefusal(temp_dir);
  TestSblrExportAndSupportBundleRoutes(temp_dir);
  TestServerAdminManagementRoutes(temp_dir);
  TestServerCreateOpenAttachDetachRoutes(temp_dir);
  std::filesystem::remove_all(temp_dir);
  std::cout << "CBQ_057_BACKUP_RESTORE_EXPORT_ADMIN_COMPLETE=passed\n";
  return EXIT_SUCCESS;
}
