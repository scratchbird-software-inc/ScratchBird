# ScratchBird Default Policy Pack

This document describes the default policy pack shipped with the beta release resources. It is operational documentation, not a private specification.

The pack is create-time input only. `CREATE DATABASE` validates the pack manifest, opens every manifest entry, verifies each SHA-256 hash, loads the policy defaults, and materializes the durable catalog rows inside the create transaction. After creation, the durable catalog is the authority; the filesystem pack is not re-read as policy authority.

- Policy pack: `default-local-password`
- Policy generation: `1`
- Default policy count: `58`
- Content SHA-256: `7773d76bb23126e65711f80e23d3e6a7d25b7956f008199b159df3d1d4d2d2c8`
- Source catalog: `policies/default_policy_catalog.json`
- Defaults resource: `policies/policy_defaults.json`

## Policies

### 001. `policy.catalog.bootstrap`

- Default profile: `strict_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `generation_start` default `required_by_default`: Requires generation start for the policy.catalog.bootstrap policy family using profile strict_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: policy-pack loader; policy catalog cache; policy mutation gate.
  - `missing_policy` default `required_by_default`: Requires missing policy for the policy.catalog.bootstrap policy family using profile strict_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: policy-pack loader; policy catalog cache; policy mutation gate.
  - `unknown_policy` default `required_by_default`: Requires unknown policy for the policy.catalog.bootstrap policy family using profile strict_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: policy-pack loader; policy catalog cache; policy mutation gate.
  - `mutation_requires_audit` default `required_by_default`: Requires mutation requires audit for the policy.catalog.bootstrap policy family using profile strict_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: policy-pack loader; policy catalog cache; policy mutation gate.
  - `policy_rows_uuidv7` default `required_by_default`: Requires policy rows uuidv7 for the policy.catalog.bootstrap policy family using profile strict_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: policy-pack loader; policy catalog cache; policy mutation gate.

### 002. `database.identity`

- Default profile: `uuidv7_local_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `database_uuid` default `required_by_default`: Requires database uuid for the database.identity policy family using profile uuidv7_local_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `row_uuid` default `required_by_default`: Requires row uuid for the database.identity policy family using profile uuidv7_local_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `name_uuid_registry` default `required_by_default`: Requires name uuid registry for the database.identity policy family using profile uuidv7_local_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `uuid_order_not_finality` default `required_by_default`: Requires uuid order not finality for the database.identity policy family using profile uuidv7_local_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.

### 003. `database.create.failure_cleanup`

- Default profile: `remove_or_quarantine_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `on_tx1_failure` default `required_by_default`: Requires on tx1 failure for the database.create.failure_cleanup policy family using profile remove_or_quarantine_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `preserve_evidence` default `required_by_default`: Requires preserve evidence for the database.create.failure_cleanup policy family using profile remove_or_quarantine_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `guess_identity=false` default `false`: Requires guess identity equals false for the database.create.failure_cleanup policy family using profile remove_or_quarantine_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.

### 004. `database.bootstrap.tx1`

- Default profile: `system_structure_seed_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `tx_number` default `required_by_default`: Requires tx number for the database.bootstrap.tx1 policy family using profile system_structure_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `sys_schema` default `required_by_default`: Requires sys schema for the database.bootstrap.tx1 policy family using profile system_structure_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `users_schema` default `required_by_default`: Requires users schema for the database.bootstrap.tx1 policy family using profile system_structure_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `users_public` default `required_by_default`: Requires users public for the database.bootstrap.tx1 policy family using profile system_structure_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `remote_schema` default `required_by_default`: Requires remote schema for the database.bootstrap.tx1 policy family using profile system_structure_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `emulated_schema` default `required_by_default`: Requires emulated schema for the database.bootstrap.tx1 policy family using profile system_structure_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `cluster_schema` default `required_by_default`: Requires cluster schema for the database.bootstrap.tx1 policy family using profile system_structure_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.

### 005. `database.first_open.tx2_activation`

- Default profile: `runtime_activation_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `tx_number` default `required_by_default`: Requires tx number for the database.first_open.tx2_activation policy family using profile runtime_activation_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `start_agents_after_tx2` default `required_by_default`: Requires start agents after tx2 for the database.first_open.tx2_activation policy family using profile runtime_activation_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `start_ipc_after_policy_load` default `required_by_default`: Requires start ipc after policy load for the database.first_open.tx2_activation policy family using profile runtime_activation_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.
  - `ordinary_work_before_tx2` default `required_by_default`: Requires ordinary work before tx2 for the database.first_open.tx2_activation policy family using profile runtime_activation_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle create/open; catalog bootstrap; identity reconciliation.

### 006. `schema.bootstrap.roots`

- Default profile: `local_roots_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `roots` default `required_by_default`: Requires roots for the schema.bootstrap.roots policy family using profile local_roots_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: catalog root bootstrap; name/UUID resolver; schema visibility filters.
  - `cluster_roots` default `required_by_default`: Requires cluster roots for the schema.bootstrap.roots policy family using profile local_roots_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: catalog root bootstrap; name/UUID resolver; schema visibility filters.
  - `all_objects_uuidv7` default `required_by_default`: Requires all objects uuidv7 for the schema.bootstrap.roots policy family using profile local_roots_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: catalog root bootstrap; name/UUID resolver; schema visibility filters.
  - `common_name_uuid_tables` default `required_by_default`: Requires common name uuid tables for the schema.bootstrap.roots policy family using profile local_roots_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: catalog root bootstrap; name/UUID resolver; schema visibility filters.

### 007. `security.authority_selection`

