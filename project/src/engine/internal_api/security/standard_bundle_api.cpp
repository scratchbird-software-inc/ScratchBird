// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/standard_bundle_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "security/security_model.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_STANDARD_BUNDLE_API_BEHAVIOR
EngineSeedStandardSecurityBundlesResult EngineSeedStandardSecurityBundles(
    const EngineSeedStandardSecurityBundlesRequest& request) {
  if (!SecurityContextHasRight(request.context, "SEC_GRANT_ADMIN")) {
    return SecurityFailure<EngineSeedStandardSecurityBundlesResult>(
        request.context,
        "security.seed_standard_bundles",
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "SEC_GRANT_ADMIN"));
  }
  static constexpr const char* kGroups[] = {"PUBLIC", "APP", "DEV", "ANL", "ETL", "SCH", "DBA", "AUD", "SUP", "OPS", "SEC", "ROOT"};
  static constexpr const char* kRoles[] = {"ROLE_APP_RUNTIME", "ROLE_DBA", "ROLE_SECURITY_ADMIN", "ROLE_AUDIT_READER", "ROLE_OPERATOR"};
  static constexpr const char* kPolicies[] = {
      "policy.catalog.bootstrap",
      "database.identity",
      "database.create.failure_cleanup",
      "database.bootstrap.tx1",
      "database.first_open.tx2_activation",
      "schema.bootstrap.roots",
      "security.authority_selection",
      "security.authentication_provider",
      "security.bootstrap_password",
      "security.authorization_default",
      "security.principal_role_group_seed",
      "security.user_home_schema",
      "security.audit",
      "security.redaction",
      "security.protected_material",
      "security.encryption_key_admission",
      "configuration.source_precedence",
      "configuration.override_reload",
      "resource.seed_i18n",
      "resource.signature_provenance",
      "storage.filespace_profile",
      "storage.filespace_lifecycle",
      "storage.allocation_freespace_pagemap",
      "lifecycle.ownership_stale_owner",
      "lifecycle.recovery_dirty_open",
      "lifecycle.maintenance_restricted",
      "lifecycle.shutdown_graceful_drain",
      "lifecycle.shutdown_force",
      "transaction.admission",
      "transaction.default_isolation_snapshot",
      "transaction.commit_durability",
      "transaction.rollback_savepoint_limbo",
      "transaction.mga_gc_retention",
      "concurrency.lock_wait_deadlock",
      "cache.checkpoint_preload_flush",
      "backup.archive_restore_snapshot_shadow",
      "workload.resource_quota",
      "temp.spill_workspace",
      "session.disconnect_timeout",
      "server.route_listener_startup",
      "listener.bind_tls_pool",
      "parser.package_admission",
      "ipc.frame_auth_backpressure",
      "udr.extension_trust_resource",
      "executable.side_effect",
      "sequence.generator_cache",
      "event.queue_notification",
      "diagnostics.message_vector",
      "observability.metrics_log",
      "support.bundle",
      "evidence.retention",
      "job.scheduler",
      "capability.feature_gate",
      "upgrade.migration_refusal",
      "admin.management_command_authorization",
      "reference.emulation_profile",
      "replication.cdc_changefeed_boundary",
      "cluster.boundary_fail_closed"};
  auto result = SecuritySuccess<EngineSeedStandardSecurityBundlesResult>(request.context, "security.seed_standard_bundles");
  for (const auto* group : kGroups) {
    EngineApiRequest record_request = request;
    record_request.localized_names.push_back({"en", "default", group, group, true});
    record_request.option_envelopes.push_back(std::string("bundle_group:") + group);
    const auto persisted = PersistApiBehaviorRecord(record_request, "security.seed_standard_bundles", "security_group", true, "active");
    if (!persisted.ok) { return SecurityFailure<EngineSeedStandardSecurityBundlesResult>(request.context, "security.seed_standard_bundles", persisted.diagnostic); }
    ++result.groups_seeded;
  }
  for (const auto* role : kRoles) {
    EngineApiRequest record_request = request;
    record_request.localized_names.push_back({"en", "default", role, role, true});
    record_request.option_envelopes.push_back(std::string("bundle_role:") + role);
    const auto persisted = PersistApiBehaviorRecord(record_request, "security.seed_standard_bundles", "security_role", true, "active");
    if (!persisted.ok) { return SecurityFailure<EngineSeedStandardSecurityBundlesResult>(request.context, "security.seed_standard_bundles", persisted.diagnostic); }
    ++result.roles_seeded;
  }
  for (const auto* policy : kPolicies) {
    EngineApiRequest record_request = request;
    record_request.localized_names.push_back({"en", "default", policy, policy, true});
    record_request.option_envelopes.push_back(std::string("bundle_policy:") + policy);
    const auto persisted = PersistApiBehaviorRecord(record_request, "security.seed_standard_bundles", "security_policy", true, "active");
    if (!persisted.ok) { return SecurityFailure<EngineSeedStandardSecurityBundlesResult>(request.context, "security.seed_standard_bundles", persisted.diagnostic); }
    ++result.policies_seeded;
  }
  AddSecurityEvidence(&result, "standard_security_bundles", "seeded");
  AddSecurityRow(&result, {{"groups_seeded", std::to_string(result.groups_seeded)},
                           {"roles_seeded", std::to_string(result.roles_seeded)},
                           {"policies_seeded", std::to_string(result.policies_seeded)}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
