// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import "fmt"

const (
	DriverReadinessSchemaVersion = "scratchbird.driver.readiness.v1"
	DriverComponentID            = "driver:go"
	DriverPackageUUID            = "019e12a0-0005-7000-8000-000000000005"
	DriverStatus                 = "beta_2"
	DriverReleaseBucket          = "release_candidate"
	DriverConformanceProfile     = "driver_go_gate"
	DriverSourcePath             = "project/drivers/driver/go"
	DriverLicense                = "MPL-2.0"
	StandardEnglishLanguage      = "en_US"
)

type ReadinessDiagnostic struct {
	Code     string `json:"code"`
	SQLState string `json:"sqlstate"`
	Message  string `json:"message"`
}

func (d ReadinessDiagnostic) Error() string {
	if d.SQLState == "" {
		return d.Message
	}
	return fmt.Sprintf("[%s] %s", d.SQLState, d.Message)
}

type DriverRuntimeMapping struct {
	APISurface         string   `json:"api_surface"`
	IngressModes       []string `json:"ingress_modes"`
	WireProtocols      []string `json:"wire_protocols"`
	DSNKeys            []string `json:"dsn_keys"`
	AuthMethods        []string `json:"auth_methods"`
	TLSProfile         string   `json:"tls_profile"`
	TypeMappingProfile string   `json:"type_mapping_profile"`
	DiagnosticProfile  string   `json:"diagnostic_mapping_profile"`
	MetadataProfile    string   `json:"metadata_profile"`
	ThreadSafetyClass  string   `json:"thread_safety_class"`
	PoolingCapability  string   `json:"pooling_capability"`
}

type DriverAuthorityBoundary struct {
	LocalSBLRIsAdvisory          bool   `json:"local_sblr_is_advisory"`
	LocalUUIDCacheIsAdvisory     bool   `json:"local_uuid_cache_is_advisory"`
	LocalResultCacheIsAdvisory   bool   `json:"local_result_cache_is_advisory"`
	ServerRevalidationRequired   bool   `json:"server_revalidation_required"`
	TransactionFinalityOwner     string `json:"transaction_finality_owner"`
	LanguageFallbackProfile      string `json:"language_fallback_profile"`
	CacheInvalidationRequirement string `json:"cache_invalidation_requirement"`
}

type BetaReadinessStatus struct {
	SchemaVersion         string                  `json:"schema_version"`
	ComponentID           string                  `json:"component_id"`
	DriverPackageUUID     string                  `json:"driver_package_uuid"`
	DriverStatus          string                  `json:"driver_status"`
	ReleaseBucket         string                  `json:"release_bucket"`
	ConformanceProfileRef string                  `json:"conformance_profile_ref"`
	SourcePath            string                  `json:"source_path"`
	PackageName           string                  `json:"package_name"`
	ImportPath            string                  `json:"import_path"`
	License               string                  `json:"license"`
	RuntimeMapping        DriverRuntimeMapping    `json:"runtime_mapping"`
	AuthorityBoundary     DriverAuthorityBoundary `json:"authority_boundary"`
}

func BetaDriverReadinessStatus() BetaReadinessStatus {
	return BetaReadinessStatus{
		SchemaVersion:         DriverReadinessSchemaVersion,
		ComponentID:           DriverComponentID,
		DriverPackageUUID:     DriverPackageUUID,
		DriverStatus:          DriverStatus,
		ReleaseBucket:         DriverReleaseBucket,
		ConformanceProfileRef: DriverConformanceProfile,
		SourcePath:            DriverSourcePath,
		PackageName:           "scratchbird",
		ImportPath:            "github.com/scratchbird/scratchbird-go",
		License:               DriverLicense,
		RuntimeMapping: DriverRuntimeMapping{
			APISurface:         "database_sql",
			IngressModes:       []string{"direct_listener", "manager_proxy"},
			WireProtocols:      []string{"sbwp_v1_1"},
			DSNKeys:            []string{"database", "host", "port", "user", "auth_method"},
			AuthMethods:        []string{"engine_local_password", "scram_ready"},
			TLSProfile:         "scratchbird_tls_1_3_floor",
			TypeMappingProfile: "sbsql_core",
			DiagnosticProfile:  "native_sqlstate",
			MetadataProfile:    "sys_information_recursive",
			ThreadSafetyClass:  "thread_safe",
			PoolingCapability:  "connection_pool",
		},
		AuthorityBoundary: DriverAuthorityBoundary{
			LocalSBLRIsAdvisory:          true,
			LocalUUIDCacheIsAdvisory:     true,
			LocalResultCacheIsAdvisory:   true,
			ServerRevalidationRequired:   true,
			TransactionFinalityOwner:     "engine_mga_transaction_inventory",
			LanguageFallbackProfile:      StandardEnglishLanguage,
			CacheInvalidationRequirement: "policy_schema_language_capability_transaction_epoch",
		},
	}
}

