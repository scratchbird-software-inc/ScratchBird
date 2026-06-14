// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_SBLR_ADMISSION_VALIDATOR

#include "sblr_admission.hpp"

#include "scratchbird/engine/sblr_envelope.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace scratchbird::server {
namespace {

struct FamilyRule {
  std::string_view family;
  std::string_view default_operation_id;
  bool cluster_private = false;
};

constexpr std::array<FamilyRule, 62> kServerSblrFamilies{{
    {"sblr.acceleration.gpu.v3", "acceleration.gpu.operation", false},
    {"sblr.acceleration.llvm.v3", "extensibility.compile_llvm_module", false},
    {"sblr.archive_replication.operation.v3", "archive_replication.operation", false},
    {"sblr.archive.operation.v3", "archive.operation", false},
    {"sblr.backup.operation.v3", "backup.operation", false},
    {"sblr.bridge.operation.v3", "bridge.describe_capabilities", false},
    {"sblr.bulk.export.v3", "bulk.export", false},
    {"sblr.bulk.import.v3", "bulk.import", false},
    {"sblr.catalog.introspect.v3", "catalog.get_descriptor", false},
    {"sblr.catalog.mutation.v3", "catalog.mutation", false},
    {"sblr.cluster.control.v3", "cluster.control_cluster", true},
    {"sblr.cluster.report.v3", "cluster.inspect_state", true},
    {"sblr.cursor.operation.v3", "session.cursor_open", false},
    {"sblr.database.management.v3", "lifecycle.inspect_database", false},
    {"sblr.diagnostic.control.v3", "diagnostic.control", false},
    {"sblr.diagnostic.refusal.v3", "diagnostic.refusal", false},
    {"sblr.dml.operation.v3", "dml.operation", false},
    {"sblr.dml.delete.v3", "dml.delete_rows", false},
    {"sblr.dml.insert.v3", "dml.insert_rows", false},
    {"sblr.dml.merge.v3", "dml.merge_rows", false},
    {"sblr.dml.update.v3", "dml.update_rows", false},
    {"sblr.event.channel.v3", "event.channel.notify", false},
    {"sblr.event.delivery.v3", "event.delivery.poll", false},
    {"sblr.event.publication.v3", "event.publication.operation", false},
    {"sblr.event.subscription.v3", "event.subscription.list", false},
    {"sblr.expression.runtime.v3", "query.cast_value", false},
    {"sblr.filespace.management.v3", "storage.manage_operation", false},
    {"sblr.fulltext.execution.v3", "nosql.search_query", false},
    {"sblr.general.operation.v3", "general.operation", false},
    {"sblr.graph.execution.v3", "nosql.graph_query", false},
    {"sblr.index.maintenance.v3", "index.maintenance", false},
    {"sblr.jobs.operation.v3", "jobs.scheduler.operation", false},
    {"sblr.language.resource_control.v3", "language.session.show", false},
    {"sblr.management.control.v3", "management.control_runtime", false},
    {"sblr.management.report.v3", "management.inspect_runtime", false},
    {"sblr.management.runtime_operation.v3", "management.inspect_runtime", false},
    {"sblr.migration.operation.v3", "op.show.migrations", false},
    {"sblr.metrics.read.v3", "observability.show_metrics", false},
    {"sblr.mga.control.v3", "transaction.set_characteristics", false},
    {"sblr.mga.report.v3", "observability.show_transactions", false},
    {"sblr.observability.inspect.v3", "observability.show_version", false},
    {"sblr.optimizer.plan.v3", "query.plan_operation", false},
    {"sblr.parser.operation.v3", "extensibility.register_parser_package", false},
    {"sblr.policy.operation.v3", "security.policy.show", false},
    {"sblr.query.document.v3", "nosql.document_find", false},
    {"sblr.query.graph.v3", "nosql.graph_query", false},
    {"sblr.query.kv.v3", "nosql.key_value_get", false},
    {"sblr.query.multimodel_or_ddl.v3", "query.multimodel_or_ddl", false},
    {"sblr.query.relational.v3", "dml.select", false},
    {"sblr.query.search.v3", "nosql.search_query", false},
    {"sblr.query.timeseries.v3", "nosql.time_series_append", false},
    {"sblr.query.vector.v3", "nosql.vector_search", false},
    {"sblr.replication.consumer.v3", "cluster.inspect_replication", true},
    {"sblr.replication.operation.v3", "replication.operation", false},
    {"sblr.routine.define.v3", "routine.define", false},
    {"sblr.routine.execute.v3", "extensibility.invoke_udr_package", false},
    {"sblr.security.mutation.v3", "security.grant_right", false},
    {"sblr.session.management.v3", "session.prepare_statement", false},
    {"sblr.statement.management.v3", "session.prepare_statement", false},
    {"sblr.transaction.control.v3", "transaction.control", false},
    {"sblr.udr.operation.v3", "extensibility.invoke_udr_package", false},
    {"sblr.vector.execution.v3", "nosql.vector_search", false},
}};

constexpr std::array<std::string_view, 1> kFailClosedSblrFamilies{{
    "sblr.cluster.private_operation.v3",
}};

constexpr std::array<std::string_view, 10> kNonPrimarySblrAuditFamilies{{
    "sblr.acceleration.operation.v3",
    "sblr.archive_replication.operation.v3",
    "sblr.cluster.private_operation.v3",
    "sblr.dml.operation.v3",
    "sblr.expression.runtime.v3",
    "sblr.general.operation.v3",
    "sblr.jobs.operation.v3",
    "sblr.management.runtime_operation.v3",
    "sblr.observability.inspect.v3",
    "sblr.query.multimodel_or_ddl.v3",
}};

ServerDiagnostic AdmissionDiagnostic(std::string code,
                                     std::string message,
                                     std::string detail = {}) {
  std::vector<ServerDiagnosticField> fields;
  if (!detail.empty()) fields.push_back({"detail", detail});
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

std::string Trim(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' ||
          value[first] == '\n')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t' ||
          value[last - 1] == '\r' || value[last - 1] == '\n')) {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

bool EqualAsciiInsensitive(char lhs, char rhs) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(lhs))) ==
         static_cast<char>(std::tolower(static_cast<unsigned char>(rhs)));
}

bool StartsWithInsensitive(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin(), EqualAsciiInsensitive);
}

