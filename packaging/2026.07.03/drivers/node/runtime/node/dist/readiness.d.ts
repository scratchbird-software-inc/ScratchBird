export declare const DRIVER_READINESS_SCHEMA_VERSION = "scratchbird.driver.readiness.v1";
export declare const DRIVER_COMPONENT_ID = "driver:node";
export declare const DRIVER_PACKAGE_UUID = "019e12a0-0008-7000-8000-000000000008";
export declare const DRIVER_STATUS = "beta_2";
export declare const DRIVER_RELEASE_BUCKET = "release_candidate";
export declare const DRIVER_CONFORMANCE_PROFILE = "driver_node_gate";
export declare const DRIVER_SOURCE_PATH = "project/drivers/driver/node";
export declare const DRIVER_LICENSE = "MPL-2.0";
export declare const STANDARD_ENGLISH_LANGUAGE = "en_US";
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
export declare function betaDriverReadinessStatus(): {
    schema_version: string;
    component_id: string;
    driver_package_uuid: string;
    driver_status: string;
    release_bucket: string;
    conformance_profile_ref: string;
    source_path: string;
    package_name: string;
    import_path: string;
    license: string;
    runtime_mapping: {
        api_surface: string;
        ingress_modes: string[];
        wire_protocols: string[];
        dsn_keys: string[];
        auth_methods: string[];
        tls_profile: string;
        type_mapping_profile: string;
        diagnostic_mapping_profile: string;
        metadata_profile: string;
        thread_safety_class: string;
        pooling_capability: string;
    };
    authority_boundary: {
        local_sblr_is_advisory: boolean;
        local_uuid_cache_is_advisory: boolean;
        local_result_cache_is_advisory: boolean;
        server_revalidation_required: boolean;
        transaction_finality_owner: string;
        language_fallback_profile: string;
        cache_invalidation_requirement: string;
    };
};
export declare function validateAdvisoryCacheContext(cached: AdvisoryCacheContext, current: AdvisoryCacheContext): ReadinessDiagnostic | undefined;
export declare function validatePreparedBundleReuse(bundle: PreparedBundleContext, current: AdvisoryCacheContext): ReadinessDiagnostic | undefined;
export declare function resolveLanguageProfile(requested: string, available?: Record<string, boolean>): LanguageProfileResolution;
export declare function validateLanguageResourceState(state: LanguageResourceState): ReadinessDiagnostic | undefined;