type AdvisoryCacheContext struct {
	DatabaseUUID    string
	SchemaEpoch     string
	PolicyEpoch     string
	LanguageEpoch   string
	CapabilityEpoch string
	PrincipalUUID   string
	RoleSetHash     string
	GroupSetHash    string
	TransactionUUID string
}

func ValidateAdvisoryCacheContext(cached, current AdvisoryCacheContext) *ReadinessDiagnostic {
	checks := []struct {
		name string
		got  string
		want string
		code string
	}{
		{"database_uuid", cached.DatabaseUUID, current.DatabaseUUID, "SB_DRIVER_CACHE_DATABASE_MISMATCH"},
		{"schema_epoch", cached.SchemaEpoch, current.SchemaEpoch, "SB_DRIVER_CACHE_SCHEMA_EPOCH_STALE"},
		{"policy_epoch", cached.PolicyEpoch, current.PolicyEpoch, "SB_DRIVER_CACHE_POLICY_EPOCH_STALE"},
		{"language_epoch", cached.LanguageEpoch, current.LanguageEpoch, "SB_DRIVER_CACHE_LANGUAGE_EPOCH_STALE"},
		{"capability_epoch", cached.CapabilityEpoch, current.CapabilityEpoch, "SB_DRIVER_CACHE_CAPABILITY_EPOCH_STALE"},
		{"principal_uuid", cached.PrincipalUUID, current.PrincipalUUID, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"},
		{"role_set_hash", cached.RoleSetHash, current.RoleSetHash, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"},
		{"group_set_hash", cached.GroupSetHash, current.GroupSetHash, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"},
	}
	for _, check := range checks {
		if check.got != check.want {
			return &ReadinessDiagnostic{
				Code:     check.code,
				SQLState: "42501",
				Message:  fmt.Sprintf("advisory cache entry refused: %s does not match current context", check.name),
			}
		}
	}
	if cached.TransactionUUID != "" || current.TransactionUUID != "" {
		if cached.TransactionUUID != current.TransactionUUID {
			return &ReadinessDiagnostic{
				Code:     "SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH",
				SQLState: "25001",
				Message:  "advisory cache entry refused: transaction context does not match current MGA boundary",
			}
		}
	}
	return nil
}

type PreparedBundleContext struct {
	DatabaseUUID    string
	SchemaEpoch     string
	PolicyEpoch     string
	PrincipalUUID   string
	TransactionUUID string
	ServerAdmitted  bool
}

func ValidatePreparedBundleReuse(bundle PreparedBundleContext, current AdvisoryCacheContext) *ReadinessDiagnostic {
	if !bundle.ServerAdmitted {
		return &ReadinessDiagnostic{
			Code:     "SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED",
			SQLState: "0A000",
			Message:  "driver-prepared SBLR/UUID bundle is advisory until server admission succeeds",
		}
	}
	return ValidateAdvisoryCacheContext(AdvisoryCacheContext{
		DatabaseUUID:    bundle.DatabaseUUID,
		SchemaEpoch:     bundle.SchemaEpoch,
		PolicyEpoch:     bundle.PolicyEpoch,
		PrincipalUUID:   bundle.PrincipalUUID,
		TransactionUUID: bundle.TransactionUUID,
	}, AdvisoryCacheContext{
		DatabaseUUID:    current.DatabaseUUID,
		SchemaEpoch:     current.SchemaEpoch,
		PolicyEpoch:     current.PolicyEpoch,
		PrincipalUUID:   current.PrincipalUUID,
		TransactionUUID: current.TransactionUUID,
	})
}

type LanguageProfileResolution struct {
	Requested string `json:"requested"`
	Selected  string `json:"selected"`
	Fallback  bool   `json:"fallback"`
	Reason    string `json:"reason"`
}

func ResolveLanguageProfile(requested string, available map[string]bool) LanguageProfileResolution {
	if requested == "" {
		requested = StandardEnglishLanguage
	}
	if available != nil && available[requested] {
		return LanguageProfileResolution{Requested: requested, Selected: requested}
	}
	return LanguageProfileResolution{
		Requested: requested,
		Selected:  StandardEnglishLanguage,
		Fallback:  true,
		Reason:    "unsupported_or_unavailable_language_profile",
	}
}

type LanguageResourceState struct {
	Locale        string
	SchemaVersion string
	ContentHash   string
	Signature     string
	Epoch         string
	ExpectedEpoch string
}

func ValidateLanguageResourceState(state LanguageResourceState) *ReadinessDiagnostic {
	if state.Locale == "" || state.SchemaVersion == "" || state.ContentHash == "" || state.Signature == "" {
		return &ReadinessDiagnostic{
			Code:     "SB_DRIVER_LANGUAGE_RESOURCE_INCOMPLETE",
			SQLState: "0A000",
			Message:  "language resource refused: locale, schema version, content hash, and signature are required",
		}
	}
	if state.ExpectedEpoch != "" && state.Epoch != state.ExpectedEpoch {
		return &ReadinessDiagnostic{
			Code:     "SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE",
			SQLState: "0A000",
			Message:  "language resource refused: language epoch does not match current context",
		}
	}
	return nil
}