bool LooksLikeRawSql(std::string_view payload) {
  const std::string trimmed = Trim(payload);
  return StartsWithInsensitive(trimmed, "select") || StartsWithInsensitive(trimmed, "insert") ||
         StartsWithInsensitive(trimmed, "update") || StartsWithInsensitive(trimmed, "delete") ||
         StartsWithInsensitive(trimmed, "create") || StartsWithInsensitive(trimmed, "alter") ||
         StartsWithInsensitive(trimmed, "drop") || StartsWithInsensitive(trimmed, "with");
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsClusterOperationId(std::string_view operation_id) {
  return StartsWith(operation_id, "cluster.") ||
         StartsWith(operation_id, "op.cluster.") ||
         StartsWith(operation_id, "op.show.cluster.") ||
         operation_id == "op.show.cluster_gpu_placement" ||
         StartsWith(operation_id, "placement.cluster.");
}

bool IsMemoryControlOperationId(std::string_view operation_id) {
  return operation_id == "memory.profile.set" ||
         operation_id == "memory.cache.flush" ||
         operation_id == "memory.cache.invalidate" ||
         operation_id == "memory.scavenge" ||
         operation_id == "memory.grant_feedback.reset" ||
         operation_id == "memory.stream_policy.set" ||
         operation_id == "memory.udr_limit.set" ||
         operation_id == "memory.dump_policy.set" ||
         operation_id == "memory.optimizer.set" ||
         operation_id == "memory.optimizer.run" ||
         operation_id == "memory.object_residency.set" ||
         operation_id == "memory.rate_limit.set" ||
         operation_id == "memory.policy_migration.plan";
}

bool IsStorageTierMigrationOperationId(std::string_view operation_id) {
  return StartsWith(operation_id, "storage_tier.");
}

bool IsFilespaceDiscoveryOperationId(std::string_view operation_id) {
  return StartsWith(operation_id, "filespace.discovery.");
}

bool IsFilespacePackageOperationId(std::string_view operation_id) {
  return StartsWith(operation_id, "filespace.package.");
}

bool IsFilespaceLifecycleOperationId(std::string_view operation_id) {
  return operation_id == "filespace.create" ||
         operation_id == "filespace.preallocate" ||
         operation_id == "filespace.attach" ||
         operation_id == "filespace.detach" ||
         operation_id == "filespace.disconnect" ||
         operation_id == "filespace.move" ||
         operation_id == "filespace.merge" ||
         operation_id == "filespace.promote" ||
         operation_id == "filespace.verify" ||
         operation_id == "filespace.compact" ||
         operation_id == "filespace.fence" ||
         operation_id == "filespace.release" ||
         operation_id == "filespace.archive" ||
         operation_id == "filespace.quarantine" ||
         operation_id == "filespace.snapshot.create" ||
         operation_id == "filespace.snapshot.refresh" ||
         operation_id == "filespace.snapshot.validate" ||
         operation_id == "filespace.snapshot.retire" ||
         operation_id == "filespace.shadow.create" ||
         operation_id == "filespace.shadow.refresh" ||
         operation_id == "filespace.shadow.validate" ||
         operation_id == "filespace.shadow.promote" ||
         operation_id == "filespace.truncate" ||
         operation_id == "filespace.drop" ||
         operation_id == "filespace.delete_physical" ||
         operation_id == "filespace.repair" ||
         operation_id == "filespace.rebuild" ||
         operation_id == "filespace.salvage";
}

bool IsShardPlacementDescriptorOperationId(std::string_view operation_id) {
  return StartsWith(operation_id, "shard_placement.");
}

bool IsEncryptionMaintenanceOperationId(std::string_view operation_id) {
  return operation_id == "security.encryption_key.admit" ||
         operation_id == "security.encryption_key.rotate" ||
         operation_id == "security.protected_material_cache.inspect" ||
         operation_id == "security.protected_material_cache.purge" ||
         operation_id == "security.protected_material_cache.shutdown" ||
         operation_id == "security.encrypted_filespace.open" ||
         operation_id == "security.request_protected_material" ||
         operation_id == "security.protected_material.version.purge";
}

bool IsProtectedMaterialOperationId(std::string_view operation_id) {
  return StartsWith(operation_id, "security.protected_material.");
}

bool IsPublicExactOperationId(std::string_view operation_id) {
  return operation_id == "op.gpu.artifact_quarantine" ||
         operation_id == "op.gpu.cache_clear" ||
         operation_id == "op.gpu.device_quarantine" ||
         operation_id == "op.gpu.kernel_quarantine" ||
         operation_id == "op.gpu.profile_disable" ||
         operation_id == "op.gpu.profile_enable" ||
         operation_id == "op.management.listener.drain" ||
         operation_id == "op.management.listener.undrain" ||
         operation_id == "op.management.manager.restart" ||
         operation_id == "op.management.manager.start" ||
         operation_id == "op.management.manager.stop" ||
         operation_id == "op.management.parser_pool.resize" ||
         operation_id == "op.native_compile.aot_rebuild" ||
         operation_id == "op.native_compile.artifact_quarantine" ||
         operation_id == "op.native_compile.cache_invalidate" ||
         operation_id == "op.native_compile.profile_disable" ||
         operation_id == "op.native_compile.profile_enable" ||
         operation_id == "op.management.config.reload" ||
         operation_id == "op.management.instruction.ack" ||
         operation_id == "op.management.instruction.apply" ||
         operation_id == "op.management.instruction.cancel" ||
         operation_id == "op.management.instruction.quarantine" ||
         operation_id == "op.management.support_bundle.create" ||
         StartsWith(operation_id, "memory.") ||
         IsStorageTierMigrationOperationId(operation_id) ||
         IsFilespaceDiscoveryOperationId(operation_id) ||
         IsFilespacePackageOperationId(operation_id) ||
         IsFilespaceLifecycleOperationId(operation_id) ||
         IsShardPlacementDescriptorOperationId(operation_id) ||
         IsEncryptionMaintenanceOperationId(operation_id) ||
         IsProtectedMaterialOperationId(operation_id) ||
         operation_id == "op.migration.alter" ||
         operation_id == "op.migration.begin_from_reference" ||
         operation_id == "op.show.aot_artifacts" ||
         operation_id == "op.show.audit" ||
         operation_id == "op.show.buffer_pool" ||
         operation_id == "op.show.cache" ||
         operation_id == "op.show.capabilities" ||
         operation_id == "op.show.context" ||
         operation_id == "op.show.dialect" ||
         operation_id == "op.show.discovery_rights" ||
         operation_id == "op.show.gpu" ||
         operation_id == "op.show.gpu_artifacts" ||
         operation_id == "op.show.gpu_capability" ||
         operation_id == "op.show.gpu_devices" ||
         operation_id == "op.show.gpu_kernels" ||
         operation_id == "op.show.gpu_memory" ||
         operation_id == "op.show.grants" ||
         operation_id == "op.show.groups" ||
         operation_id == "op.show.identity_providers" ||
         operation_id == "op.show.index_health" ||
         operation_id == "op.show.io" ||
         operation_id == "op.show.job" ||
         operation_id == "op.show.job_dependencies" ||
         operation_id == "op.show.job_runs" ||
         operation_id == "op.show.jobs" ||
         operation_id == "op.show.llvm" ||
         operation_id == "op.show.llvm_provenance" ||
         operation_id == "op.show.llvm_targets" ||
         operation_id == "op.show.management.config" ||
         operation_id == "op.show.management.drift" ||
         operation_id == "op.show.locks" ||
         operation_id == "op.show.management.instructions" ||
         operation_id == "op.show.management.listeners" ||
         operation_id == "op.show.management.manager" ||
         operation_id == "op.show.management.parser_pool" ||
         operation_id == "op.show.management.readiness" ||
         operation_id == "op.show.management.servers" ||
         operation_id == "op.show.management.support_bundle_safety" ||
         operation_id == "op.show.management.support_bundles" ||
         operation_id == "op.show.migration" ||
         operation_id == "op.show.migrations" ||
         operation_id == "op.show.masks" ||
         operation_id == "op.show.metrics" ||
         operation_id == "op.show.metrics_family" ||
         operation_id == "op.show.native_compile" ||
         operation_id == "op.show.native_compile_cache" ||
         operation_id == "op.show.object_visibility" ||
         operation_id == "op.show.performance" ||
         operation_id == "op.show.policies" ||
         operation_id == "op.show.query_store" ||
         operation_id == "op.show.rls" ||
         operation_id == "op.show.roles" ||
         operation_id == "op.show.schema_path" ||
         operation_id == "op.show.search_path" ||
         operation_id == "op.show.security_events" ||
         operation_id == "op.show.security_profiles" ||
         operation_id == "op.show.sessions" ||
         operation_id == "op.show.statement_cache" ||
         operation_id == "op.show.statements" ||
         operation_id == "op.show.system" ||
         operation_id == "op.show.transaction" ||
         operation_id == "op.show.transaction_isolation" ||
         operation_id == "op.show.transactions" ||
         operation_id == "op.show.users" ||
         operation_id == "op.show.version" ||
         operation_id == "op.show.wait_events";
}

std::optional<std::string> FamilyForClusterOperationId(std::string_view operation_id) {
  if (operation_id == "cluster.control_cluster" ||
      operation_id == "cluster.profile_operation" ||
      operation_id == "cluster.route" ||
      operation_id == "cluster.prepare_remote_participant_insert" ||
      operation_id == "cluster.validate_insert_route_fence" ||
      operation_id == "cluster.place_object" ||
      StartsWith(operation_id, "op.cluster.") ||
      StartsWith(operation_id, "placement.cluster.")) {
    return "sblr.cluster.control.v3";
  }
  if (operation_id == "cluster.inspect_state" ||
      operation_id == "cluster.inspect_provider" ||
      operation_id == "cluster.inspect_routing_plan" ||
      operation_id == "cluster.sys.agents" ||
      operation_id == "op.show.cluster_gpu_placement" ||
      StartsWith(operation_id, "cluster.agent.") ||
      StartsWith(operation_id, "op.show.cluster")) {
    return "sblr.cluster.report.v3";
  }
  if (operation_id == "cluster.inspect_replication") {
    return "sblr.replication.consumer.v3";
  }
  return std::nullopt;
}

std::string PublicExactFamilyForOperationId(std::string_view operation_id) {
  if (IsClusterOperationId(operation_id)) {
    return FamilyForClusterOperationId(operation_id).value_or("");
  }
  if (operation_id == "op.show.audit" ||
      operation_id == "op.show.discovery_rights" ||
      operation_id == "op.show.grants" ||
      operation_id == "op.show.groups" ||
      operation_id == "op.show.identity_providers" ||
      operation_id == "op.show.masks" ||
      operation_id == "op.show.object_visibility" ||
      operation_id == "op.show.policies" ||
      operation_id == "op.show.rls" ||
      operation_id == "op.show.roles" ||
      operation_id == "op.show.security_events" ||
      operation_id == "op.show.security_profiles" ||
      operation_id == "op.show.users") {
    if (operation_id == "op.show.policies" ||
        operation_id == "op.show.rls" ||
        operation_id == "op.show.masks") {
      return "sblr.policy.operation.v3";
    }
    return "sblr.catalog.introspect.v3";
  }
  if (StartsWith(operation_id, "op.migration.") ||
      operation_id == "op.show.migration" ||
      operation_id == "op.show.migrations") {
    return "sblr.migration.operation.v3";
  }
  if (operation_id == "management.control_runtime" ||
      StartsWith(operation_id, "agents.") ||
      StartsWith(operation_id, "op.management.")) {
    return "sblr.management.control.v3";
  }
  if (StartsWith(operation_id, "memory.")) {
    return IsMemoryControlOperationId(operation_id) ? "sblr.management.control.v3"
                                                   : "sblr.management.report.v3";
  }
  if (IsStorageTierMigrationOperationId(operation_id)) {
    return "sblr.filespace.management.v3";
  }
  if (IsFilespaceDiscoveryOperationId(operation_id)) {
    return "sblr.filespace.management.v3";
  }
  if (IsFilespacePackageOperationId(operation_id)) {
    return "sblr.filespace.management.v3";
  }
  if (IsFilespaceLifecycleOperationId(operation_id)) {
    return "sblr.filespace.management.v3";
  }
  if (IsShardPlacementDescriptorOperationId(operation_id)) {
    return "sblr.filespace.management.v3";
  }
  if (IsEncryptionMaintenanceOperationId(operation_id)) {
    return "sblr.security.mutation.v3";
  }
  if (IsProtectedMaterialOperationId(operation_id)) {
    return "sblr.security.mutation.v3";
  }
  if (operation_id == "management.inspect_runtime" ||
      operation_id == "op.show.management.config" ||
      operation_id == "op.show.management.drift" ||
      operation_id == "op.show.management.instructions" ||
      operation_id == "op.show.management.listeners" ||
      operation_id == "op.show.management.manager" ||
      operation_id == "op.show.management.parser_pool" ||
      operation_id == "op.show.management.readiness" ||
      operation_id == "op.show.management.servers" ||
      operation_id == "op.show.management.support_bundle_safety" ||
      operation_id == "op.show.management.support_bundles") {
    return "sblr.management.report.v3";
  }
  if (StartsWith(operation_id, "op.gpu.") ||
      StartsWith(operation_id, "op.native_compile.") ||
      operation_id == "op.show.aot_artifacts" ||
      StartsWith(operation_id, "op.show.gpu") ||
      StartsWith(operation_id, "op.show.llvm") ||
      operation_id == "op.show.native_compile" ||
      operation_id == "op.show.native_compile_cache") {
    if (StartsWith(operation_id, "op.gpu.") ||
        StartsWith(operation_id, "op.show.gpu")) {
      return "sblr.acceleration.gpu.v3";
    }
    if (StartsWith(operation_id, "op.native_compile.") ||
        operation_id == "op.show.aot_artifacts" ||
        StartsWith(operation_id, "op.show.llvm") ||
        operation_id == "op.show.native_compile" ||
        operation_id == "op.show.native_compile_cache") {
      return "sblr.acceleration.llvm.v3";
    }
    return "sblr.management.report.v3";
  }
  if (operation_id == "op.show.metrics" ||
      operation_id == "op.show.metrics_family" ||
      operation_id == "op.show.performance") {
    return "sblr.metrics.read.v3";
  }
  if (operation_id == "op.show.index_health") {
    return "sblr.index.maintenance.v3";
  }
  return "sblr.management.report.v3";
}

bool RequiresEnginePublicAbiDispatch(std::string_view operation_id) {
  return IsClusterOperationId(operation_id) ||
         IsPublicExactOperationId(operation_id) ||
         StartsWith(operation_id, "bridge.") ||
         StartsWith(operation_id, "index.") ||
         StartsWith(operation_id, "lifecycle.") ||
         StartsWith(operation_id, "transaction.") ||
         operation_id == "dml.select_rows" ||
         operation_id == "dml.insert_rows" ||
         operation_id == "dml.update_rows" ||
         operation_id == "dml.delete_rows" ||
         operation_id == "dml.merge_rows" ||
         operation_id == "dml.plan_import_rows" ||
         operation_id == "dml.execute_import_rows" ||
         operation_id == "dml.execute_native_bulk_ingest" ||
         operation_id == "dml.normalize_import_checkpoint_model" ||
         operation_id == "dml.normalize_import_reject_model" ||
         operation_id == "artifact.export_catalog" ||
         operation_id == "artifact.import_catalog" ||
         operation_id == "ddl.create_database" ||
         operation_id == "catalog.resolve_name" ||
         operation_id == "catalog.map_uuid_to_name" ||
         operation_id == "catalog.lookup_object" ||
         operation_id == "catalog.list_children" ||
         operation_id == "catalog.get_dependencies" ||
         operation_id == "query.bind_expression" ||
         operation_id == "query.bind_predicate" ||
         operation_id == "query.bind_projection" ||
         operation_id == "query.cast_value" ||
         operation_id == "query.extract_value" ||
         operation_id == "query.set_operation" ||
         operation_id == "query.apply_numeric_operation" ||
         operation_id == "query.canonicalize_document_value" ||
         operation_id == "query.evaluate_advanced_datatype_family" ||
         operation_id == "query.validate_domain_value" ||
         operation_id == "query.invoke_domain_method" ||
         operation_id == "query.evaluate_projection" ||
         operation_id == "query.plan_operation" ||
         operation_id == "ddl.create_schema" ||
         operation_id == "ddl.create_table" ||
         operation_id == "ddl.create_index" ||
         operation_id == "ddl.create_index_template" ||
         operation_id == "ddl.create_domain" ||
         operation_id == "ddl.create_sequence" ||
         operation_id == "ddl.create_statistics" ||
         operation_id == "ddl.create_view" ||
         operation_id == "ddl.create_function" ||
         operation_id == "ddl.create_procedure" ||
         operation_id == "ddl.create_trigger" ||
         operation_id == "ddl.alter_object" ||
         operation_id == "ddl.drop_object" ||
         operation_id == "ddl.comment_on_object" ||
         operation_id == "catalog.get_descriptor" ||
         StartsWith(operation_id, "catalog.mutation.") ||
         StartsWith(operation_id, "nosql.") ||
         operation_id == "ddl.constraint.create" ||
         operation_id == "ddl.constraint.alter" ||
         operation_id == "ddl.constraint.drop" ||
         operation_id == "event.channel.create" ||
         operation_id == "event.channel.listen" ||
         operation_id == "event.channel.unlisten" ||
         operation_id == "event.channel.notify" ||
         operation_id == "event.subscription.list" ||
         operation_id == "event.delivery.poll" ||
         operation_id == "event.delivery.ack" ||
         operation_id == "session.notification.unlisten" ||
         operation_id == "session.notification.unlisten_all" ||
         StartsWith(operation_id, "agents.") ||
         operation_id == "security.create_identity" ||
         operation_id == "security.alter_identity" ||
         operation_id == "security.grant_right" ||
         operation_id == "security.revoke_right" ||
         operation_id == "security.evaluate_visibility" ||
         operation_id == "security.evaluate_policy" ||
         operation_id == "security.evaluate_deep_enforcement" ||
         operation_id == "management.inspect_config" ||
         operation_id == "management.set_config" ||
         operation_id == "management.reset_config" ||
         operation_id == "management.prepare_support_bundle" ||
         operation_id == "management.inspect_runtime" ||
         operation_id == "management.control_runtime" ||
         operation_id == "storage.manage_operation" ||
         IsStorageTierMigrationOperationId(operation_id) ||
         StartsWith(operation_id, "filespace.") ||
         operation_id == "extensibility.register_parser_package" ||
         StartsWith(operation_id, "extensibility.register_udr_package") ||
         StartsWith(operation_id, "extensibility.load_udr_package") ||
         StartsWith(operation_id, "extensibility.unload_udr_package") ||
         StartsWith(operation_id, "extensibility.inspect_udr_packages") ||
         StartsWith(operation_id, "extensibility.invoke_udr_package") ||
         operation_id == "extensibility.inspect_gpu_capability" ||
         operation_id == "extensibility.compile_llvm_module" ||
         operation_id == "observability.show_version" ||
         operation_id == "observability.show_database" ||
         operation_id == "observability.show_system" ||
         operation_id == "observability.show_catalog" ||
         operation_id == "observability.show_sessions" ||
         operation_id == "observability.show_transactions" ||
         operation_id == "observability.show_locks" ||
         operation_id == "observability.show_statements" ||
         operation_id == "observability.show_jobs" ||
         operation_id == "observability.show_management" ||
         operation_id == "observability.show_diagnostics" ||
         operation_id == "observability.show_diagnostics_extended" ||
         operation_id == "observability.show_archive_replication" ||
         operation_id == "observability.show_agents_extended" ||
         operation_id == "observability.show_filespace_extended" ||
         operation_id == "observability.show_decision_service" ||
         operation_id == "observability.show_acceleration" ||
         operation_id == "observability.show_acceleration_extended" ||
         operation_id == "observability.show_metrics" ||
         operation_id == "observability.explain_operation" ||
         StartsWith(operation_id, "general.") ||
         operation_id == "security.privilege.grant" ||
         operation_id == "security.privilege.revoke" ||
         operation_id == "security.session.set_role" ||
         operation_id == "security.principal.create" ||
         operation_id == "security.principal.alter" ||
         operation_id == "security.policy.create" ||
         operation_id == "security.policy.alter" ||
         operation_id == "security.policy.attach" ||
         operation_id == "security.policy.activate" ||
         operation_id == "security.policy.deactivate" ||
         operation_id == "security.policy.validate" ||
         operation_id == "security.policy.show";
}

bool ContainsSqlTextMarker(std::string_view encoded) {
  return Contains(encoded, "contains_sql_text=true") ||
         Contains(encoded, "\"contains_sql_text\":true") ||
         Contains(encoded, "\"contains_sql_text\": true") ||
         Contains(encoded, "\"source_payload_embedded\":true") ||
         Contains(encoded, "\"source_payload_embedded\": true") ||
         Contains(encoded, "\"source_text\"") ||
         Contains(encoded, "\"sql_text\"");
}

std::optional<std::string> TextField(std::string_view encoded, std::string_view key) {
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos && Trim(line.substr(0, equals)) == key) {
      return Trim(line.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::size_t TextFieldCount(std::string_view encoded, std::string_view key) {
  std::size_t count = 0;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos && Trim(line.substr(0, equals)) == key) {
      ++count;
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return count;
}

std::optional<std::string> JsonStringField(std::string_view encoded, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = encoded.find(needle);
  if (key_pos == std::string_view::npos) return std::nullopt;
  const std::size_t colon = encoded.find(':', key_pos + needle.size());
  if (colon == std::string_view::npos) return std::nullopt;
  std::size_t quote = colon + 1;
  while (quote < encoded.size() && std::isspace(static_cast<unsigned char>(encoded[quote]))) {
    ++quote;
  }
  if (quote >= encoded.size() || encoded[quote] != '"') return std::nullopt;
  ++quote;
  std::string out;
  bool escaped = false;
  for (std::size_t i = quote; i < encoded.size(); ++i) {
    const char ch = encoded[i];
    if (!escaped && ch == '"') return out;
    if (!escaped && ch == '\\') {
      escaped = true;
      continue;
    }
    out.push_back(ch);
    escaped = false;
  }
  return std::nullopt;
}

std::size_t JsonKeyCount(std::string_view encoded, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  std::size_t count = 0;
  std::size_t start = 0;
  while (start < encoded.size()) {
    const std::size_t key_pos = encoded.find(needle, start);
    if (key_pos == std::string_view::npos) break;
    std::size_t colon = key_pos + needle.size();
    while (colon < encoded.size() && std::isspace(static_cast<unsigned char>(encoded[colon]))) {
      ++colon;
    }
    if (colon < encoded.size() && encoded[colon] == ':') ++count;
    start = key_pos + needle.size();
  }
  return count;
}

std::optional<std::string> DuplicateTextField(std::string_view encoded) {
  for (const std::string_view key : {"envelope",
                                     "envelope_major",
                                     "sblr_version",
                                     "operation_id",
                                     "sblr_operation_family",
                                     "result_shape",
                                     "diagnostic_shape",
                                     "parser_resolved_names_to_uuids",
                                     "requires_cluster_authority"}) {
    if (TextFieldCount(encoded, key) > 1) return std::string(key);
  }
  return std::nullopt;
}

std::optional<std::string> DuplicateJsonField(std::string_view encoded) {
  for (const std::string_view key : {"envelope",
                                     "operation_family",
                                     "operation_id",
                                     "result_shape",
                                     "diagnostic_shape",
                                     "contains_sql_text",
                                     "source_payload_embedded",
                                     "source_text",
                                     "sql_text"}) {
    if (JsonKeyCount(encoded, key) > 1) return std::string(key);
  }
  return std::nullopt;
}

const FamilyRule* FindFamily(std::string_view family) {
  for (const auto& rule : kServerSblrFamilies) {
    if (rule.family == family) return &rule;
  }
  return nullptr;
}

bool IsFailClosedSblrFamily(std::string_view family) {
  for (const auto fail_closed_family : kFailClosedSblrFamilies) {
    if (fail_closed_family == family) return true;
  }
  return false;
}

bool IsNonPrimarySblrAuditFamily(std::string_view family) {
  for (const auto audit_family : kNonPrimarySblrAuditFamilies) {
    if (audit_family == family) return true;
  }
  return false;
}

bool IsUmbrellaSblrFamily(std::string_view family) {
  return family == "sblr.acceleration.operation.v3" ||
         family == "sblr.archive_replication.operation.v3" ||
         family == "sblr.bridge.operation.v3" ||
         family == "sblr.dml.operation.v3" ||
         family == "sblr.expression.runtime.v3" ||
         family == "sblr.general.operation.v3" ||
         family == "sblr.jobs.operation.v3" ||
         family == "sblr.management.runtime_operation.v3" ||
         family == "sblr.observability.inspect.v3" ||
         family == "sblr.query.multimodel_or_ddl.v3";
}

std::string FamilyForPublicEnvelope(scratchbird::engine::SblrOperationFamily family) {
  using scratchbird::engine::SblrOperationFamily;
  switch (family) {
    case SblrOperationFamily::relational_query: return "sblr.query.relational.v3";
    case SblrOperationFamily::dml_insert: return "sblr.dml.insert.v3";
    case SblrOperationFamily::dml_update: return "sblr.dml.update.v3";
    case SblrOperationFamily::dml_delete: return "sblr.dml.delete.v3";
    case SblrOperationFamily::dml_merge: return "sblr.dml.merge.v3";
    case SblrOperationFamily::bulk_import: return "sblr.bulk.import.v3";
    case SblrOperationFamily::bulk_export: return "sblr.bulk.export.v3";
    case SblrOperationFamily::catalog_mutation: return "sblr.catalog.mutation.v3";
    case SblrOperationFamily::security_mutation: return "sblr.security.mutation.v3";
    case SblrOperationFamily::transaction_control: return "sblr.transaction.control.v3";
    case SblrOperationFamily::management_inspect: return "sblr.management.report.v3";
    case SblrOperationFamily::management_control: return "sblr.management.control.v3";
    case SblrOperationFamily::metrics_inspect: return "sblr.metrics.read.v3";
    case SblrOperationFamily::replication_operation:
      return "sblr.replication.operation.v3";
    case SblrOperationFamily::structured_kv: return "sblr.query.kv.v3";
    case SblrOperationFamily::document: return "sblr.query.document.v3";
    case SblrOperationFamily::graph: return "sblr.query.graph.v3";
    case SblrOperationFamily::search: return "sblr.query.search.v3";
    case SblrOperationFamily::vector: return "sblr.query.vector.v3";
    case SblrOperationFamily::timeseries: return "sblr.query.timeseries.v3";
    case SblrOperationFamily::versioned_history:
    case SblrOperationFamily::cluster_placement:
    case SblrOperationFamily::acceleration_management:
    case SblrOperationFamily::reference_meta: break;
  }
  return {};
}

std::string OperationForLegacyEnvelope(std::string_view encoded) {
  if (Contains(encoded, "warning_chain_count") || Contains(encoded, "partial_result_rows")) {
    return "dml.partial_result";
  }
  if (Contains(encoded, "multi_result_count")) return "dml.multi_result";
  if (Contains(encoded, "copy_stream_kind") || Contains(encoded, "copy_import") ||
      Contains(encoded, "copy_statement")) {
    return "dml.copy_import";
  }
  if (Contains(encoded, "copy_export")) return "dml.copy_export";
  if (Contains(encoded, "load_data")) return "dml.load_data";
  if (Contains(encoded, "show.version")) return "observability.show_version";
  if (Contains(encoded, "show.database")) return "observability.show_database";
  if (Contains(encoded, "crud.insert") || Contains(encoded, "dml.insert")) return "dml.insert";
  if (Contains(encoded, "crud.update") || Contains(encoded, "dml.update")) return "dml.update";
  if (Contains(encoded, "crud.delete") || Contains(encoded, "dml.delete")) return "dml.delete";
  if (Contains(encoded, "crud.select") || Contains(encoded, "query.relational") ||
      Contains(encoded, "query.values")) {
    return "dml.select";
  }
  if (Contains(encoded, "catalog.create_table")) return "catalog.create_table";
  if (Contains(encoded, "catalog.get_descriptor")) return "catalog.get_descriptor";
  if (Contains(encoded, "index.create")) return "index.create";
  if (Contains(encoded, "index.rebuild")) return "index.rebuild";
  if (Contains(encoded, "index.rebalance")) return "index.rebalance";
  if (Contains(encoded, "index.verify")) return "index.verify";
  if (Contains(encoded, "index.validate")) return "index.validate";
  if (Contains(encoded, "index.repair")) return "index.repair";
  if (Contains(encoded, "index.discard_unpublished")) return "index.discard_unpublished";
  if (Contains(encoded, "index.gather_statistics")) return "index.gather_statistics";
  if (Contains(encoded, "index.cleanup_mga_versions")) return "index.cleanup_mga_versions";
  if (Contains(encoded, "datatype.cast")) return "datatype.cast";
  if (Contains(encoded, "datatype.extract")) return "datatype.extract";
  if (Contains(encoded, "datatype.set")) return "datatype.set";
  if (Contains(encoded, "optimizer.explain")) return "optimizer.explain";
  if (Contains(encoded, "optimizer.plan")) return "optimizer.plan";
  if (Contains(encoded, "llvm.compile")) return "llvm.compile";
  if (Contains(encoded, "transaction.begin")) return "transaction.begin";
  if (Contains(encoded, "event.channel.create")) return "event.channel.create";
  if (Contains(encoded, "event.channel.listen")) return "event.channel.listen";
  if (Contains(encoded, "event.channel.unlisten")) return "event.channel.unlisten";
  if (Contains(encoded, "event.channel.notify")) return "event.channel.notify";
  if (Contains(encoded, "event.subscription.list")) return "event.subscription.list";
  if (Contains(encoded, "event.delivery.poll")) return "event.delivery.poll";
  if (Contains(encoded, "event.delivery.ack")) return "event.delivery.ack";
  return "sblr.dispatch";
}

std::optional<std::string> FamilyForLegacyEnvelope(std::string_view encoded) {
  if (Contains(encoded, "sblr.bridge") || Contains(encoded, "bridge.")) {
    return "sblr.bridge.operation.v3";
  }
  if (Contains(encoded, "sblr.cluster.control") || Contains(encoded, "cluster.control") ||
      Contains(encoded, "cluster.route")) {
    return "sblr.cluster.control.v3";
  }
  if (Contains(encoded, "sblr.cluster.report") || Contains(encoded, "cluster.inspect") ||
      Contains(encoded, "cluster.show")) {
    return "sblr.cluster.report.v3";
  }
  if (Contains(encoded, "sblr.query.relational") || Contains(encoded, "sblr.query.values") ||
      Contains(encoded, "sblr.crud.select")) {
    return "sblr.query.relational.v3";
  }
  if (Contains(encoded, "sblr.dml.insert") || Contains(encoded, "sblr.crud.insert")) {
    return "sblr.dml.insert.v3";
  }
  if (Contains(encoded, "sblr.dml.update") || Contains(encoded, "sblr.crud.update")) {
    return "sblr.dml.update.v3";
  }
  if (Contains(encoded, "sblr.dml.delete") || Contains(encoded, "sblr.crud.delete")) {
    return "sblr.dml.delete.v3";
  }
  if (Contains(encoded, "sblr.dml.merge")) {
    return "sblr.dml.merge.v3";
  }
  if (Contains(encoded, "sblr.index")) {
    return "sblr.index.maintenance.v3";
  }
  if (Contains(encoded, "catalog.get_descriptor") || Contains(encoded, "sblr.catalog.introspect")) {
    return "sblr.catalog.introspect.v3";
  }
  if (Contains(encoded, "sblr.catalog")) {
    return "sblr.catalog.mutation.v3";
  }
  if (Contains(encoded, "sblr.optimizer")) {
    return "sblr.optimizer.plan.v3";
  }
  if (Contains(encoded, "sblr.datatype")) {
    return "sblr.query.relational.v3";
  }
  if (Contains(encoded, "sblr.llvm")) {
    return "sblr.acceleration.llvm.v3";
  }
  if (Contains(encoded, "sblr.gpu")) {
    return "sblr.acceleration.gpu.v3";
  }
  if (Contains(encoded, "sblr.query.show") || Contains(encoded, "sblr.management.show")) {
    return "sblr.management.report.v3";
  }
  if (Contains(encoded, "sblr.transaction")) {
    return "sblr.transaction.control.v3";
  }
  if (Contains(encoded, "sblr.event.channel")) {
    return "sblr.event.channel.v3";
  }
  if (Contains(encoded, "sblr.event.subscription")) {
    return "sblr.event.subscription.v3";
  }
  if (Contains(encoded, "sblr.event.delivery")) {
    return "sblr.event.delivery.v3";
  }
  if (Contains(encoded, "sblr.session.cursor")) {
    return "sblr.cursor.operation.v3";
  }
  if (Contains(encoded, "sblr.session")) {
    return "sblr.session.management.v3";
  }
  return std::nullopt;
}

std::optional<std::string> FamilyForOperationId(std::string_view operation_id) {
  if (operation_id.starts_with("bridge.")) {
    return "sblr.bridge.operation.v3";
  }
  if (IsPublicExactOperationId(operation_id)) {
    const std::string family = PublicExactFamilyForOperationId(operation_id);
    if (!family.empty()) return family;
    return std::nullopt;
  }
  if (IsClusterOperationId(operation_id)) {
    return FamilyForClusterOperationId(operation_id);
  }
  if (operation_id.starts_with("archive.")) {
    return "sblr.archive.operation.v3";
  }
  if (operation_id.starts_with("backup_archive.")) {
    return "sblr.backup.operation.v3";
  }
  if (operation_id.starts_with("backup.")) {
    return "sblr.backup.operation.v3";
  }
  if (operation_id.starts_with("replication.")) {
    return "sblr.replication.operation.v3";
  }
  if (operation_id.starts_with("jobs.")) {
    if (operation_id.find(".cancel") != std::string_view::npos ||
        operation_id.find(".start") != std::string_view::npos ||
        operation_id.find(".stop") != std::string_view::npos ||
        operation_id.find(".control") != std::string_view::npos) {
      return "sblr.management.control.v3";
    }
    return "sblr.management.report.v3";
  }
  if (operation_id.starts_with("language.")) {
    return "sblr.language.resource_control.v3";
  }
  if (operation_id.starts_with("index.")) return "sblr.index.maintenance.v3";
  if (operation_id == "catalog.get_descriptor" ||
      operation_id == "catalog.resolve_name" ||
      operation_id == "catalog.map_uuid_to_name" ||
      operation_id == "catalog.lookup_object" ||
      operation_id == "catalog.list_children" ||
      operation_id == "catalog.get_dependencies") {
    return "sblr.catalog.introspect.v3";
  }
  if (operation_id.starts_with("ddl.") ||
      operation_id.starts_with("catalog.") ||
      operation_id.starts_with("artifact.")) {
    return "sblr.catalog.mutation.v3";
  }
  if (operation_id == "dml.select" || operation_id == "dml.select_rows") {
    return "sblr.query.relational.v3";
  }
  if (operation_id == "dml.insert" || operation_id == "dml.insert_rows") {
    return "sblr.dml.insert.v3";
  }
  if (operation_id == "dml.update" || operation_id == "dml.update_rows") {
    return "sblr.dml.update.v3";
  }
  if (operation_id == "dml.delete" || operation_id == "dml.delete_rows") {
    return "sblr.dml.delete.v3";
  }
  if (operation_id == "dml.merge" || operation_id == "dml.merge_rows") {
    return "sblr.dml.merge.v3";
  }
  if (operation_id == "dml.copy_import" ||
      operation_id == "dml.load_data" ||
      operation_id == "dml.plan_import_rows" ||
      operation_id == "dml.execute_import_rows" ||
      operation_id == "dml.execute_native_bulk_ingest" ||
      operation_id == "dml.normalize_import_checkpoint_model" ||
      operation_id == "dml.normalize_import_reject_model") {
    return "sblr.bulk.import.v3";
  }
  if (operation_id == "dml.copy_export") {
    return "sblr.bulk.export.v3";
  }
  if (operation_id == "dml.multi_result" ||
      operation_id == "dml.partial_result") {
    return "sblr.dml.operation.v3";
  }
  if (operation_id.starts_with("dml.")) return std::nullopt;
  if (operation_id.starts_with("transaction.")) return "sblr.transaction.control.v3";
  if (operation_id == "storage.manage_operation" ||
      IsStorageTierMigrationOperationId(operation_id) ||
      operation_id.starts_with("filespace.") ||
      operation_id.starts_with("storage.filespace.") ||
      operation_id.starts_with("storage.file_space.")) {
    return "sblr.filespace.management.v3";
  }
  if (operation_id.starts_with("storage.index.")) return "sblr.index.maintenance.v3";
  if (operation_id.starts_with("storage.database.")) return "sblr.database.management.v3";
  if (operation_id.starts_with("security.policy.") ||
      operation_id == "security.evaluate_visibility" ||
      operation_id == "security.evaluate_policy" ||
      operation_id == "security.evaluate_deep_enforcement") {
    return "sblr.policy.operation.v3";
  }
  if (operation_id.starts_with("security.")) return "sblr.security.mutation.v3";
  if (operation_id == "observability.show_metrics") return "sblr.metrics.read.v3";
  if (operation_id == "observability.show_transactions") return "sblr.mga.report.v3";
  if (operation_id == "observability.show_catalog") return "sblr.catalog.introspect.v3";
  if (operation_id == "observability.show_diagnostics" ||
      operation_id == "observability.show_diagnostics_extended") {
    return "sblr.diagnostic.control.v3";
  }
  if (operation_id == "observability.show_filespace_extended") {
    return "sblr.filespace.management.v3";
  }
  if (operation_id == "observability.show_archive_replication") {
    return "sblr.replication.consumer.v3";
  }
  if (operation_id == "observability.show_acceleration" ||
      operation_id == "observability.show_acceleration_extended" ||
      operation_id.starts_with("observability.show_")) {
    return "sblr.management.report.v3";
  }
  if (operation_id == "observability.explain_operation") return "sblr.optimizer.plan.v3";
  if (operation_id.starts_with("observability.")) return "sblr.management.report.v3";
  if (operation_id == "management.control_runtime" ||
      operation_id == "management.set_config" ||
      operation_id == "management.reset_config" ||
      operation_id == "management.prepare_support_bundle" ||
      operation_id.starts_with("agents.")) {
    return "sblr.management.control.v3";
  }
  if (operation_id.starts_with("memory.")) {
    return IsMemoryControlOperationId(operation_id) ? "sblr.management.control.v3"
                                                   : "sblr.management.report.v3";
  }
  if (operation_id.starts_with("storage_tier.")) {
    return "sblr.filespace.management.v3";
  }
  if (operation_id.starts_with("general.")) {
    return "sblr.management.control.v3";
  }
  if (operation_id.starts_with("filespaces.") || operation_id.starts_with("pages.")) {
    return "sblr.management.report.v3";
  }
  if (operation_id == "management.inspect_runtime" ||
      operation_id == "management.inspect_config") {
    return "sblr.management.report.v3";
  }
  if (operation_id.starts_with("lifecycle.")) {
    return "sblr.database.management.v3";
  }
  if (operation_id.starts_with("nosql.key_value") ||
      operation_id.starts_with("nosql.kv")) {
    return "sblr.query.kv.v3";
  }
  if (operation_id.starts_with("nosql.document")) return "sblr.query.document.v3";
  if (operation_id.starts_with("nosql.graph")) return "sblr.query.graph.v3";
  if (operation_id.starts_with("nosql.search")) return "sblr.query.search.v3";
  if (operation_id.starts_with("nosql.vector")) return "sblr.query.vector.v3";
  if (operation_id.starts_with("nosql.time_series") ||
      operation_id.starts_with("nosql.timeseries")) {
    return "sblr.query.timeseries.v3";
  }
  if (operation_id.starts_with("nosql.fulltext")) return "sblr.fulltext.execution.v3";
  if (operation_id.starts_with("nosql.")) return std::nullopt;
  if (operation_id == "query.plan_operation") return "sblr.optimizer.plan.v3";
  if (operation_id == "query.canonicalize_document_value") {
    return "sblr.query.document.v3";
  }
  if (operation_id.starts_with("query.")) return "sblr.query.relational.v3";
  if (operation_id.starts_with("event.channel.")) return "sblr.event.channel.v3";
  if (operation_id.starts_with("event.subscription.")) return "sblr.event.subscription.v3";
  if (operation_id.starts_with("event.delivery.")) return "sblr.event.delivery.v3";
  if (operation_id.starts_with("event.publication.")) return "sblr.event.publication.v3";
  if (operation_id.starts_with("session.cursor_")) {
    return "sblr.cursor.operation.v3";
  }
  if (operation_id.starts_with("session.notification.") ||
      operation_id.starts_with("session.prepare") ||
      operation_id.starts_with("session.statement")) {
    return "sblr.session.management.v3";
  }
  if (operation_id.starts_with("session.")) return "sblr.session.management.v3";
  if (operation_id.starts_with("extensibility.register_udr_package") ||
      operation_id.starts_with("extensibility.load_udr_package") ||
      operation_id.starts_with("extensibility.unload_udr_package") ||
      operation_id.starts_with("extensibility.inspect_udr_packages") ||
      operation_id.starts_with("extensibility.invoke_udr_package")) {
    return "sblr.udr.operation.v3";
  }
  if (operation_id == "extensibility.register_parser_package") {
    return "sblr.parser.operation.v3";
  }
  if (operation_id.starts_with("extensibility.compile_llvm") ||
      operation_id.starts_with("llvm.")) {
    return "sblr.acceleration.llvm.v3";
  }
  if (operation_id.starts_with("extensibility.inspect_gpu") ||
      operation_id.starts_with("gpu.")) {
    return "sblr.acceleration.gpu.v3";
  }
  if (operation_id.starts_with("routine.define")) return "sblr.routine.define.v3";
  if (operation_id.starts_with("routine.execute")) return "sblr.routine.execute.v3";
  if (operation_id.starts_with("diagnostic.refusal")) return "sblr.diagnostic.refusal.v3";
  if (operation_id.starts_with("diagnostic.")) return "sblr.diagnostic.control.v3";
  if (operation_id.starts_with("mga.control.")) return "sblr.mga.control.v3";
  if (operation_id.starts_with("mga.report.")) return "sblr.mga.report.v3";
  return std::nullopt;
}

std::uint64_t RowCountHint(std::string_view operation_id, std::string_view family) {
  if (family == "sblr.query.relational.v3" || family == "sblr.management.report.v3") return 1;
  return Contains(operation_id, "select") || Contains(operation_id, "show") ||
                 Contains(operation_id, "descriptor") || Contains(operation_id, "explain") ||
                 Contains(operation_id, "plan") || Contains(operation_id, "inspect")
             ? 1
             : 0;
}

bool LooksLikeBinarySblrEnvelope(std::string_view encoded) {
  return encoded.size() >= 4 && encoded[0] == 'S' && encoded[1] == 'B' &&
         encoded[2] == 'L' && encoded[3] == 'R';
}

ServerSblrAdmissionResult Reject(std::string code, std::string message, std::string detail) {
  ServerSblrAdmissionResult result;
  result.diagnostics.push_back(AdmissionDiagnostic(std::move(code), std::move(message), std::move(detail)));
  return result;
}

ServerSblrAdmissionResult RejectFamilyReconciliationRequired(std::string_view detail) {
  return Reject("SBLR.FAMILY_RECONCILIATION_REQUIRED",
                "SBLR operation families must resolve to declared primary envelope families before admission.",
                std::string(detail));
}

std::string ReconciledExplicitServerFamily(std::string family,
                                           std::string_view operation_id,
                                           bool prefer_primary_family,
                                           bool preserve_query_plan_route_family) {
  (void)preserve_query_plan_route_family;
  if (family == "sblr.dml.operation.v3") {
    const auto resolved_family = FamilyForOperationId(operation_id);
    if (resolved_family.has_value()) return *resolved_family;
    if (operation_id == "dml.insert_rows") return "sblr.dml.insert.v3";
    if (operation_id == "dml.update_rows") return "sblr.dml.update.v3";
    if (operation_id == "dml.delete_rows") return "sblr.dml.delete.v3";
    if (operation_id == "dml.merge_rows") return "sblr.dml.merge.v3";
    if (operation_id == "dml.plan_import_rows") return "sblr.bulk.import.v3";
  }
  if (family == "sblr.acceleration.operation.v3") {
    const auto resolved_family = FamilyForOperationId(operation_id);
    if (resolved_family == "sblr.acceleration.gpu.v3" ||
        resolved_family == "sblr.acceleration.llvm.v3") {
      return *resolved_family;
    }
  }
  if (operation_id == "cluster.inspect_provider" &&
      family == "sblr.cluster.private_operation.v3") {
    return "sblr.cluster.report.v3";
  }
  if (family == "sblr.management.runtime_operation.v3" &&
      operation_id.starts_with("lifecycle.")) {
    const auto resolved_family = FamilyForOperationId(operation_id);
    if (resolved_family.has_value()) return *resolved_family;
  }
  if (operation_id == "query.plan_operation" &&
      (family == "sblr.query.relational.v3" ||
       family == "sblr.query.multimodel_or_ddl.v3")) {
    return family;
  }
  if (prefer_primary_family &&
      (family == "sblr.management.runtime_operation.v3" ||
       family == "sblr.observability.inspect.v3")) {
    const auto resolved_family = FamilyForOperationId(operation_id);
    if (resolved_family.has_value()) return *resolved_family;
  }
  if (prefer_primary_family && family == "sblr.cluster.private_operation.v3") {
    const auto resolved_family = FamilyForClusterOperationId(operation_id);
    if (resolved_family.has_value()) return *resolved_family;
  }
  return family;
}

ServerSblrAdmissionResult AdmitFamily(std::string family,
                                      std::string operation_id,
                                      bool public_abi_dispatch,
                                      bool cluster_authority_active) {
  if (IsFailClosedSblrFamily(family)) {
    return RejectFamilyReconciliationRequired(family);
  }
  if (IsNonPrimarySblrAuditFamily(family)) {
    if (operation_id.empty() || !FamilyForOperationId(operation_id).has_value()) {
      return RejectFamilyReconciliationRequired(family);
    }
  }
  if (IsUmbrellaSblrFamily(family) &&
      (operation_id.empty() || !FamilyForOperationId(operation_id).has_value())) {
    return RejectFamilyReconciliationRequired(family);
  }
  const FamilyRule* rule = FindFamily(family);
  if (rule == nullptr) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "The server could not map the SBLR operation family to an admission rule.",
                  family);
  }

  ServerSblrAdmissionResult result;
  result.admitted = true;
  result.operation_family = std::move(family);
  result.operation_id =
      (operation_id.empty() || (rule->cluster_private && !IsClusterOperationId(operation_id)))
          ? std::string(rule->default_operation_id)
          : std::move(operation_id);
  result.requires_public_abi_dispatch =
      rule->cluster_private || cluster_authority_active || public_abi_dispatch ||
      RequiresEnginePublicAbiDispatch(result.operation_id);
  result.row_count_hint = RowCountHint(result.operation_id, result.operation_family);
  return result;
}

ServerSblrAdmissionResult AdmitTextOperationEnvelope(std::string_view encoded,
                                                    bool public_abi_dispatch,
                                                    bool cluster_authority_active) {
  if (ContainsSqlTextMarker(encoded)) {
    return Reject("SBLR.SQL_TEXT_FORBIDDEN",
                  "The engine accepts canonical SBLR only.",
                  "raw_sql_forbidden");
  }
  if (const auto duplicate = DuplicateTextField(encoded)) {
    return Reject("PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD",
                  "SBLR operation envelopes must not contain duplicate authority fields.",
                  *duplicate);
  }
  if (const auto envelope = TextField(encoded, "envelope");
      envelope.has_value() && envelope->starts_with("SBLRExecutionEnvelope.") &&
      *envelope != "SBLRExecutionEnvelope.v3") {
    return Reject("PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED",
                  "The SBLR envelope version is unsupported by this server.",
                  "unsupported_sblr_execution_envelope_version");
  }
  if (const auto major = TextField(encoded, "envelope_major");
      major.has_value() && *major != "3") {
    return Reject("PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED",
                  "The SBLR envelope major version is unsupported by this server.",
                  "unsupported_sblr_envelope_major");
  }
  if (const auto version = TextField(encoded, "sblr_version");
      version.has_value() && *version != "sblr_v3") {
    return Reject("PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED",
                  "The SBLR envelope version is unsupported by this server.",
                  "unsupported_sblr_version");
  }
  const auto operation_id = TextField(encoded, "operation_id").value_or("");
  if (operation_id.empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "SBLR operation envelopes require an operation_id.",
                  "operation_id_required");
  }
  if (TextField(encoded, "result_shape").value_or("").empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "SBLR operation envelopes require a result_shape.",
                  "result_shape_required");
  }
  if (TextField(encoded, "diagnostic_shape").value_or("").empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "SBLR operation envelopes require a diagnostic_shape.",
                  "diagnostic_shape_required");
  }
  if (TextField(encoded, "parser_resolved_names_to_uuids").value_or("false") != "true") {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "Parser-generated SBLR must resolve names to UUIDs before server admission.",
                  "names_not_resolved_to_uuids");
  }
  const bool requires_cluster =
      TextField(encoded, "requires_cluster_authority").value_or("false") == "true";
  if (requires_cluster && !IsClusterOperationId(operation_id)) {
    return Reject("SBLR.CLUSTER_MAPPING.UNAVAILABLE",
                  "Cluster authority cannot be attached to a non-cluster SBLR operation.",
                  "requires_cluster_authority_without_cluster_operation");
  }
  if (requires_cluster && !cluster_authority_active && operation_id == "cluster.route") {
    return Reject("SBLR.CAPABILITY.FORBIDDEN",
                  "Cluster SBLR requires a mapped cluster provider operation.",
                  "cluster_mapping_unavailable");
  }
  std::string family = TextField(encoded, "sblr_operation_family").value_or("");
  const bool prefer_primary_family =
      public_abi_dispatch ||
      Contains(encoded, "public_sbsql_exact_command=true") ||
      Contains(encoded, "engine_api_command_route=true") ||
      Contains(encoded, "cluster_provider_dispatch=true");
  const bool preserve_query_plan_route_family =
      Contains(encoded, "sbsfc081_descriptor_expression_residual=true") ||
      Contains(encoded, "sbsfc083_grammar_surface=true") ||
      Contains(encoded, "sbsfc085_grammar_surface=true");
  family = ReconciledExplicitServerFamily(std::move(family),
                                          operation_id,
                                          prefer_primary_family,
                                          preserve_query_plan_route_family);
  if (family.empty()) {
    const auto resolved_family = FamilyForOperationId(operation_id);
    if (!resolved_family.has_value()) {
      return RejectFamilyReconciliationRequired(operation_id);
    }
    family = *resolved_family;
  }
  if (requires_cluster) {
    const auto cluster_family = FamilyForClusterOperationId(operation_id);
    if (!cluster_family.has_value()) {
      return RejectFamilyReconciliationRequired(operation_id);
    }
    family = *cluster_family;
  }
  return AdmitFamily(std::move(family), operation_id, public_abi_dispatch, cluster_authority_active);
}