- Default profile: `database_local_internal_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `authority_class` default `required_by_default`: Requires authority class for the security.authority_selection policy family using profile database_local_internal_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `fallback_security_database` default `required_by_default`: Requires fallback security database for the security.authority_selection policy family using profile database_local_internal_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `no_security_database_policy` default `required_by_default`: Requires no security database policy for the security.authority_selection policy family using profile database_local_internal_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `embedded_bootstrap_allowed` default `required_by_default`: Requires embedded bootstrap allowed for the security.authority_selection policy family using profile database_local_internal_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 008. `security.authentication_provider`

- Default profile: `engine_hash_provider_v1`
- State: `enabled`
- Override class: `security_admin`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `plugin_required` default `required_by_default`: Requires plugin required for the security.authentication_provider policy family using profile engine_hash_provider_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `cleartext_password_storage` default `false`: Requires cleartext password storage for the security.authentication_provider policy family using profile engine_hash_provider_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `compare_password_hash` default `required_by_default`: Requires compare password hash for the security.authentication_provider policy family using profile engine_hash_provider_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `provider_failure` default `required_by_default`: Requires provider failure for the security.authentication_provider policy family using profile engine_hash_provider_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 009. `security.bootstrap_password`

- Default profile: `must_change_or_replace_v1`
- State: `enabled`
- Override class: `security_admin`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `initial_sysarch_equivalent` default `required_by_default`: Requires initial sysarch equivalent for the security.bootstrap_password policy family using profile must_change_or_replace_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `password_policy` default `required_by_default`: Requires password policy for the security.bootstrap_password policy family using profile must_change_or_replace_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `default_password_allowed` default `false`: Requires default password allowed for the security.bootstrap_password policy family using profile must_change_or_replace_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `hash_only` default `required_by_default`: Requires hash only for the security.bootstrap_password policy family using profile must_change_or_replace_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 010. `security.authorization_default`

- Default profile: `default_deny_explicit_allow_v1`
- State: `enabled`
- Override class: `security_admin`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `default_action` default `required_by_default`: Requires default action for the security.authorization_default policy family using profile default_deny_explicit_allow_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `grant_sources` default `required_by_default`: Requires grant sources for the security.authorization_default policy family using profile default_deny_explicit_allow_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `deny_rules_supported` default `required_by_default`: Requires deny rules supported for the security.authorization_default policy family using profile default_deny_explicit_allow_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `hidden_object_disclosure` default `required_by_default`: Requires hidden object disclosure for the security.authorization_default policy family using profile default_deny_explicit_allow_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 011. `security.principal_role_group_seed`

- Default profile: `sysarch_public_user_home_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `bootstrap_sysarch_equivalent` default `required_by_default`: Requires bootstrap sysarch equivalent for the security.principal_role_group_seed policy family using profile sysarch_public_user_home_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `public_group` default `required_by_default`: Requires public group for the security.principal_role_group_seed policy family using profile sysarch_public_user_home_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `users_public_schema` default `required_by_default`: Requires users public schema for the security.principal_role_group_seed policy family using profile sysarch_public_user_home_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `new_user_home_schema` default `required_by_default`: Requires new user home schema for the security.principal_role_group_seed policy family using profile sysarch_public_user_home_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 012. `security.user_home_schema`

- Default profile: `users_tree_default_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `home_root` default `required_by_default`: Requires home root for the security.user_home_schema policy family using profile users_tree_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `default_home_path` default `required_by_default`: Requires default home path for the security.user_home_schema policy family using profile users_tree_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `allow_alternate_home_root` default `required_by_default`: Requires allow alternate home root for the security.user_home_schema policy family using profile users_tree_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `cluster_users` default `required_by_default`: Requires cluster users for the security.user_home_schema policy family using profile users_tree_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 013. `security.audit`

- Default profile: `security_activity_audit_v1`
- State: `enabled`
- Override class: `security_admin`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `audit_security_events` default `required_by_default`: Requires audit security events for the security.audit policy family using profile security_activity_audit_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `audit_policy_mutation` default `required_by_default`: Requires audit policy mutation for the security.audit policy family using profile security_activity_audit_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `audit_create_database` default `required_by_default`: Requires audit create database for the security.audit policy family using profile security_activity_audit_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `tamper_evidence` default `required_by_default`: Requires tamper evidence for the security.audit policy family using profile security_activity_audit_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `failure_behavior` default `required_by_default`: Requires failure behavior for the security.audit policy family using profile security_activity_audit_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 014. `security.redaction`

- Default profile: `least_disclosure_v1`
- State: `enabled`
- Override class: `security_admin`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `client_default` default `required_by_default`: Requires client default for the security.redaction policy family using profile least_disclosure_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `operator_requires_right` default `required_by_default`: Requires operator requires right for the security.redaction policy family using profile least_disclosure_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `protected_material` default `required_by_default`: Requires protected material for the security.redaction policy family using profile least_disclosure_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `parser_sql_text` default `required_by_default`: Requires parser sql text for the security.redaction policy family using profile least_disclosure_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 015. `security.protected_material`

- Default profile: `reference_only_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `store_secret_material` default `required_by_default`: Requires store secret material for the security.protected_material policy family using profile reference_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `secret_refs_allowed` default `required_by_default`: Requires secret refs allowed for the security.protected_material policy family using profile reference_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `key_release_requires_authority` default `required_by_default`: Requires key release requires authority for the security.protected_material policy family using profile reference_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `diagnostic_redaction` default `required_by_default`: Requires diagnostic redaction for the security.protected_material policy family using profile reference_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 016. `security.encryption_key_admission`

- Default profile: `unencrypted_default_key_required_if_encrypted_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `database_encryption_default` default `required_by_default`: Requires database encryption default for the security.encryption_key_admission policy family using profile unencrypted_default_key_required_if_encrypted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `filespace_key_from_database` default `required_by_default`: Requires filespace key from database for the security.encryption_key_admission policy family using profile unencrypted_default_key_required_if_encrypted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `encrypted_open_without_key` default `required_by_default`: Requires encrypted open without key for the security.encryption_key_admission policy family using profile unencrypted_default_key_required_if_encrypted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.
  - `key_cache_purge_on_shutdown` default `required_by_default`: Requires key cache purge on shutdown for the security.encryption_key_admission policy family using profile unencrypted_default_key_required_if_encrypted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: authentication provider; authorization cache; security catalog and audit.

### 017. `configuration.source_precedence`

- Default profile: `compiled_then_durable_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `pre_mount_order` default `required_by_default`: Requires pre mount order for the configuration.source_precedence policy family using profile compiled_then_durable_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.
  - `post_mount_durable_wins` default `required_by_default`: Requires post mount durable wins for the configuration.source_precedence policy family using profile compiled_then_durable_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.
  - `silent_fallback_on_invalid` default `required_by_default`: Requires silent fallback on invalid for the configuration.source_precedence policy family using profile compiled_then_durable_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.
  - `unknown_key` default `required_by_default`: Requires unknown key for the configuration.source_precedence policy family using profile compiled_then_durable_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.

