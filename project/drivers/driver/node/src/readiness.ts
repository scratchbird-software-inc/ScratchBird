// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

export const DRIVER_READINESS_SCHEMA_VERSION = "scratchbird.driver.readiness.v1";
export const DRIVER_COMPONENT_ID = "driver:node";
export const DRIVER_PACKAGE_UUID = "019e12a0-0008-7000-8000-000000000008";
export const DRIVER_STATUS = "beta_2";
export const DRIVER_RELEASE_BUCKET = "release_candidate";
export const DRIVER_CONFORMANCE_PROFILE = "driver_node_gate";
export const DRIVER_SOURCE_PATH = "project/drivers/driver/node";
export const DRIVER_LICENSE = "MPL-2.0";
export const STANDARD_ENGLISH_LANGUAGE = "en_US";

export interface ReadinessDiagnostic {
  code: string;
  sqlstate: string;
  message: string;
}

export interface AdvisoryCacheContext {
  databaseUuid?: string;
  schemaEpoch?: string;
  policyEpoch?: string;
  languageEpoch?: string;
  capabilityEpoch?: string;
  principalUuid?: string;
  roleSetHash?: string;
  groupSetHash?: string;
  transactionUuid?: string;
}

export interface PreparedBundleContext {
  databaseUuid?: string;
  schemaEpoch?: string;
  policyEpoch?: string;
  principalUuid?: string;
  transactionUuid?: string;
  serverAdmitted?: boolean;
}

export interface LanguageProfileResolution {
  requested: string;
  selected: string;
  fallback: boolean;
  reason: string;
}

export interface LanguageResourceState {
  locale?: string;
  schemaVersion?: string;
  contentHash?: string;
  signature?: string;
  epoch?: string;
  expectedEpoch?: string;
}

export function betaDriverReadinessStatus() {
  return {
    schema_version: DRIVER_READINESS_SCHEMA_VERSION,
    component_id: DRIVER_COMPONENT_ID,
    driver_package_uuid: DRIVER_PACKAGE_UUID,
    driver_status: DRIVER_STATUS,
    release_bucket: DRIVER_RELEASE_BUCKET,
    conformance_profile_ref: DRIVER_CONFORMANCE_PROFILE,
    source_path: DRIVER_SOURCE_PATH,
    package_name: "scratchbird",
    import_path: "scratchbird",
    license: DRIVER_LICENSE,
    runtime_mapping: {
      api_surface: "language_binding",
      ingress_modes: ["direct_listener", "manager_proxy"],
      wire_protocols: ["sbwp_v1_1"],
      dsn_keys: ["database", "host", "port", "user", "auth_method"],
      auth_methods: ["engine_local_password", "scram_ready"],
      tls_profile: "scratchbird_tls_1_3_floor",
      type_mapping_profile: "sbsql_core",
      diagnostic_mapping_profile: "native_sqlstate",
      metadata_profile: "sys_information_recursive",
      thread_safety_class: "thread_safe",
      pooling_capability: "connection_pool",
    },
    authority_boundary: {
      local_sblr_is_advisory: true,
      local_uuid_cache_is_advisory: true,
      local_result_cache_is_advisory: true,
      server_revalidation_required: true,
      transaction_finality_owner: "engine_mga_transaction_inventory",
      language_fallback_profile: STANDARD_ENGLISH_LANGUAGE,
      cache_invalidation_requirement: "policy_schema_language_capability_transaction_epoch",
    },
  };
}

export function validateAdvisoryCacheContext(
  cached: AdvisoryCacheContext,
  current: AdvisoryCacheContext,
): ReadinessDiagnostic | undefined {
  const checks: Array<[string, string | undefined, string | undefined, string]> = [
    ["database_uuid", cached.databaseUuid, current.databaseUuid, "SB_DRIVER_CACHE_DATABASE_MISMATCH"],
    ["schema_epoch", cached.schemaEpoch, current.schemaEpoch, "SB_DRIVER_CACHE_SCHEMA_EPOCH_STALE"],
    ["policy_epoch", cached.policyEpoch, current.policyEpoch, "SB_DRIVER_CACHE_POLICY_EPOCH_STALE"],
    ["language_epoch", cached.languageEpoch, current.languageEpoch, "SB_DRIVER_CACHE_LANGUAGE_EPOCH_STALE"],
    ["capability_epoch", cached.capabilityEpoch, current.capabilityEpoch, "SB_DRIVER_CACHE_CAPABILITY_EPOCH_STALE"],
    ["principal_uuid", cached.principalUuid, current.principalUuid, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"],
    ["role_set_hash", cached.roleSetHash, current.roleSetHash, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"],
    ["group_set_hash", cached.groupSetHash, current.groupSetHash, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"],
  ];
  for (const [name, observed, expected, code] of checks) {
    if ((observed ?? "") !== (expected ?? "")) {
      return {
        code,
        sqlstate: "42501",
        message: `advisory cache entry refused: ${name} does not match current context`,
      };
    }
  }
  if ((cached.transactionUuid ?? "") || (current.transactionUuid ?? "")) {
    if ((cached.transactionUuid ?? "") !== (current.transactionUuid ?? "")) {
      return {
        code: "SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH",
        sqlstate: "25001",
        message: "advisory cache entry refused: transaction context does not match current MGA boundary",
      };
    }
  }
  return undefined;
}

export function validatePreparedBundleReuse(
  bundle: PreparedBundleContext,
  current: AdvisoryCacheContext,
): ReadinessDiagnostic | undefined {
  if (!bundle.serverAdmitted) {
    return {
      code: "SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED",
      sqlstate: "0A000",
      message: "driver-prepared SBLR/UUID bundle is advisory until server admission succeeds",
    };
  }
  return validateAdvisoryCacheContext(
    {
      databaseUuid: bundle.databaseUuid,
      schemaEpoch: bundle.schemaEpoch,
      policyEpoch: bundle.policyEpoch,
      principalUuid: bundle.principalUuid,
      transactionUuid: bundle.transactionUuid,
    },
    {
      databaseUuid: current.databaseUuid,
      schemaEpoch: current.schemaEpoch,
      policyEpoch: current.policyEpoch,
      principalUuid: current.principalUuid,
      transactionUuid: current.transactionUuid,
    },
  );
}

export function resolveLanguageProfile(
  requested: string,
  available: Record<string, boolean> = {},
): LanguageProfileResolution {
  const selectedRequest = requested || STANDARD_ENGLISH_LANGUAGE;
  if (available[selectedRequest]) {
    return { requested: selectedRequest, selected: selectedRequest, fallback: false, reason: "" };
  }
  return {
    requested: selectedRequest,
    selected: STANDARD_ENGLISH_LANGUAGE,
    fallback: true,
    reason: "unsupported_or_unavailable_language_profile",
  };
}

export function validateLanguageResourceState(state: LanguageResourceState): ReadinessDiagnostic | undefined {
  if (!state.locale || !state.schemaVersion || !state.contentHash || !state.signature) {
    return {
      code: "SB_DRIVER_LANGUAGE_RESOURCE_INCOMPLETE",
      sqlstate: "0A000",
      message: "language resource refused: locale, schema version, content hash, and signature are required",
    };
  }
  if (state.expectedEpoch && state.epoch !== state.expectedEpoch) {
    return {
      code: "SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE",
      sqlstate: "0A000",
      message: "language resource refused: language epoch does not match current context",
    };
  }
  return undefined;
}