ServerSblrAdmissionResult AdmitParserJsonEnvelope(std::string_view encoded,
                                                  bool cluster_authority_active) {
  if (ContainsSqlTextMarker(encoded)) {
    return Reject("SBLR.SQL_TEXT_FORBIDDEN",
                  "The engine accepts canonical SBLR only.",
                  "raw_sql_forbidden");
  }
  if (const auto duplicate = DuplicateJsonField(encoded)) {
    return Reject("PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD",
                  "SBLR operation envelopes must not contain duplicate authority fields.",
                  *duplicate);
  }
  std::string family = JsonStringField(encoded, "operation_family").value_or("");
  if (family.empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "Parser SBLR envelopes require an operation_family.",
                  "operation_family_required");
  }
  if (JsonStringField(encoded, "result_shape").value_or("").empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "Parser SBLR envelopes require a result_shape.",
                  "result_shape_required");
  }
  if (JsonStringField(encoded, "diagnostic_shape").value_or("").empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "Parser SBLR envelopes require a diagnostic_shape.",
                  "diagnostic_shape_required");
  }
  std::string operation_id = JsonStringField(encoded, "operation_id").value_or("");
  if (operation_id.empty()) operation_id = OperationForLegacyEnvelope(encoded);
  const bool prefer_primary_family =
      Contains(encoded, "\"public_sbsql_exact_command\":true") ||
      Contains(encoded, "\"public_sbsql_exact_command\": true") ||
      Contains(encoded, "\"engine_api_command_route\":true") ||
      Contains(encoded, "\"engine_api_command_route\": true") ||
      Contains(encoded, "\"cluster_provider_dispatch\":true") ||
      Contains(encoded, "\"cluster_provider_dispatch\": true") ||
      Contains(encoded, "\"sbsfc077_non_general_residual\":true") ||
      Contains(encoded, "\"sbsfc077_non_general_residual\": true");
  const bool preserve_query_plan_route_family =
      Contains(encoded, "\"sbsfc081_descriptor_expression_residual\":true") ||
      Contains(encoded, "\"sbsfc081_descriptor_expression_residual\": true") ||
      Contains(encoded, "\"sbsfc083_grammar_surface\":true") ||
      Contains(encoded, "\"sbsfc083_grammar_surface\": true") ||
      Contains(encoded, "\"sbsfc085_grammar_surface\":true") ||
      Contains(encoded, "\"sbsfc085_grammar_surface\": true");
  family = ReconciledExplicitServerFamily(std::move(family),
                                          operation_id,
                                          prefer_primary_family,
                                          preserve_query_plan_route_family);
  const bool public_abi_dispatch = RequiresEnginePublicAbiDispatch(operation_id);
  return AdmitFamily(std::move(family), std::move(operation_id), public_abi_dispatch, cluster_authority_active);
}