### 018. `configuration.override_reload`

- Default profile: `safe_reload_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `bootstrap_override_after_mount` default `required_by_default`: Requires bootstrap override after mount for the configuration.override_reload policy family using profile safe_reload_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.
  - `reload_requires_generation` default `required_by_default`: Requires reload requires generation for the configuration.override_reload policy family using profile safe_reload_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.
  - `stale_policy` default `required_by_default`: Requires stale policy for the configuration.override_reload policy family using profile safe_reload_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.
  - `unsafe_reload` default `required_by_default`: Requires unsafe reload for the configuration.override_reload policy family using profile safe_reload_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server configuration loader; policy reload path; durable catalog policy image.

### 019. `resource.seed_i18n`

- Default profile: `required_seed_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `timezone` default `required_by_default`: Requires timezone for the resource.seed_i18n policy family using profile required_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.
  - `charset` default `required_by_default`: Requires charset for the resource.seed_i18n policy family using profile required_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.
  - `collation` default `required_by_default`: Requires collation for the resource.seed_i18n policy family using profile required_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.
  - `locale` default `required_by_default`: Requires locale for the resource.seed_i18n policy family using profile required_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.
  - `missing_resource_blocks_ordinary_open` default `required_by_default`: Requires missing resource blocks ordinary open for the resource.seed_i18n policy family using profile required_seed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.

### 020. `resource.signature_provenance`

- Default profile: `unsigned_local_seed_allowed_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `pack_signature_required` default `required_by_default`: Requires pack signature required for the resource.signature_provenance policy family using profile unsigned_local_seed_allowed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.
  - `source_hash_required` default `required_by_default`: Requires source hash required for the resource.signature_provenance policy family using profile unsigned_local_seed_allowed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.
  - `version_required` default `required_by_default`: Requires version required for the resource.signature_provenance policy family using profile unsigned_local_seed_allowed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.
  - `unsupported_version` default `required_by_default`: Requires unsupported version for the resource.signature_provenance policy family using profile unsigned_local_seed_allowed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource seed-pack loader; i18n resource activation; create database bootstrap.

### 021. `storage.filespace_profile`

- Default profile: `single_active_primary_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `first_filespace_uuid` default `required_by_default`: Requires first filespace uuid for the storage.filespace_profile policy family using profile single_active_primary_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `active_primary_required` default `required_by_default`: Requires active primary required for the storage.filespace_profile policy family using profile single_active_primary_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `secondary_filespaces` default `required_by_default`: Requires secondary filespaces for the storage.filespace_profile policy family using profile single_active_primary_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `path_not_identity` default `required_by_default`: Requires path not identity for the storage.filespace_profile policy family using profile single_active_primary_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.

### 022. `storage.filespace_lifecycle`

- Default profile: `strict_identity_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `stale_missing_duplicate` default `required_by_default`: Requires stale missing duplicate for the storage.filespace_lifecycle policy family using profile strict_identity_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `verify_no_repair_without_authority` default `required_by_default`: Requires verify no repair without authority for the storage.filespace_lifecycle policy family using profile strict_identity_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `drop_active_pins` default `required_by_default`: Requires drop active pins for the storage.filespace_lifecycle policy family using profile strict_identity_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `quarantine_on_ambiguous` default `required_by_default`: Requires quarantine on ambiguous for the storage.filespace_lifecycle policy family using profile strict_identity_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.

### 023. `storage.allocation_freespace_pagemap`

- Default profile: `durable_map_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `free_space_map` default `required_by_default`: Requires free space map for the storage.allocation_freespace_pagemap policy family using profile durable_map_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `page_ownership` default `required_by_default`: Requires page ownership for the storage.allocation_freespace_pagemap policy family using profile durable_map_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `reusable_space_after_mga_cleanup` default `required_by_default`: Requires reusable space after mga cleanup for the storage.allocation_freespace_pagemap policy family using profile durable_map_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.
  - `physical_order_not_authority` default `required_by_default`: Requires physical order not authority for the storage.allocation_freespace_pagemap policy family using profile durable_map_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: filespace manager; page allocator; free-space/page-map agent.

### 024. `lifecycle.ownership_stale_owner`

- Default profile: `exclusive_owner_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `single_owner` default `required_by_default`: Requires single owner for the lifecycle.ownership_stale_owner policy family using profile exclusive_owner_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `heartbeat_required` default `required_by_default`: Requires heartbeat required for the lifecycle.ownership_stale_owner policy family using profile exclusive_owner_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `ambiguous_owner` default `required_by_default`: Requires ambiguous owner for the lifecycle.ownership_stale_owner policy family using profile exclusive_owner_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `stale_proof_required` default `required_by_default`: Requires stale proof required for the lifecycle.ownership_stale_owner policy family using profile exclusive_owner_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.

### 025. `lifecycle.recovery_dirty_open`

- Default profile: `mga_recovery_first_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `clean_open` default `required_by_default`: Requires clean open for the lifecycle.recovery_dirty_open policy family using profile mga_recovery_first_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `dirty_open` default `required_by_default`: Requires dirty open for the lifecycle.recovery_dirty_open policy family using profile mga_recovery_first_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `ambiguous` default `required_by_default`: Requires ambiguous for the lifecycle.recovery_dirty_open policy family using profile mga_recovery_first_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `corrupt` default `required_by_default`: Requires corrupt for the lifecycle.recovery_dirty_open policy family using profile mga_recovery_first_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `wal_not_authority` default `required_by_default`: Requires wal not authority for the lifecycle.recovery_dirty_open policy family using profile mga_recovery_first_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.

### 026. `lifecycle.maintenance_restricted`

