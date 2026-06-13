# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import pathlib

import scratchbird
from scratchbird import (
    AdvisoryCacheContext,
    LanguageResourceState,
    PreparedBundleContext,
    resolve_language_profile,
    validate_advisory_cache_context,
    validate_language_resource_state,
    validate_prepared_bundle_reuse,
)


def test_beta_driver_readiness_status_matches_manifest_lane():
    status = scratchbird.beta_driver_readiness_status()
    assert status["component_id"] == "driver:python"
    assert status["driver_package_uuid"] == "019e12a0-0012-7000-8000-000000000012"
    assert status["driver_status"] == "beta_2"
    assert status["release_bucket"] == "release_candidate"
    assert status["conformance_profile_ref"] == "driver_python_gate"
    assert status["authority_boundary"]["server_revalidation_required"] is True
    assert status["authority_boundary"]["local_sblr_is_advisory"] is True
    assert status["authority_boundary"]["local_uuid_cache_is_advisory"] is True
    assert status["authority_boundary"]["transaction_finality_owner"] == "engine_mga_transaction_inventory"
    assert "auth_method" in status["runtime_mapping"]["dsn_keys"]


def test_advisory_cache_context_refuses_stale_epochs_and_transaction_reuse():
    current = AdvisoryCacheContext(
        database_uuid="db-1",
        schema_epoch="schema-2",
        policy_epoch="policy-2",
        language_epoch="lang-2",
        capability_epoch="cap-2",
        principal_uuid="principal-1",
        role_set_hash="role-a",
        group_set_hash="group-a",
        transaction_uuid="txn-2",
    )
    cached = AdvisoryCacheContext(
        database_uuid="db-1",
        schema_epoch="schema-2",
        policy_epoch="policy-1",
        language_epoch="lang-2",
        capability_epoch="cap-2",
        principal_uuid="principal-1",
        role_set_hash="role-a",
        group_set_hash="group-a",
        transaction_uuid="txn-2",
    )
    diag = validate_advisory_cache_context(cached, current)
    assert diag is not None
    assert diag.code == "SB_DRIVER_CACHE_POLICY_EPOCH_STALE"
    assert diag.sqlstate == "42501"

    cached = AdvisoryCacheContext(
        database_uuid="db-1",
        schema_epoch="schema-2",
        policy_epoch="policy-2",
        language_epoch="lang-2",
        capability_epoch="cap-2",
        principal_uuid="principal-1",
        role_set_hash="role-a",
        group_set_hash="group-a",
        transaction_uuid="txn-1",
    )
    diag = validate_advisory_cache_context(cached, current)
    assert diag is not None
    assert diag.code == "SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH"
    assert diag.sqlstate == "25001"
    assert validate_advisory_cache_context(current, current) is None


def test_prepared_bundle_reuse_requires_server_admission_and_matching_context():
    current = AdvisoryCacheContext(
        database_uuid="db-1",
        schema_epoch="schema-1",
        policy_epoch="policy-1",
        principal_uuid="principal-1",
        transaction_uuid="txn-1",
    )
    bundle = PreparedBundleContext(
        database_uuid="db-1",
        schema_epoch="schema-1",
        policy_epoch="policy-1",
        principal_uuid="principal-1",
        transaction_uuid="txn-1",
        server_admitted=False,
    )
    diag = validate_prepared_bundle_reuse(bundle, current)
    assert diag is not None
    assert diag.code == "SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED"

    bundle = PreparedBundleContext(
        database_uuid="db-2",
        schema_epoch="schema-1",
        policy_epoch="policy-1",
        principal_uuid="principal-1",
        transaction_uuid="txn-1",
        server_admitted=True,
    )
    diag = validate_prepared_bundle_reuse(bundle, current)
    assert diag is not None
    assert diag.code == "SB_DRIVER_CACHE_DATABASE_MISMATCH"


def test_language_resource_fallback_and_integrity_refusal():
    resolved = resolve_language_profile("fr_CA", {"en_US": True})
    assert resolved.selected == "en_US"
    assert resolved.fallback is True

    resolved = resolve_language_profile("fr_CA", {"en_US": True, "fr_CA": True})
    assert resolved.selected == "fr_CA"
    assert resolved.fallback is False

    diag = validate_language_resource_state(
        LanguageResourceState(
            locale="fr_CA",
            schema_version="1",
            content_hash="sha256:abc",
            signature="sig",
            epoch="lang-1",
            expected_epoch="lang-2",
        )
    )
    assert diag is not None
    assert diag.code == "SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE"


def test_package_import_smoke_reports_mpl_license():
    assert scratchbird.apilevel == "2.0"
    assert scratchbird.threadsafety == 2
    pyproject = pathlib.Path(__file__).resolve().parents[1] / "pyproject.toml"
    assert 'license = {text = "MPL-2.0"}' in pyproject.read_text(encoding="utf-8")