ServerSblrAdmissionResult AdmitBinaryEnvelope(std::string_view encoded,
                                              bool cluster_authority_active) {
  const auto decoded = scratchbird::engine::DecodeSblrEnvelopeBytes(
      reinterpret_cast<const std::uint8_t*>(encoded.data()),
      static_cast<std::uint64_t>(encoded.size()));
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok) {
    return Reject(std::string(decoded.diagnostic_code.empty() ? "SBLR.ENVELOPE.INVALID"
                                                              : decoded.diagnostic_code),
                  "The SBLR envelope failed server revalidation.",
                  "binary_envelope_invalid");
  }

  std::string family = FamilyForPublicEnvelope(decoded.envelope.family);
  if (family.empty()) {
    return RejectFamilyReconciliationRequired("public_envelope_family_unresolved");
  }
  if (decoded.envelope.payload_kind == scratchbird::engine::SblrPayloadKind::operation_envelope) {
    const auto* data = reinterpret_cast<const char*>(decoded.envelope.canonical_bytes.data());
    const std::string_view text(data, decoded.envelope.canonical_bytes.size());
    auto result = AdmitTextOperationEnvelope(text, true, cluster_authority_active);
    if (result.admitted && result.operation_family.empty() && !family.empty()) {
      result.operation_family = std::move(family);
    }
    return result;
  }
  return AdmitFamily(std::move(family), {}, true, cluster_authority_active);
}

}  // namespace