- Default profile: `authorized_fence_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `enter_requires_authority` default `required_by_default`: Requires enter requires authority for the lifecycle.maintenance_restricted policy family using profile authorized_fence_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `ordinary_attach_blocked` default `required_by_default`: Requires ordinary attach blocked for the lifecycle.maintenance_restricted policy family using profile authorized_fence_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `verify_allowed` default `required_by_default`: Requires verify allowed for the lifecycle.maintenance_restricted policy family using profile authorized_fence_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `repair_requires_explicit_authority` default `required_by_default`: Requires repair requires explicit authority for the lifecycle.maintenance_restricted policy family using profile authorized_fence_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.

### 027. `lifecycle.shutdown_graceful_drain`

- Default profile: `drain_then_close_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `default_drain_ms` default `required_by_default`: Requires default drain ms for the lifecycle.shutdown_graceful_drain policy family using profile drain_then_close_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `fence_new_work` default `required_by_default`: Requires fence new work for the lifecycle.shutdown_graceful_drain policy family using profile drain_then_close_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `notify_associated_components` default `required_by_default`: Requires notify associated components for the lifecycle.shutdown_graceful_drain policy family using profile drain_then_close_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `close_after_commit_or_rollback` default `required_by_default`: Requires close after commit or rollback for the lifecycle.shutdown_graceful_drain policy family using profile drain_then_close_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `timeout_without_force` default `required_by_default`: Requires timeout without force for the lifecycle.shutdown_graceful_drain policy family using profile drain_then_close_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.

### 028. `lifecycle.shutdown_force`

- Default profile: `explicit_force_only_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `implicit_escalation` default `required_by_default`: Requires implicit escalation for the lifecycle.shutdown_force policy family using profile explicit_force_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `terminate_target_database_scope_only` default `required_by_default`: Requires terminate target database scope only for the lifecycle.shutdown_force policy family using profile explicit_force_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `preserve_mga_recovery_evidence` default `required_by_default`: Requires preserve mga recovery evidence for the lifecycle.shutdown_force policy family using profile explicit_force_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.
  - `unrelated_database_protected` default `required_by_default`: Requires unrelated database protected for the lifecycle.shutdown_force policy family using profile explicit_force_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: database lifecycle manager; maintenance fences; shutdown handling.

### 029. `transaction.admission`

- Default profile: `engine_mga_admission_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `requires_ownership` default `required_by_default`: Requires requires ownership for the transaction.admission policy family using profile engine_mga_admission_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `requires_security` default `required_by_default`: Requires requires security for the transaction.admission policy family using profile engine_mga_admission_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `requires_policy_generation` default `required_by_default`: Requires requires policy generation for the transaction.admission policy family using profile engine_mga_admission_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `requires_catalog_snapshot` default `required_by_default`: Requires requires catalog snapshot for the transaction.admission policy family using profile engine_mga_admission_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `requires_filespace_valid` default `required_by_default`: Requires requires filespace valid for the transaction.admission policy family using profile engine_mga_admission_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.

### 030. `transaction.default_isolation_snapshot`

- Default profile: `read_committed_mga_snapshot_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `default_isolation` default `required_by_default`: Requires default isolation for the transaction.default_isolation_snapshot policy family using profile read_committed_mga_snapshot_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `snapshot_source` default `required_by_default`: Requires snapshot source for the transaction.default_isolation_snapshot policy family using profile read_committed_mga_snapshot_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `security_epoch_captured` default `required_by_default`: Requires security epoch captured for the transaction.default_isolation_snapshot policy family using profile read_committed_mga_snapshot_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `catalog_epoch_captured` default `required_by_default`: Requires catalog epoch captured for the transaction.default_isolation_snapshot policy family using profile read_committed_mga_snapshot_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.

### 031. `transaction.commit_durability`

- Default profile: `inventory_sync_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `commit_authority` default `required_by_default`: Requires commit authority for the transaction.commit_durability policy family using profile inventory_sync_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `success_after_sync_policy` default `required_by_default`: Requires success after sync policy for the transaction.commit_durability policy family using profile inventory_sync_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `wal_finality` default `required_by_default`: Requires wal finality for the transaction.commit_durability policy family using profile inventory_sync_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `cache_flush_not_finality` default `required_by_default`: Requires cache flush not finality for the transaction.commit_durability policy family using profile inventory_sync_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.

### 032. `transaction.rollback_savepoint_limbo`

- Default profile: `mga_owned_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `rollback_engine_owned` default `required_by_default`: Requires rollback engine owned for the transaction.rollback_savepoint_limbo policy family using profile mga_owned_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `savepoints_transaction_local` default `required_by_default`: Requires savepoints transaction local for the transaction.rollback_savepoint_limbo policy family using profile mga_owned_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `disconnect_unknown_outcome` default `required_by_default`: Requires disconnect unknown outcome for the transaction.rollback_savepoint_limbo policy family using profile mga_owned_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `limbo_requires_recovery_policy` default `required_by_default`: Requires limbo requires recovery policy for the transaction.rollback_savepoint_limbo policy family using profile mga_owned_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.

### 033. `transaction.mga_gc_retention`

- Default profile: `safe_bounded_cleanup_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `cleanup_requires_horizon` default `required_by_default`: Requires cleanup requires horizon for the transaction.mga_gc_retention policy family using profile safe_bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `backup_hold_respected` default `required_by_default`: Requires backup hold respected for the transaction.mga_gc_retention policy family using profile safe_bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `archive_hold_respected` default `required_by_default`: Requires archive hold respected for the transaction.mga_gc_retention policy family using profile safe_bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `unknown_outcome_protected` default `required_by_default`: Requires unknown outcome protected for the transaction.mga_gc_retention policy family using profile safe_bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.
  - `bounded_memory` default `required_by_default`: Requires bounded memory for the transaction.mga_gc_retention policy family using profile safe_bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: MGA transaction manager; commit/rollback path; visibility horizon.

### 034. `concurrency.lock_wait_deadlock`

