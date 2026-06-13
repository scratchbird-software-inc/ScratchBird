// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use scratchbird::{
    beta_driver_readiness_status, resolve_language_profile, validate_advisory_cache_context,
    validate_language_resource_state, validate_prepared_bundle_reuse, AdvisoryCacheContext,
    LanguageResourceState, PreparedBundleContext,
};

fn current_context() -> AdvisoryCacheContext {
    AdvisoryCacheContext {
        database_uuid: "db-1".to_string(),
        schema_epoch: "schema-2".to_string(),
        policy_epoch: "policy-2".to_string(),
        language_epoch: "lang-2".to_string(),
        capability_epoch: "cap-2".to_string(),
        principal_uuid: "principal-1".to_string(),
        role_set_hash: "role-a".to_string(),
        group_set_hash: "group-a".to_string(),
        transaction_uuid: "txn-2".to_string(),
    }
}

#[test]
fn beta_driver_readiness_status_matches_manifest_lane() {
    let status = beta_driver_readiness_status();
    assert_eq!(status.component_id, "driver:rust");
    assert_eq!(
        status.driver_package_uuid,
        "019e12a0-0015-7000-8000-000000000015"
    );
    assert_eq!(status.driver_status, "beta_2");
    assert_eq!(status.release_bucket, "release_candidate");
    assert_eq!(status.conformance_profile_ref, "driver_rust_gate");
    assert!(status.authority_boundary.server_revalidation_required);
    assert!(status.authority_boundary.local_sblr_is_advisory);
    assert!(status.authority_boundary.local_uuid_cache_is_advisory);
    assert_eq!(
        status.authority_boundary.transaction_finality_owner,
        "engine_mga_transaction_inventory"
    );
    assert!(status.runtime_mapping.dsn_keys.contains(&"auth_method"));
}

#[test]
fn advisory_cache_context_refuses_stale_epochs_and_transaction_reuse() {
    let current = current_context();
    let mut cached = current.clone();
    cached.policy_epoch = "policy-1".to_string();
    let diag = validate_advisory_cache_context(&cached, &current).expect("stale policy refusal");
    assert_eq!(diag.code, "SB_DRIVER_CACHE_POLICY_EPOCH_STALE");
    assert_eq!(diag.sqlstate, "42501");

    let mut cached = current.clone();
    cached.transaction_uuid = "txn-1".to_string();
    let diag =
        validate_advisory_cache_context(&cached, &current).expect("transaction boundary refusal");
    assert_eq!(diag.code, "SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH");
    assert_eq!(diag.sqlstate, "25001");
    assert!(validate_advisory_cache_context(&current, &current).is_none());
}

#[test]
fn prepared_bundle_reuse_requires_server_admission_and_matching_context() {
    let current = AdvisoryCacheContext {
        database_uuid: "db-1".to_string(),
        schema_epoch: "schema-1".to_string(),
        policy_epoch: "policy-1".to_string(),
        principal_uuid: "principal-1".to_string(),
        transaction_uuid: "txn-1".to_string(),
        ..AdvisoryCacheContext::default()
    };
    let mut bundle = PreparedBundleContext {
        database_uuid: "db-1".to_string(),
        schema_epoch: "schema-1".to_string(),
        policy_epoch: "policy-1".to_string(),
        principal_uuid: "principal-1".to_string(),
        transaction_uuid: "txn-1".to_string(),
        server_admitted: false,
    };
    let diag = validate_prepared_bundle_reuse(&bundle, &current).expect("admission refusal");
    assert_eq!(diag.code, "SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED");

    bundle.server_admitted = true;
    bundle.database_uuid = "db-2".to_string();
    let diag = validate_prepared_bundle_reuse(&bundle, &current).expect("database refusal");
    assert_eq!(diag.code, "SB_DRIVER_CACHE_DATABASE_MISMATCH");
}

#[test]
fn language_resource_fallback_and_integrity_refusal() {
    let resolved = resolve_language_profile("fr_CA", &["en_US"]);
    assert_eq!(resolved.selected, "en_US");
    assert!(resolved.fallback);

    let resolved = resolve_language_profile("fr_CA", &["en_US", "fr_CA"]);
    assert_eq!(resolved.selected, "fr_CA");
    assert!(!resolved.fallback);

    let diag = validate_language_resource_state(&LanguageResourceState {
        locale: "fr_CA".to_string(),
        schema_version: "1".to_string(),
        content_hash: "sha256:abc".to_string(),
        signature: "sig".to_string(),
        epoch: "lang-1".to_string(),
        expected_epoch: "lang-2".to_string(),
    })
    .expect("stale language refusal");
    assert_eq!(diag.code, "SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE");
}

#[test]
fn package_smoke_reports_mpl_license() {
    assert_eq!(env!("CARGO_PKG_NAME"), "scratchbird");
    assert_eq!(env!("CARGO_PKG_LICENSE"), "MPL-2.0");
}