ServerSblrAdmissionResult AdmitServerSblrEnvelope(
    const ServerSblrAdmissionRequest& request) {
  const std::string_view raw(request.encoded_sblr_envelope);
  if (raw.empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "The SBLR envelope payload is empty.",
                  "empty_sblr_envelope");
  }
  if (LooksLikeBinarySblrEnvelope(raw)) {
    return AdmitBinaryEnvelope(raw, request.cluster_authority_active);
  }
  const std::string encoded = Trim(raw);
  if (encoded.empty()) {
    return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                  "The SBLR envelope payload is empty.",
                  "empty_sblr_envelope");
  }
  if (LooksLikeRawSql(encoded)) {
    return Reject("SBLR.SQL_TEXT_FORBIDDEN",
                  "The engine accepts canonical SBLR only.",
                  "raw_sql_forbidden");
  }
  if (TextField(encoded, "operation_id").has_value()) {
    return AdmitTextOperationEnvelope(encoded, false, request.cluster_authority_active);
  }
  if (const auto envelope = JsonStringField(encoded, "envelope");
      envelope.has_value() && envelope->starts_with("SBLRExecutionEnvelope.")) {
    if (*envelope != "SBLRExecutionEnvelope.v3") {
      return Reject("PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED",
                    "The SBLR envelope version is unsupported by this server.",
                    "unsupported_sblr_execution_envelope_version");
    }
    return AdmitParserJsonEnvelope(encoded, request.cluster_authority_active);
  }
  if (IsFailClosedSblrFamily(encoded)) {
    return RejectFamilyReconciliationRequired(encoded);
  }
  if (IsNonPrimarySblrAuditFamily(encoded)) {
    return RejectFamilyReconciliationRequired(encoded);
  }
  if (FindFamily(encoded) != nullptr) {
    return AdmitFamily(encoded, {}, false, request.cluster_authority_active);
  }
  if (auto family = FamilyForLegacyEnvelope(encoded)) {
    return AdmitFamily(std::move(*family),
                       OperationForLegacyEnvelope(encoded),
                       false,
                       request.cluster_authority_active);
  }
  return Reject("PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
                "The server could not classify the SBLR envelope.",
                "sblr_family_unknown");
}

}  // namespace scratchbird::server
