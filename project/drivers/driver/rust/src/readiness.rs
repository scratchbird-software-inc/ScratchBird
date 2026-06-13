// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

pub const DRIVER_READINESS_SCHEMA_VERSION: &str = "scratchbird.driver.readiness.v1";
pub const DRIVER_COMPONENT_ID: &str = "driver:rust";
pub const DRIVER_PACKAGE_UUID: &str = "019e12a0-0015-7000-8000-000000000015";
pub const DRIVER_STATUS: &str = "beta_2";
pub const DRIVER_RELEASE_BUCKET: &str = "release_candidate";
pub const DRIVER_CONFORMANCE_PROFILE: &str = "driver_rust_gate";
pub const DRIVER_SOURCE_PATH: &str = "project/drivers/driver/rust";
pub const DRIVER_LICENSE: &str = "MPL-2.0";
pub const STANDARD_ENGLISH_LANGUAGE: &str = "en_US";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReadinessDiagnostic {
    pub code: &'static str,
    pub sqlstate: &'static str,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DriverRuntimeMapping {
    pub api_surface: &'static str,
    pub ingress_modes: Vec<&'static str>,
    pub wire_protocols: Vec<&'static str>,
    pub dsn_keys: Vec<&'static str>,
    pub auth_methods: Vec<&'static str>,
    pub tls_profile: &'static str,
    pub type_mapping_profile: &'static str,
    pub diagnostic_mapping_profile: &'static str,
    pub metadata_profile: &'static str,
    pub thread_safety_class: &'static str,
    pub pooling_capability: &'static str,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DriverAuthorityBoundary {
    pub local_sblr_is_advisory: bool,
    pub local_uuid_cache_is_advisory: bool,
    pub local_result_cache_is_advisory: bool,
    pub server_revalidation_required: bool,
    pub transaction_finality_owner: &'static str,
    pub language_fallback_profile: &'static str,
    pub cache_invalidation_requirement: &'static str,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BetaReadinessStatus {
    pub schema_version: &'static str,
    pub component_id: &'static str,
    pub driver_package_uuid: &'static str,
    pub driver_status: &'static str,
    pub release_bucket: &'static str,
    pub conformance_profile_ref: &'static str,
    pub source_path: &'static str,
    pub package_name: &'static str,
    pub import_path: &'static str,
    pub license: &'static str,
    pub runtime_mapping: DriverRuntimeMapping,
    pub authority_boundary: DriverAuthorityBoundary,
}