- Default profile: `bounded_wait_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `default_lock_wait_ms` default `required_by_default`: Requires default lock wait ms for the concurrency.lock_wait_deadlock policy family using profile bounded_wait_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: lock manager; deadlock detector; transaction admission.
  - `deadlock_detection` default `required_by_default`: Requires deadlock detection for the concurrency.lock_wait_deadlock policy family using profile bounded_wait_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: lock manager; deadlock detector; transaction admission.
  - `disconnect_cleanup` default `required_by_default`: Requires disconnect cleanup for the concurrency.lock_wait_deadlock policy family using profile bounded_wait_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: lock manager; deadlock detector; transaction admission.
  - `victim_policy` default `required_by_default`: Requires victim policy for the concurrency.lock_wait_deadlock policy family using profile bounded_wait_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: lock manager; deadlock detector; transaction admission.

### 035. `cache.checkpoint_preload_flush`

- Default profile: `evidence_not_finality_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `preload_after_tx2` default `required_by_default`: Requires preload after tx2 for the cache.checkpoint_preload_flush policy family using profile evidence_not_finality_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: page cache manager; checkpoint preload/flush agents; open database policy image.
  - `flush_on_shutdown` default `required_by_default`: Requires flush on shutdown for the cache.checkpoint_preload_flush policy family using profile evidence_not_finality_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: page cache manager; checkpoint preload/flush agents; open database policy image.
  - `checkpoint_is_clean_close_evidence_only` default `required_by_default`: Requires checkpoint is clean close evidence only for the cache.checkpoint_preload_flush policy family using profile evidence_not_finality_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: page cache manager; checkpoint preload/flush agents; open database policy image.
  - `cache_not_finality` default `required_by_default`: Requires cache not finality for the cache.checkpoint_preload_flush policy family using profile evidence_not_finality_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: page cache manager; checkpoint preload/flush agents; open database policy image.

### 036. `backup.archive_restore_snapshot_shadow`

- Default profile: `engine_owned_no_live_shortcut_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `backup_requires_engine_path` default `required_by_default`: Requires backup requires engine path for the backup.archive_restore_snapshot_shadow policy family using profile engine_owned_no_live_shortcut_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: backup service; archive/restore admission; database lifecycle.
  - `live_file_shortcut` default `required_by_default`: Requires live file shortcut for the backup.archive_restore_snapshot_shadow policy family using profile engine_owned_no_live_shortcut_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: backup service; archive/restore admission; database lifecycle.
  - `restore_inspection_mode` default `required_by_default`: Requires restore inspection mode for the backup.archive_restore_snapshot_shadow policy family using profile engine_owned_no_live_shortcut_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: backup service; archive/restore admission; database lifecycle.
  - `legal_hold_respected` default `required_by_default`: Requires legal hold respected for the backup.archive_restore_snapshot_shadow policy family using profile engine_owned_no_live_shortcut_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: backup service; archive/restore admission; database lifecycle.

### 037. `workload.resource_quota`

- Default profile: `safe_local_default_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `max_connections` default `required_by_default`: Requires max connections for the workload.resource_quota policy family using profile safe_local_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource quota agent; admission control; memory/cache policy.
  - `max_active_requests` default `required_by_default`: Requires max active requests for the workload.resource_quota policy family using profile safe_local_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource quota agent; admission control; memory/cache policy.
  - `max_open_cursors_per_session` default `required_by_default`: Requires max open cursors per session for the workload.resource_quota policy family using profile safe_local_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource quota agent; admission control; memory/cache policy.
  - `memory_pressure` default `required_by_default`: Requires memory pressure for the workload.resource_quota policy family using profile safe_local_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: resource quota agent; admission control; memory/cache policy.

### 038. `temp.spill_workspace`

- Default profile: `bounded_cleanup_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `temp_catalog_durable` default `required_by_default`: Requires temp catalog durable for the temp.spill_workspace policy family using profile bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: temporary workspace manager; spill cleanup agent; resource quota checks.
  - `cleanup_on_commit` default `required_by_default`: Requires cleanup on commit for the temp.spill_workspace policy family using profile bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: temporary workspace manager; spill cleanup agent; resource quota checks.
  - `cleanup_on_rollback` default `required_by_default`: Requires cleanup on rollback for the temp.spill_workspace policy family using profile bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: temporary workspace manager; spill cleanup agent; resource quota checks.
  - `cleanup_on_disconnect` default `required_by_default`: Requires cleanup on disconnect for the temp.spill_workspace policy family using profile bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: temporary workspace manager; spill cleanup agent; resource quota checks.
  - `spill_encryption` default `required_by_default`: Requires spill encryption for the temp.spill_workspace policy family using profile bounded_cleanup_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: temporary workspace manager; spill cleanup agent; resource quota checks.

### 039. `session.disconnect_timeout`

- Default profile: `explicit_unknown_outcome_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `idle_timeout_ms` default `required_by_default`: Requires idle timeout ms for the session.disconnect_timeout policy family using profile explicit_unknown_outcome_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: session manager; disconnect handling; transaction cleanup.
  - `statement_timeout_ms` default `required_by_default`: Requires statement timeout ms for the session.disconnect_timeout policy family using profile explicit_unknown_outcome_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: session manager; disconnect handling; transaction cleanup.
  - `disconnect_does_not_commit` default `required_by_default`: Requires disconnect does not commit for the session.disconnect_timeout policy family using profile explicit_unknown_outcome_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: session manager; disconnect handling; transaction cleanup.
  - `unknown_outcome_message_vector` default `required_by_default`: Requires unknown outcome message vector for the session.disconnect_timeout policy family using profile explicit_unknown_outcome_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: session manager; disconnect handling; transaction cleanup.

### 040. `server.route_listener_startup`

