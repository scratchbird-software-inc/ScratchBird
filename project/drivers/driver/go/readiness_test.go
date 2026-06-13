// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"database/sql"
	"testing"
)

func TestBetaDriverReadinessStatusMatchesManifestLane(t *testing.T) {
	status := BetaDriverReadinessStatus()
	if status.ComponentID != "driver:go" {
		t.Fatalf("unexpected component id: %s", status.ComponentID)
	}
	if status.DriverPackageUUID != "019e12a0-0005-7000-8000-000000000005" {
		t.Fatalf("unexpected package uuid: %s", status.DriverPackageUUID)
	}
	if status.DriverStatus != "beta_2" || status.ReleaseBucket != "release_candidate" {
		t.Fatalf("unexpected manifest status: %#v", status)
	}
	if !status.AuthorityBoundary.ServerRevalidationRequired {
		t.Fatalf("server revalidation must remain required")
	}
	if !status.AuthorityBoundary.LocalSBLRIsAdvisory || !status.AuthorityBoundary.LocalUUIDCacheIsAdvisory {
		t.Fatalf("local SBLR/UUID cache must be advisory")
	}
	if status.AuthorityBoundary.TransactionFinalityOwner != "engine_mga_transaction_inventory" {
		t.Fatalf("unexpected finality owner: %s", status.AuthorityBoundary.TransactionFinalityOwner)
	}
}

func TestAdvisoryCacheContextRefusesStaleEpochsAndTransactionReuse(t *testing.T) {
	current := AdvisoryCacheContext{
		DatabaseUUID:    "db-1",
		SchemaEpoch:     "schema-2",
		PolicyEpoch:     "policy-2",
		LanguageEpoch:   "lang-2",
		CapabilityEpoch: "cap-2",
		PrincipalUUID:   "principal-1",
		RoleSetHash:     "role-a",
		GroupSetHash:    "group-a",
		TransactionUUID: "txn-2",
	}
	cached := current
	cached.PolicyEpoch = "policy-1"
	diag := ValidateAdvisoryCacheContext(cached, current)
	if diag == nil || diag.Code != "SB_DRIVER_CACHE_POLICY_EPOCH_STALE" {
		t.Fatalf("expected stale policy diagnostic, got %#v", diag)
	}

	cached = current
	cached.TransactionUUID = "txn-1"
	diag = ValidateAdvisoryCacheContext(cached, current)
	if diag == nil || diag.Code != "SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH" || diag.SQLState != "25001" {
		t.Fatalf("expected transaction boundary diagnostic, got %#v", diag)
	}

	if diag = ValidateAdvisoryCacheContext(current, current); diag != nil {
		t.Fatalf("matching advisory context should pass, got %#v", diag)
	}
}

func TestPreparedBundleReuseRequiresServerAdmissionAndMatchingContext(t *testing.T) {
	current := AdvisoryCacheContext{
		DatabaseUUID:    "db-1",
		SchemaEpoch:     "schema-1",
		PolicyEpoch:     "policy-1",
		PrincipalUUID:   "principal-1",
		TransactionUUID: "txn-1",
	}
	bundle := PreparedBundleContext{
		DatabaseUUID:    "db-1",
		SchemaEpoch:     "schema-1",
		PolicyEpoch:     "policy-1",
		PrincipalUUID:   "principal-1",
		TransactionUUID: "txn-1",
		ServerAdmitted:  false,
	}
	diag := ValidatePreparedBundleReuse(bundle, current)
	if diag == nil || diag.Code != "SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED" {
		t.Fatalf("expected server admission diagnostic, got %#v", diag)
	}

	bundle.ServerAdmitted = true
	bundle.DatabaseUUID = "db-2"
	diag = ValidatePreparedBundleReuse(bundle, current)
	if diag == nil || diag.Code != "SB_DRIVER_CACHE_DATABASE_MISMATCH" {
		t.Fatalf("expected cross-database refusal, got %#v", diag)
	}
}

func TestLanguageResourceFallbackAndIntegrityRefusal(t *testing.T) {
	resolved := ResolveLanguageProfile("fr_CA", map[string]bool{"en_US": true})
	if resolved.Selected != "en_US" || !resolved.Fallback {
		t.Fatalf("expected standard English fallback, got %#v", resolved)
	}

	resolved = ResolveLanguageProfile("fr_CA", map[string]bool{"en_US": true, "fr_CA": true})
	if resolved.Selected != "fr_CA" || resolved.Fallback {
		t.Fatalf("expected preferred language selection, got %#v", resolved)
	}

	diag := ValidateLanguageResourceState(LanguageResourceState{
		Locale:        "fr_CA",
		SchemaVersion: "1",
		ContentHash:   "sha256:abc",
		Signature:     "sig",
		Epoch:         "lang-1",
		ExpectedEpoch: "lang-2",
	})
	if diag == nil || diag.Code != "SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE" {
		t.Fatalf("expected stale language resource diagnostic, got %#v", diag)
	}
}

func TestPackageSmokeRegistersDatabaseSQLDriver(t *testing.T) {
	for _, name := range sql.Drivers() {
		if name == "scratchbird" {
			return
		}
	}
	t.Fatalf("scratchbird database/sql driver was not registered")
}
