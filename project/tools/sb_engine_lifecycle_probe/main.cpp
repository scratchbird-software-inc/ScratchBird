// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lifecycle/engine_lifecycle_api.hpp"
#include "database_lifecycle.hpp"
#include "startup_state.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

std::uint64_t NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

scratchbird::core::platform::TypedUuid Generate(scratchbird::core::platform::UuidKind kind, std::uint64_t millis) {
  auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

bool CreateDatabase(const std::string& path) {
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".sb.owner.lock");
  scratchbird::storage::database::DatabaseCreateConfig create;
  const auto seed = NowMillis();
  create.path = path;
  create.database_uuid = Generate(scratchbird::core::platform::UuidKind::database, seed);
  create.filespace_uuid = Generate(scratchbird::core::platform::UuidKind::filespace, seed + 1);
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  return create.database_uuid.valid() && create.filespace_uuid.valid() && scratchbird::storage::database::CreateDatabaseFile(create).ok();
}

bool ForceDirtyFenced(const std::string& path) {
  scratchbird::storage::disk::FileDevice device;
  const auto opened = device.Open(path, scratchbird::storage::disk::FileOpenMode::open_existing);
  if (!opened.ok()) { return false; }
  auto state = scratchbird::storage::database::ReadStartupStatePageBody(&device, 16384);
  if (!state.ok()) { (void)device.Close(); return false; }
  state.state.clean_shutdown = false;
  state.state.startup_dirty = true;
  state.state.write_admission_fenced = true;
  state.state.recovery_classification = scratchbird::storage::database::StartupRecoveryClassification::restricted_open_required;
  const bool written = scratchbird::storage::database::WriteStartupStatePageBody(&device, state.state).ok();
  (void)device.Close();
  return written;
}

EngineRequestContext Context(const std::string& path, bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "engine-lifecycle-secure" : "engine-lifecycle-open";
  context.database_path = path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001901";
  context.session_uuid.canonical = secure ? "00000000-0000-7000-8000-000000001902" : "00000000-0000-7000-8000-000000001903";
  context.principal_uuid.canonical = secure ? "00000000-0000-7000-8000-000000001904" : "";
  return context;
}

bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

bool RowFieldEquals(const EngineApiResult& result, const std::string& field_name, const std::string& expected) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name && field.second.encoded_value == expected) { return true; }
    }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main() {
  const std::string path = "/tmp/sb_engine_lifecycle_probe_" + std::to_string(NowMillis()) + ".db";
  const bool database_created = CreateDatabase(path);

  EngineOpenLifecycleRequest open;
  open.context = Context(path, false);
  open.option_envelopes.push_back("mode:read_write");
  const auto open_result = EngineOpenLifecycle(open);

  EngineEnterMaintenanceLifecycleRequest open_maintenance;
  open_maintenance.context = Context(path, false);
  open_maintenance.option_envelopes.push_back("mode:maintenance");
  const auto maintenance_security_result = EngineEnterMaintenanceLifecycle(open_maintenance);

  EngineAttachLifecycleRequest attach;
  attach.context = Context(path, true);
  attach.option_envelopes.push_back("attach:true");
  const auto attach_result = EngineAttachLifecycle(attach);

  EngineEnterMaintenanceLifecycleRequest maintenance;
  maintenance.context = Context(path, true);
  maintenance.option_envelopes.push_back("mode:maintenance");
  maintenance.option_envelopes.push_back("policy:maintenance_allowed");
  const auto maintenance_result = EngineEnterMaintenanceLifecycle(maintenance);

  EngineOpenLifecycleRequest unsafe;
  unsafe.context = Context(path, true);
  unsafe.option_envelopes.push_back("mode:read_only");
  unsafe.option_envelopes.push_back("allow_writes:true");
  const auto unsafe_result = EngineOpenLifecycle(unsafe);

  EngineOpenLifecycleRequest cluster;
  cluster.context = Context(path, true);
  cluster.option_envelopes.push_back("global_deploy:true");
  const auto cluster_result = EngineOpenLifecycle(cluster);

  EngineShutdownLifecycleRequest shutdown;
  shutdown.context = Context(path, true);
  shutdown.option_envelopes.push_back("shutdown:clean");
  const auto shutdown_result = EngineShutdownLifecycle(shutdown);

  EngineOpenLifecycleRequest read_only;
  read_only.context = Context(path, false);
  read_only.option_envelopes.push_back("mode:read_only");
  const auto read_only_result = EngineOpenLifecycle(read_only);

  const bool dirty_forced = ForceDirtyFenced(path);
  EngineEnterRestrictedOpenLifecycleRequest restricted;
  restricted.context = Context(path, true);
  restricted.option_envelopes.push_back("mode:restricted_open");
  const auto restricted_result = EngineEnterRestrictedOpenLifecycle(restricted);

  EngineDetachLifecycleRequest detach;
  detach.context = Context(path, true);
  detach.option_envelopes.push_back("detach:true");
  const auto detach_result = EngineDetachLifecycle(detach);

  const bool open_ok = open_result.ok && HasEvidence(open_result, "engine_lifecycle", "read_write") && RowFieldEquals(open_result, "write_admission_fenced", "false");
  const bool maintenance_security_denied = !maintenance_security_result.ok && HasDiagnosticCode(maintenance_security_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");
  const bool attach_ok = attach_result.ok && HasEvidence(attach_result, "engine_lifecycle", "attached");
  const bool maintenance_ok = maintenance_result.ok && HasEvidence(maintenance_result, "engine_lifecycle", "maintenance") && RowFieldEquals(maintenance_result, "read_only", "true");
  const bool unsafe_denied = !unsafe_result.ok && HasDiagnosticCode(unsafe_result, "SB_ENGINE_API_LIFECYCLE_UNSAFE_PROFILE");
  const bool cluster_denied = !cluster_result.ok && cluster_result.cluster_authority_required;
  const bool shutdown_ok = shutdown_result.ok && HasEvidence(shutdown_result, "engine_lifecycle", "shutdown_clean");
  const bool read_only_ok = read_only_result.ok && HasEvidence(read_only_result, "engine_lifecycle", "read_only") && RowFieldEquals(read_only_result, "read_only", "true");
  const bool restricted_ok = dirty_forced && restricted_result.ok && HasEvidence(restricted_result, "engine_lifecycle", "restricted_open") && RowFieldEquals(restricted_result, "read_only", "true");
  const bool detach_ok = detach_result.ok && HasEvidence(detach_result, "engine_lifecycle", "detached");

  const bool ok = database_created && open_ok && maintenance_security_denied && attach_ok && maintenance_ok &&
                  unsafe_denied && cluster_denied && shutdown_ok && read_only_ok && restricted_ok && detach_ok;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("database_created", database_created, true);
  PrintBool("open_ok", open_ok, true);
  PrintBool("maintenance_security_denied", maintenance_security_denied, true);
  PrintBool("attach_ok", attach_ok, true);
  PrintBool("maintenance_ok", maintenance_ok, true);
  PrintBool("unsafe_denied", unsafe_denied, true);
  PrintBool("cluster_denied", cluster_denied, true);
  PrintBool("shutdown_ok", shutdown_ok, true);
  PrintBool("read_only_ok", read_only_ok, true);
  PrintBool("restricted_ok", restricted_ok, true);
  PrintBool("detach_ok", detach_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