- Default profile: `local_disabled_until_configured_v1`
- State: `enabled`
- Override class: `create_database_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `network_listener_default` default `required_by_default`: Requires network listener default for the server.route_listener_startup policy family using profile local_disabled_until_configured_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server route startup; listener/manager bootstrap; configuration policy.
  - `loopback_default` default `required_by_default`: Requires loopback default for the server.route_listener_startup policy family using profile local_disabled_until_configured_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server route startup; listener/manager bootstrap; configuration policy.
  - `native_port` default `required_by_default`: Requires native port for the server.route_listener_startup policy family using profile local_disabled_until_configured_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server route startup; listener/manager bootstrap; configuration policy.
  - `start_after_security_policy` default `required_by_default`: Requires start after security policy for the server.route_listener_startup policy family using profile local_disabled_until_configured_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: server route startup; listener/manager bootstrap; configuration policy.

### 041. `listener.bind_tls_pool`

- Default profile: `secure_bind_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `tls_required_for_inet` default `required_by_default`: Requires tls required for inet for the listener.bind_tls_pool policy family using profile secure_bind_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: listener bind path; TLS/session pool; route admission.
  - `unix_socket_allowed_local` default `required_by_default`: Requires unix socket allowed local for the listener.bind_tls_pool policy family using profile secure_bind_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: listener bind path; TLS/session pool; route admission.
  - `parser_pool_min` default `required_by_default`: Requires parser pool min for the listener.bind_tls_pool policy family using profile secure_bind_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: listener bind path; TLS/session pool; route admission.
  - `parser_pool_max` default `required_by_default`: Requires parser pool max for the listener.bind_tls_pool policy family using profile secure_bind_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: listener bind path; TLS/session pool; route admission.
  - `reuseaddr_policy` default `required_by_default`: Requires reuseaddr policy for the listener.bind_tls_pool policy family using profile secure_bind_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: listener bind path; TLS/session pool; route admission.

### 042. `parser.package_admission`

- Default profile: `registered_packages_only_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `unregistered_parser` default `required_by_default`: Requires unregistered parser for the parser.package_admission policy family using profile registered_packages_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: parser package admission; SBParser route; dynamic SBsql lowering boundary.
  - `sbsql_profile` default `required_by_default`: Requires sbsql profile for the parser.package_admission policy family using profile registered_packages_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: parser package admission; SBParser route; dynamic SBsql lowering boundary.
  - `reference_profile` default `required_by_default`: Requires reference profile for the parser.package_admission policy family using profile registered_packages_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: parser package admission; SBParser route; dynamic SBsql lowering boundary.
  - `parser_auth_authority` default `required_by_default`: Requires parser auth authority for the parser.package_admission policy family using profile registered_packages_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: parser package admission; SBParser route; dynamic SBsql lowering boundary.

### 043. `ipc.frame_auth_backpressure`

- Default profile: `authenticated_framed_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `max_frame_bytes` default `required_by_default`: Requires max frame bytes for the ipc.frame_auth_backpressure policy family using profile authenticated_framed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: IPC frame validator; parser/server transport; backpressure controller.
  - `malformed_frame` default `required_by_default`: Requires malformed frame for the ipc.frame_auth_backpressure policy family using profile authenticated_framed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: IPC frame validator; parser/server transport; backpressure controller.
  - `backpressure` default `required_by_default`: Requires backpressure for the ipc.frame_auth_backpressure policy family using profile authenticated_framed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: IPC frame validator; parser/server transport; backpressure controller.
  - `endpoint_descriptor_required` default `required_by_default`: Requires endpoint descriptor required for the ipc.frame_auth_backpressure policy family using profile authenticated_framed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: IPC frame validator; parser/server transport; backpressure controller.

### 044. `udr.extension_trust_resource`

- Default profile: `cxx_registered_only_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `trusted_udr_language` default `required_by_default`: Requires trusted udr language for the udr.extension_trust_resource policy family using profile cxx_registered_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: UDR loader; extension trust policy; dynamic SBsql parser UDR.
  - `dynamic_load_requires_policy` default `required_by_default`: Requires dynamic load requires policy for the udr.extension_trust_resource policy family using profile cxx_registered_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: UDR loader; extension trust policy; dynamic SBsql parser UDR.
  - `resource_limits` default `required_by_default`: Requires resource limits for the udr.extension_trust_resource policy family using profile cxx_registered_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: UDR loader; extension trust policy; dynamic SBsql parser UDR.
  - `unload_or_quiesce_required` default `required_by_default`: Requires unload or quiesce required for the udr.extension_trust_resource policy family using profile cxx_registered_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: UDR loader; extension trust policy; dynamic SBsql parser UDR.

### 045. `executable.side_effect`

- Default profile: `side_effects_disabled_by_default_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `routine_side_effects` default `required_by_default`: Requires routine side effects for the executable.side_effect policy family using profile side_effects_disabled_by_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: procedural object executor; UDR admission; side-effect guard.
  - `trigger_side_effects` default `required_by_default`: Requires trigger side effects for the executable.side_effect policy family using profile side_effects_disabled_by_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: procedural object executor; UDR admission; side-effect guard.
  - `external_outbox_requires_policy` default `required_by_default`: Requires external outbox requires policy for the executable.side_effect policy family using profile side_effects_disabled_by_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: procedural object executor; UDR admission; side-effect guard.
  - `definer_rights` default `required_by_default`: Requires definer rights for the executable.side_effect policy family using profile side_effects_disabled_by_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: procedural object executor; UDR admission; side-effect guard.

### 046. `sequence.generator_cache`

- Default profile: `bounded_nonfinality_cache_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `default_cache_size` default `required_by_default`: Requires default cache size for the sequence.generator_cache policy family using profile bounded_nonfinality_cache_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: sequence generator; transaction commit hooks; cache invalidation.
  - `cache_not_transaction_finality` default `required_by_default`: Requires cache not transaction finality for the sequence.generator_cache policy family using profile bounded_nonfinality_cache_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: sequence generator; transaction commit hooks; cache invalidation.
  - `crash_gaps_allowed` default `required_by_default`: Requires crash gaps allowed for the sequence.generator_cache policy family using profile bounded_nonfinality_cache_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: sequence generator; transaction commit hooks; cache invalidation.
  - `reference_mapping_requires_profile` default `required_by_default`: Requires reference mapping requires profile for the sequence.generator_cache policy family using profile bounded_nonfinality_cache_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: sequence generator; transaction commit hooks; cache invalidation.

### 047. `event.queue_notification`

- Default profile: `bounded_volatile_default_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `max_payload_bytes` default `required_by_default`: Requires max payload bytes for the event.queue_notification policy family using profile bounded_volatile_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: event queue; notification dispatcher; transaction commit hooks.
  - `max_queued_events` default `required_by_default`: Requires max queued events for the event.queue_notification policy family using profile bounded_volatile_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: event queue; notification dispatcher; transaction commit hooks.
  - `retention_seconds` default `required_by_default`: Requires retention seconds for the event.queue_notification policy family using profile bounded_volatile_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: event queue; notification dispatcher; transaction commit hooks.
  - `overflow_behavior` default `required_by_default`: Requires overflow behavior for the event.queue_notification policy family using profile bounded_volatile_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: event queue; notification dispatcher; transaction commit hooks.
  - `security_filtering` default `required_by_default`: Requires security filtering for the event.queue_notification policy family using profile bounded_volatile_default_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: event queue; notification dispatcher; transaction commit hooks.

