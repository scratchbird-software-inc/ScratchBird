# Management surface reference

This reference defines the fields a management UI may render and the authority
it must preserve. Fields not returned by the engine are unknown, not false.

## Performance and authority identity

The optimization surface reports `optimization_profile` together with
`catalog_generation_id`, `name_resolution_epoch`, `security_epoch`,
`resource_epoch`, and `statistics_epoch`. A client must treat that tuple as one
snapshot. It must never combine rows from different tuples or use an older
name-only cache as catalog authority.

## MGA cleanup and physical backlog

`cleanup_horizon_authority_status` qualifies these transaction horizons:

- `oldest_interesting_transaction_id`
- `oldest_active_transaction_id`
- `oldest_snapshot_transaction_id`
- `oldest_cleanup_transaction_id`

The corresponding work indicators are
`storage_row_version_backlog_count`, `index_delta_backlog_count`,
`index_garbage_backlog_count`, and `page_summary_backlog_count`.

## Index state

The engine may publish `secondary_index_state`, `shadow_index_state`,
`summary_index_state`, and `specialized_index_state`. The
`index_state_authority_source` identifies the engine-owned source for those
values. Planning and maintenance evidence may additionally name
`ordered_table_candidate_set`, `secondary_delta_ledger`,
`page_extent_summary`, `time_range_summary`, `shadow_index_build_state`,
`inverted_search_segment_state`, and `vector_generation_state`.

## Resource and refusal state

`resource_governor_state`, `resource_quota_grants`, and `backpressure_active`
describe current admission pressure. A refused request carries
`exact_refusal_diagnostic_code`, `exact_refusal_message_vector`, and
`message_vector_ready`. UIs must retain the complete vector and its order.

## DPC-070 Resource/Soak Evidence Fields

Long-running validation exposes `fd_count`, `rss_kib`, `thread_count`,
`database_tree_bytes`, and `foreground_p95_millis`. Terminal evidence also
records `clean_shutdown` and `read_only_reopen_classification`. These values are
observations, not permission to change quotas or recovery state.

## Metrics, audit, and support bundles

Management evidence identifies its `metric_family` and `audit_event_family`.
Support-bundle status includes `support_bundle_redaction_state`,
`support_bundle_completeness_state`, and
`support_bundle_forbidden_fields_absent`.

The bundle request binds `bundle_scope`, `retention_policy_ref`,
`redaction_profile_ref`, `authority_path`, `audit_envelope_ref`, and
`flush_required_before_export`. Product output never exposes `physical_path` or
an `unsafe_payload`; protected material is represented as `<redacted>`.

## Finality fields

| Field | Type | Required interpretation |
| --- | --- | --- |
| `parser_finality_authority` | bool | Must be `false` |
| `reference_finality_authority` | bool | Must be `false` |
| `client_finality_authority` | bool | Must be `false` |
| `storage_shortcut_finality_authority` | bool | Must be `false` |
| `wal_recovery_authority` | bool | Must be `false` |
| `catalog_uuid_authority` | bool | Must identify engine authority |

## Configuration precedence and feature state

The `config_precedence_order` field records the fixed precedence
`admin_override > cli_option > environment > config_file > packaged_default`.
The management projection reports these booleans without allowing the caller to
invent a new authority source:

- `optimizer_enabled`
- `copy_append_batching_enabled`
- `native_ingest_enabled`
- `plan_cache_enabled`
- `descriptor_metadata_cache_enabled`
- `statistics_enabled`
- `summary_prune_enabled`
- `agent_workers_enabled`
- `resource_governor_enabled`
- `page_filespace_preallocation_enabled`
- `cancellation_enabled`
- `backpressure_enabled`

`copy_batch_rows_configured` is the configured batch limit, not a promise that
every operation will consume a full batch.

## Index Management Operations, Rights, And Message Vectors

Supported route identities include `index.validate`, `index.analyze`,
`index.backlog`, `index.rebuild`, `index.repair`,
`index.cleanup_mga_versions`, and `index.optimization_control`. Read routes
require `OBS_MANAGEMENT_INSPECT`, `OBS_INDEX_PROFILE_READ`, or
`MGA_CLEANUP_INSPECT` as specified by the returned route. Mutating routes require
the applicable `OBS_MANAGEMENT_CONTROL`, `MGA_CLEANUP_CONTROL`, or
`OBS_CONFIG_CONTROL` right. Bundle export requires `SUPPORT_EXPORT`.

The canonical refusal set includes:

- `DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY`
- `OBS_CONFIG_OVERRIDE_REQUIRED`
- `SECURITY.AUTHORIZATION.DENIED`
- `SECURITY.CONTEXT.EXPIRED`
- `SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED`
- `SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED`
- `OPS.SUPPORT_BUNDLE.ENGINE_AUTHORIZATION_REQUIRED`
- `OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN`
- `DPC.OBSERVABILITY.NON_AUTHORITATIVE_INPUT_REFUSED`
- `MGA.BOUNDARY.ENGINE_OWNS_FINALITY`
- `CDP.USER_OBSERVABILITY_SURFACE.INVALID_SNAPSHOT`
- `AGENT.ZERO_GREY.DIAGNOSTIC_REQUIRED`
