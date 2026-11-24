# The explicit allow lists of native_family_guard_policy_test.py (one line of reason each). Keys are project/src-relative paths.
# A file or function is listed here only when it is NOT a legacy-side-file entry a native context can reach unguarded.
FILE_ALLOW = {
    # ---- builders whose every caller holds the guard ------------------------------------------------------------------------------
    'engine/internal_api/crud_support/crud_store_00_common_types.inc': 'builder CrudEventPath only; its two callers (AppendCrudEvent, LoadCrudState) hold the guard in crud_store_05 / _04',
    'engine/internal_api/mga_relation_store/mga_relation_locator.hpp': 'inline path builders for the scoped relation store; every caller is a guarded mga_relation_store entry',
    'engine/internal_api/mga_relation_store/mga_update_durable_frame_store.cpp': 'path builders only; the callers are the guarded entries of mga_update_durable_store.cpp',
    'engine/internal_api/transaction/local_publication_store.hpp': 'inline publication-store paths; the caller RunLocalCommitPageBarrier (local_commit_publication.cpp) holds the guard',
    # ---- native-aware: the family answers a native context without a side file ------------------------------------------------------
    'engine/internal_api/sblr_executor_availability_registry.cpp': 'native-aware (S3e): every load answers a native context from the single-file build table (NativeLoad), Set refuses; no side file is read',
    'engine/internal_api/observability/metrics_api.cpp': 'MetricHistoryPath returns the empty path for a native context (in-process history only); no side file is read or created',
    'engine/native_compile/native_compile.cpp': 'AOT artifact directory; its only caller (llvm_api.cpp BuildNativeCompileRequest) passes an EMPTY path for a native context, which ArtifactDirectory already treats as no location',
    # ---- dead code ---------------------------------------------------------------------------------------------------------------
    'engine/internal_api/security/security_principal_lifecycle.cpp': 'EventPath(".sb.security_principal_events") has no caller in the translation unit (dead builder; the store moved to the database-local security event store); goes with the legacy cut',
    # ---- NOT engine families: reported to the owner, not guarded here --------------------------------------------------------------
    'server/database_ownership.cpp': 'server control file (database ownership lock files .sb.owner.lock / .sb.route.owner.lock); separate owner item Q257',
    'storage/disk/disk_device.cpp': 'the storage layer\'s ownership lock paths (.sb.owner.lock / .sb.route.owner.lock); borrowed owner lease for a served native database (0 .sb. syscalls in the native strace tests); owner item Q257',
    'storage/database/local_transaction_store.cpp': 'legacy local transaction publish journal (.sb.txn_publish); the native database uses the native transaction store (0 .sb. syscalls in the native strace tests); goes with the legacy cut',
    'storage/database/database_lifecycle.cpp': 'database lifecycle (create/drop/migrate/quarantine) of LEGACY sidecars; not an engine request path for a native context; goes with the legacy cut',
    'manager/node/manager_runtime.cpp': 'manager process own temporary-auth-token file (.sb.temporary_auth_tokens); the manager is a separate process and takes no EngineRequestContext; owner item',
}
FN_ALLOW = {
    # ---- the guard is the first thing the function calls (shared validator) ---------------------------------------------------------
    'engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp::LookupSblrDdlCreateSchemaExecutionJournalV1': 'guard is in ValidateInputs, the first call (refuses before any read, lock or create)',
    'engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp::EnsureSblrDdlCreateSchemaExecutionJournalV1': 'guard is in ValidateInputs, the first call',
    'engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp::ExecuteSblrDdlCreateSchemaExecutionJournalV1': 'guard is in ValidateInputs, the first call',
    # ---- native-aware: branches to the single-file source before any side file -----------------------------------------------------
    'engine/internal_api/catalog/name_registry.cpp::LoadNameRegistryState': 'native branch to the names source (the single file) is the first statement after the context check',
    'engine/internal_api/catalog/name_registry.cpp::LoadNameRegistryStateSnapshot': 'native-aware: goes through LoadNameRegistryState',
    'engine/internal_api/catalog/name_registry.cpp::ResolveNameRegistryPrivate': 'native branch to the exact native name resolve (Part B S2)',
    'engine/internal_api/catalog/name_registry.cpp::ResolveNameRegistryPublic': 'native-aware: goes through ResolveNameRegistryPrivate',
    'engine/internal_api/catalog/name_registry.cpp::ResolveNameRegistry': 'native-aware: goes through the private/public resolve',
    'engine/internal_api/catalog/name_registry.cpp::MapNameRegistryUuidToNamePrivate': 'native branch to UuidToName (Part B)',
    'engine/internal_api/catalog/name_registry.cpp::MapNameRegistryUuidToNamePublic': 'native-aware: goes through the private map',
    'engine/internal_api/catalog/name_registry.cpp::MapNameRegistryUuidToName': 'native-aware: goes through the private/public map',
    'engine/internal_api/catalog/name_registry.cpp::NameRegistryWouldConflict': 'native-aware: Load answers from the single file; the native branch compares bytes',
    'engine/internal_api/sblr_prepared_statement_registry.cpp::LoadSblrPreparedStatementRegistryV1': 'native-aware (S3e): LoadExact answers the empty process-local registry for a native context',
    'engine/internal_api/sblr_prepared_statement_registry.cpp::ResolveActiveSblrPreparedStatementCapabilityV1': 'native-aware: through LoadExact',
    'engine/internal_api/sblr_prepared_statement_registry.cpp::PublishSblrPreparedStatementV1': 'guarded (S3e) before the first write; authority check precedes it and touches no file',
    'engine/internal_api/sblr_prepared_statement_registry.cpp::ResolveSblrPreparedStatementExecutionV1': 'native-aware: through LoadExact',
    'engine/internal_api/sblr_prepared_statement_registry.cpp::PublishSblrPreparedStatementExecutionV1': 'guarded (S3e) before the first write',
    'engine/internal_api/sblr_prepared_statement_registry.cpp::FreeSblrPreparedStatementV1': 'guarded (S3e) before the first write',
    'engine/internal_api/sblr_prepared_statement_registry.cpp::RevokeSblrPreparedStatementSessionV1': 'native-aware: LoadExact returns not-found for a native context, so the write is unreachable',
    'engine/internal_api/sblr_diagnostic_identity_registry.cpp::LookupSblrDiagnosticIdentityV1': 'works on a frozen snapshot that LoadSblrDiagnosticIdentitySnapshotV1 (guarded) produced; no file',
    'engine/internal_api/sblr_transaction_begin_authority.cpp::LoadSblrTransactionBeginAuthorityV1': 'guard follows the result declaration, before any path or file',
    'engine/internal_api/catalog/catalog_object_lifecycle.cpp::LoadCatalogObjectLifecycleState': 'guard follows the result declaration (S3e), before any path or file',
    'engine/internal_api/catalog/catalog_journal_publication.hpp::AppendSequencedCatalogJournalRecord': 'guard follows a message lambda (S3e), before any path or file',
    'engine/internal_api/catalog/catalog_journal_batch.hpp::ValidateCatalogResolverJournalFormat': 'only called from catalog_object_lifecycle.cpp entries, which are all guarded first',
    # ---- members of a store that can only be opened through a guarded entry ------------------------------------------------------
    'engine/internal_api/mga_relation_store/mga_delete_durable_store.cpp::MgaDmlDeleteDurableStoreV1::Append': 'member of a store that exists only if Open() succeeded; Open refuses a native context',
    'engine/internal_api/mga_relation_store/mga_delete_durable_store.cpp::MgaDmlDeleteDurableStoreV1::CompleteReleasedPublication': 'member of an opened store (Open refuses a native context)',
    'engine/internal_api/mga_relation_store/mga_delete_durable_store.cpp::MgaDmlDeleteDurableStoreV1::AbortBeforePublication': 'member of an opened store (Open refuses a native context)',
    # ---- path builders that answer the empty path for a native context ------------------------------------------------------------
    'engine/internal_api/nosql/document_path_physical_provider.cpp::DocumentPathPhysicalProviderPath': 'path builder; returns the empty path for a native context (the existing no-database-path answer)',
}
STEM_OVERRIDE = {
    'engine/internal_api/catalog/name_registry.cpp': ['name_registry_names_source'],
    'engine/internal_api/management/memory_management_api.cpp': ['sys_memory_catalogs'],
    'engine/internal_api/sblr_diagnostic_identity_registry.cpp': ['diagnostic_identity_registry'],
    'engine/internal_api/sblr_prepared_statement_registry.cpp': ['prepared_statement_registry'],
    'engine/internal_api/sblr_transaction_begin_authority.cpp': ['transaction_begin_authority'],
}