### 048. `diagnostics.message_vector`

- Default profile: `canonical_redacted_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `raw_strings_forbidden` default `required_by_default`: Requires raw strings forbidden for the diagnostics.message_vector policy family using profile canonical_redacted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: diagnostic message renderer; error redaction; client result envelopes.
  - `redaction_required` default `required_by_default`: Requires redaction required for the diagnostics.message_vector policy family using profile canonical_redacted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: diagnostic message renderer; error redaction; client result envelopes.
  - `reference_mapping_generic_on_failure` default `required_by_default`: Requires reference mapping generic on failure for the diagnostics.message_vector policy family using profile canonical_redacted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: diagnostic message renderer; error redaction; client result envelopes.
  - `correlation_id_required` default `required_by_default`: Requires correlation id required for the diagnostics.message_vector policy family using profile canonical_redacted_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: diagnostic message renderer; error redaction; client result envelopes.

### 049. `observability.metrics_log`

- Default profile: `local_metrics_enabled_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `metrics_enabled` default `required_by_default`: Requires metrics enabled for the observability.metrics_log policy family using profile local_metrics_enabled_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: metrics writer; operational log policy; diagnostic reporting.
  - `flush_interval_ms` default `required_by_default`: Requires flush interval ms for the observability.metrics_log policy family using profile local_metrics_enabled_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: metrics writer; operational log policy; diagnostic reporting.
  - `local_root` default `required_by_default`: Requires local root for the observability.metrics_log policy family using profile local_metrics_enabled_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: metrics writer; operational log policy; diagnostic reporting.
  - `cluster_metrics_absent_without_cluster` default `required_by_default`: Requires cluster metrics absent without cluster for the observability.metrics_log policy family using profile local_metrics_enabled_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: metrics writer; operational log policy; diagnostic reporting.
  - `private_paths_redacted` default `required_by_default`: Requires private paths redacted for the observability.metrics_log policy family using profile local_metrics_enabled_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: metrics writer; operational log policy; diagnostic reporting.

### 050. `support.bundle`

- Default profile: `disabled_until_authorized_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `default_enabled` default `required_by_default`: Requires default enabled for the support.bundle policy family using profile disabled_until_authorized_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: support bundle collector; sysarch authorization; redaction policy.
  - `requires_operator_right` default `required_by_default`: Requires requires operator right for the support.bundle policy family using profile disabled_until_authorized_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: support bundle collector; sysarch authorization; redaction policy.
  - `redaction_required` default `required_by_default`: Requires redaction required for the support.bundle policy family using profile disabled_until_authorized_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: support bundle collector; sysarch authorization; redaction policy.
  - `retention_days` default `required_by_default`: Requires retention days for the support.bundle policy family using profile disabled_until_authorized_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: support bundle collector; sysarch authorization; redaction policy.
  - `protected_material_excluded` default `required_by_default`: Requires protected material excluded for the support.bundle policy family using profile disabled_until_authorized_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: support bundle collector; sysarch authorization; redaction policy.

### 051. `evidence.retention`

- Default profile: `audit_minimum_v1`
- State: `enabled`
- Override class: `security_admin`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `lifecycle_evidence_days` default `required_by_default`: Requires lifecycle evidence days for the evidence.retention policy family using profile audit_minimum_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: audit evidence retention; support bundle collector; release proof gates.
  - `security_audit_days` default `required_by_default`: Requires security audit days for the evidence.retention policy family using profile audit_minimum_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: audit evidence retention; support bundle collector; release proof gates.
  - `diagnostic_evidence_days` default `required_by_default`: Requires diagnostic evidence days for the evidence.retention policy family using profile audit_minimum_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: audit evidence retention; support bundle collector; release proof gates.
  - `legal_hold_overrides_cleanup` default `required_by_default`: Requires legal hold overrides cleanup for the evidence.retention policy family using profile audit_minimum_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: audit evidence retention; support bundle collector; release proof gates.

### 052. `job.scheduler`

- Default profile: `start_after_tx2_v1`
- State: `enabled`
- Override class: `policy_defined`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `normal_jobs_after_tx2` default `required_by_default`: Requires normal jobs after tx2 for the job.scheduler policy family using profile start_after_tx2_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: scheduler agent; job catalog admission; post-open activation.
  - `startup_recovery_jobs_first` default `required_by_default`: Requires startup recovery jobs first for the job.scheduler policy family using profile start_after_tx2_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: scheduler agent; job catalog admission; post-open activation.
  - `maintenance_participation` default `required_by_default`: Requires maintenance participation for the job.scheduler policy family using profile start_after_tx2_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: scheduler agent; job catalog admission; post-open activation.
  - `failure_policy` default `required_by_default`: Requires failure policy for the job.scheduler policy family using profile start_after_tx2_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: scheduler agent; job catalog admission; post-open activation.
  - `retry_backoff_ms` default `required_by_default`: Requires retry backoff ms for the job.scheduler policy family using profile start_after_tx2_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: scheduler agent; job catalog admission; post-open activation.

### 053. `capability.feature_gate`

- Default profile: `installed_enabled_else_fail_closed_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `unknown_capability` default `required_by_default`: Requires unknown capability for the capability.feature_gate policy family using profile installed_enabled_else_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: feature gate manager; database open compatibility checks; release profile gates.
  - `edition_gate_required` default `required_by_default`: Requires edition gate required for the capability.feature_gate policy family using profile installed_enabled_else_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: feature gate manager; database open compatibility checks; release profile gates.
  - `parser_profile_requires_package` default `required_by_default`: Requires parser profile requires package for the capability.feature_gate policy family using profile installed_enabled_else_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: feature gate manager; database open compatibility checks; release profile gates.
  - `downgrade_refusal` default `required_by_default`: Requires downgrade refusal for the capability.feature_gate policy family using profile installed_enabled_else_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: feature gate manager; database open compatibility checks; release profile gates.