pub fn beta_driver_readiness_status() -> BetaReadinessStatus {
    BetaReadinessStatus {
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
        runtime_mapping: DriverRuntimeMapping {
            api_surface: "language_binding",
            ingress_modes: vec!["direct_listener", "manager_proxy"],
            wire_protocols: vec!["sbwp_v1_1"],
            dsn_keys: vec!["database", "host", "port", "user", "auth_method"],
            auth_methods: vec!["engine_local_password", "scram_ready"],
            tls_profile: "scratchbird_tls_1_3_floor",
            type_mapping_profile: "sbsql_core",
            diagnostic_mapping_profile: "native_sqlstate",
            metadata_profile: "sys_information_recursive",
            thread_safety_class: "thread_safe",
            pooling_capability: "connection_pool",
        },
        authority_boundary: DriverAuthorityBoundary {
            local_sblr_is_advisory: true,
            local_uuid_cache_is_advisory: true,
            local_result_cache_is_advisory: true,
            server_revalidation_required: true,
            transaction_finality_owner: "engine_mga_transaction_inventory",
            language_fallback_profile: STANDARD_ENGLISH_LANGUAGE,
            cache_invalidation_requirement: "policy_schema_language_capability_transaction_epoch",
        },
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AdvisoryCacheContext {
    pub database_uuid: String,
    pub schema_epoch: String,
    pub policy_epoch: String,
    pub language_epoch: String,
    pub capability_epoch: String,
    pub principal_uuid: String,
    pub role_set_hash: String,
    pub group_set_hash: String,
    pub transaction_uuid: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PreparedBundleContext {
    pub database_uuid: String,
    pub schema_epoch: String,
    pub policy_epoch: String,
    pub principal_uuid: String,
    pub transaction_uuid: String,
    pub server_admitted: bool,
}

pub fn validate_advisory_cache_context(
    cached: &AdvisoryCacheContext,
    current: &AdvisoryCacheContext,
) -> Option<ReadinessDiagnostic> {
    let checks = [
        (
            "database_uuid",
            cached.database_uuid.as_str(),
            current.database_uuid.as_str(),
            "SB_DRIVER_CACHE_DATABASE_MISMATCH",
        ),
        (
            "schema_epoch",
            cached.schema_epoch.as_str(),
            current.schema_epoch.as_str(),
            "SB_DRIVER_CACHE_SCHEMA_EPOCH_STALE",
        ),
        (
            "policy_epoch",
            cached.policy_epoch.as_str(),
            current.policy_epoch.as_str(),
            "SB_DRIVER_CACHE_POLICY_EPOCH_STALE",
        ),
        (
            "language_epoch",
            cached.language_epoch.as_str(),
            current.language_epoch.as_str(),
            "SB_DRIVER_CACHE_LANGUAGE_EPOCH_STALE",
        ),
        (
            "capability_epoch",
            cached.capability_epoch.as_str(),
            current.capability_epoch.as_str(),
            "SB_DRIVER_CACHE_CAPABILITY_EPOCH_STALE",
        ),
        (
            "principal_uuid",
            cached.principal_uuid.as_str(),
            current.principal_uuid.as_str(),
            "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH",
        ),
        (
            "role_set_hash",
            cached.role_set_hash.as_str(),
            current.role_set_hash.as_str(),
            "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH",
        ),
        (
            "group_set_hash",
            cached.group_set_hash.as_str(),
            current.group_set_hash.as_str(),
            "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH",
        ),
    ];
    for (name, observed, expected, code) in checks {
        if observed != expected {
            return Some(ReadinessDiagnostic {
                code,
                sqlstate: "42501",
                message: format!(
                    "advisory cache entry refused: {name} does not match current context"
                ),
            });
        }
    }
    if (!cached.transaction_uuid.is_empty() || !current.transaction_uuid.is_empty())
        && cached.transaction_uuid != current.transaction_uuid
    {
        return Some(ReadinessDiagnostic {
            code: "SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH",
            sqlstate: "25001",
            message: "advisory cache entry refused: transaction context does not match current MGA boundary"
                .to_string(),
        });
    }
    None
}

pub fn validate_prepared_bundle_reuse(
    bundle: &PreparedBundleContext,
    current: &AdvisoryCacheContext,
) -> Option<ReadinessDiagnostic> {
    if !bundle.server_admitted {
        return Some(ReadinessDiagnostic {
            code: "SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED",
            sqlstate: "0A000",
            message: "driver-prepared SBLR/UUID bundle is advisory until server admission succeeds"
                .to_string(),
        });
    }
    validate_advisory_cache_context(
        &AdvisoryCacheContext {
            database_uuid: bundle.database_uuid.clone(),
            schema_epoch: bundle.schema_epoch.clone(),
            policy_epoch: bundle.policy_epoch.clone(),
            principal_uuid: bundle.principal_uuid.clone(),
            transaction_uuid: bundle.transaction_uuid.clone(),
            ..AdvisoryCacheContext::default()
        },
        &AdvisoryCacheContext {
            database_uuid: current.database_uuid.clone(),
            schema_epoch: current.schema_epoch.clone(),
            policy_epoch: current.policy_epoch.clone(),
            principal_uuid: current.principal_uuid.clone(),
            transaction_uuid: current.transaction_uuid.clone(),
            ..AdvisoryCacheContext::default()
        },
    )
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LanguageProfileResolution {
    pub requested: String,
    pub selected: String,
    pub fallback: bool,
    pub reason: String,
}

pub fn resolve_language_profile(requested: &str, available: &[&str]) -> LanguageProfileResolution {
    let selected_request = if requested.is_empty() {
        STANDARD_ENGLISH_LANGUAGE
    } else {
        requested
    };
    if available
        .iter()
        .any(|candidate| *candidate == selected_request)
    {
        return LanguageProfileResolution {
            requested: selected_request.to_string(),
            selected: selected_request.to_string(),
            fallback: false,
            reason: String::new(),
        };
    }
    LanguageProfileResolution {
        requested: selected_request.to_string(),
        selected: STANDARD_ENGLISH_LANGUAGE.to_string(),
        fallback: true,
        reason: "unsupported_or_unavailable_language_profile".to_string(),
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LanguageResourceState {
    pub locale: String,
    pub schema_version: String,
    pub content_hash: String,
    pub signature: String,
    pub epoch: String,
    pub expected_epoch: String,
}

pub fn validate_language_resource_state(
    state: &LanguageResourceState,
) -> Option<ReadinessDiagnostic> {
    if state.locale.is_empty()
        || state.schema_version.is_empty()
        || state.content_hash.is_empty()
        || state.signature.is_empty()
    {
        return Some(ReadinessDiagnostic {
            code: "SB_DRIVER_LANGUAGE_RESOURCE_INCOMPLETE",
            sqlstate: "0A000",
            message: "language resource refused: locale, schema version, content hash, and signature are required"
                .to_string(),
        });
    }
    if !state.expected_epoch.is_empty() && state.epoch != state.expected_epoch {
        return Some(ReadinessDiagnostic {
            code: "SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE",
            sqlstate: "0A000",
            message: "language resource refused: language epoch does not match current context"
                .to_string(),
        });
    }
    None
}