### 054. `upgrade.migration_refusal`

- Default profile: `explicit_supported_only_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `unknown_format` default `required_by_default`: Requires unknown format for the upgrade.migration_refusal policy family using profile explicit_supported_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: open compatibility classifier; migration refusal gate; catalog version policy.
  - `ambiguous_identity` default `required_by_default`: Requires ambiguous identity for the upgrade.migration_refusal policy family using profile explicit_supported_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: open compatibility classifier; migration refusal gate; catalog version policy.
  - `supported_migration_requires_plan` default `required_by_default`: Requires supported migration requires plan for the upgrade.migration_refusal policy family using profile explicit_supported_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: open compatibility classifier; migration refusal gate; catalog version policy.
  - `guess_identity=false` default `false`: Requires guess identity equals false for the upgrade.migration_refusal policy family using profile explicit_supported_only_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: open compatibility classifier; migration refusal gate; catalog version policy.

### 055. `admin.management_command_authorization`

- Default profile: `sysarch_or_delegated_v1`
- State: `enabled`
- Override class: `sysarch`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `lifecycle_commands_require_authority` default `required_by_default`: Requires lifecycle commands require authority for the admin.management_command_authorization policy family using profile sysarch_or_delegated_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: administration command admission; SBadm/SBmgr management routes; policy mutation authorization.
  - `force_shutdown_requires_explicit_right` default `required_by_default`: Requires force shutdown requires explicit right for the admin.management_command_authorization policy family using profile sysarch_or_delegated_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: administration command admission; SBadm/SBmgr management routes; policy mutation authorization.
  - `inspect_redacted_by_default` default `required_by_default`: Requires inspect redacted by default for the admin.management_command_authorization policy family using profile sysarch_or_delegated_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: administration command admission; SBadm/SBmgr management routes; policy mutation authorization.
  - `audit_required` default `required_by_default`: Requires audit required for the admin.management_command_authorization policy family using profile sysarch_or_delegated_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: administration command admission; SBadm/SBmgr management routes; policy mutation authorization.

### 056. `reference.emulation_profile`

- Default profile: `strict_not_authority_v1`
- State: `enabled`
- Override class: `no_override`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `reference_sql_exec_inside_engine` default `required_by_default`: Requires reference sql exec inside engine for the reference.emulation_profile policy family using profile strict_not_authority_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: reference-emulation boundary; compatibility parser admission; unsupported-feature gate.
  - `unsupported_reference_feature` default `required_by_default`: Requires unsupported reference feature for the reference.emulation_profile policy family using profile strict_not_authority_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: reference-emulation boundary; compatibility parser admission; unsupported-feature gate.
  - `reference_catalog_overlay_not_authority` default `required_by_default`: Requires reference catalog overlay not authority for the reference.emulation_profile policy family using profile strict_not_authority_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: reference-emulation boundary; compatibility parser admission; unsupported-feature gate.
  - `cross_dialect_dependency` default `required_by_default`: Requires cross dialect dependency for the reference.emulation_profile policy family using profile strict_not_authority_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: reference-emulation boundary; compatibility parser admission; unsupported-feature gate.

### 057. `replication.cdc_changefeed_boundary`

- Default profile: `disabled_fail_closed_v1`
- State: `fail_closed`
- Override class: `cluster_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `replication_enabled` default `fail_closed`: Requires replication enabled for the replication.cdc_changefeed_boundary policy family using profile disabled_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: CDC/changefeed boundary; cluster-only feature gate; fail-closed route policy.
  - `cdc_enabled` default `fail_closed`: Requires cdc enabled for the replication.cdc_changefeed_boundary policy family using profile disabled_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: CDC/changefeed boundary; cluster-only feature gate; fail-closed route policy.
  - `changefeed_enabled` default `fail_closed`: Requires changefeed enabled for the replication.cdc_changefeed_boundary policy family using profile disabled_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: CDC/changefeed boundary; cluster-only feature gate; fail-closed route policy.
  - `live_ingest_enabled` default `fail_closed`: Requires live ingest enabled for the replication.cdc_changefeed_boundary policy family using profile disabled_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: CDC/changefeed boundary; cluster-only feature gate; fail-closed route policy.
  - `slot_create` default `fail_closed`: Requires slot create for the replication.cdc_changefeed_boundary policy family using profile disabled_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: CDC/changefeed boundary; cluster-only feature gate; fail-closed route policy.
  - `publication_create` default `fail_closed`: Requires publication create for the replication.cdc_changefeed_boundary policy family using profile disabled_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: CDC/changefeed boundary; cluster-only feature gate; fail-closed route policy.

### 058. `cluster.boundary_fail_closed`

- Default profile: `standalone_fail_closed_v1`
- State: `enabled`
- Override class: `cluster_only`
- Catalog authority: `durable_catalog_after_create`
- Settings:
  - `cluster_schema_created` default `required_by_default`: Requires cluster schema created for the cluster.boundary_fail_closed policy family using profile standalone_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: cluster boundary stub; listener route negotiation; unsupported-feature gate.
  - `cluster_metrics_created` default `required_by_default`: Requires cluster metrics created for the cluster.boundary_fail_closed policy family using profile standalone_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: cluster boundary stub; listener route negotiation; unsupported-feature gate.
  - `cluster_routes` default `required_by_default`: Requires cluster routes for the cluster.boundary_fail_closed policy family using profile standalone_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: cluster boundary stub; listener route negotiation; unsupported-feature gate.
  - `cluster_transactions` default `required_by_default`: Requires cluster transactions for the cluster.boundary_fail_closed policy family using profile standalone_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: cluster boundary stub; listener route negotiation; unsupported-feature gate.
  - `cluster_agents` default `required_by_default`: Requires cluster agents for the cluster.boundary_fail_closed policy family using profile standalone_fail_closed_v1. The setting is seeded in tx1 and is enforced from the durable catalog after create. Used by: cluster boundary stub; listener route negotiation; unsupported-feature gate.
