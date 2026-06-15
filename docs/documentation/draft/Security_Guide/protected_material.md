# Protected Material

## Purpose

This page covers protected material: engine-held values that are treated as
secrets, credentials, encryption keys, tokens, or other release-controlled
content. Protected material is distinct from ordinary column data. The engine
does not return plaintext protected values through any ordinary query path,
diagnostic surface, or support bundle. Release requires an admitted release
route, a declared purpose, and an audit event.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/protected_material_api.hpp`,
`protected_material_api.cpp`, `src/core/catalog/catalog_records.hpp`,
`src/manager/node/manager_support_bundle.cpp`,
`src/listener/listener_support_bundle.cpp`,
`src/storage/page/late_payload_fetch.cpp`.

For the catalog surfaces, see:
- [Language Reference: sys.security.protected_material_catalog](../Language_Reference/catalog_reference/sys_security_protected_material_catalog.md)
- [Language Reference: sys.security.protected_material_version](../Language_Reference/catalog_reference/sys_security_protected_material_version.md)
- [Language Reference: sys.security.protected_material_policy_binding](../Language_Reference/catalog_reference/sys_security_protected_material_policy_binding.md)
- [Language Reference: sys.security.protected_material_audit](../Language_Reference/catalog_reference/sys_security_protected_material_audit.md)

For operator-visible concepts and the administrative surface, see:
- [Operations and Administration: Identity, Security, and Policy](../Operations_Administration/identity_security_and_policy.md)
- [Operations and Administration: Diagnostics, Message Vectors, and Support Bundles](../Operations_Administration/diagnostics_message_vectors_and_support_bundles.md)

## Definitions

**Protected material** — a secret, credential, encryption key, token,
protected binary, protected text, or other release-controlled value that the
engine owns. The engine holds the value and governs all access to it. Ordinary
SQL queries, driver metadata requests, diagnostic renderers, and support-bundle
generators cannot return plaintext protected material.

**Protected reference** — an opaque string stored in a version record that
identifies where the secret is held or how it is wrapped. It is not the secret
itself. The engine accepts this only in forms that do not embed plaintext (see
`PlaintextEvidenceRefused` gate below).

**Envelope reference** — an opaque string that identifies wrapping, key
derivation, or envelope metadata for a protected value. Like the protected
reference, it is never stored as plaintext.

**Payload hash** — a content-integrity hash computed over a protected payload.
It is kept after the protected reference is purged, enabling integrity checks
without retaining the value.

**Key handle** — a stable opaque identifier for an admitted encryption key
entry in the in-memory cache. The handle is an HMAC-SHA-256 token derived from
the key fingerprint, database UUID, filespace UUID, key UUID, and generation
counter. It never embeds the raw key.

**Key fingerprint** — an HMAC-SHA-256 token derived from the secret evidence,
the database UUID, the key UUID, and the filespace UUID. It uniquely identifies
a key admission without revealing the key.

**Release** — the admission of a protected material value for a specific
purpose. Release requires that the caller holds `PROTECTED_MATERIAL_RELEASE`,
that the purpose matches the material's `release_purposes` allowlist, and that
an audit event is written before the result is returned.

**Legal hold** — a flag on a protected material version (`legal_hold = true`
in `ProtectedMaterialVersionCatalogRecord`) that blocks all purge operations
regardless of retention policy. Legal hold must be explicitly cleared before
purge can proceed.

**Retention** — a time-based constraint (`retention_until_epoch_millis`) on a
version. Purge is refused if the current time is before this epoch and legal
hold is not the only reason.

**Rotation** — the addition of a new version that replaces the previous active
version. The prior version's `rotation_state` transitions from `"active"` to
`"rotated"`, and the new version becomes active. After purge, the
`rotation_state` becomes `"purged"`.

**Cache TTL** — the millisecond lifetime of an admitted encryption key entry in
the in-memory protected-material cache. The default is 300000 ms (5 minutes),
declared as the field default on `EngineAdmitEncryptionKeyRequest.cache_ttl_millis`
and `EngineRotateEncryptionKeyRequest.cache_ttl_millis`.

## What Protected Material Is Not

Protected material is not an ordinary column value. It does not appear in
`SELECT` results, `SHOW` output, `DESCRIBE` output, diagnostic messages,
support bundles, or audit rows in raw form. Every result struct in the API
carries `plaintext_material_returned = false` as a stated invariant, and
every inspect or export result carries `protected_material_redacted = true`.
Any call path that would require returning plaintext is refused with
`SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` or
`SECURITY.KEY.PLAINTEXT_REFUSED` before the value is exposed.

## Catalog Model

Source: `catalog_records.hpp` — `CatalogRecordKind` enum values 94–97.

The engine maintains four catalog record kinds for protected material:

| Record Kind | Enum Value | Struct | Purpose |
|-------------|-----------|--------|---------|
| `protected_material` | 94 | `ProtectedMaterialCatalogRecord` | Durable identity, lifecycle state, active version pointer, policy UUIDs |
| `protected_material_version` | 95 | `ProtectedMaterialVersionCatalogRecord` | Per-version references, rotation state, retention, legal hold |
| `protected_material_policy_binding` | 96 | `ProtectedMaterialPolicyBindingCatalogRecord` | Policy-to-material and policy-to-version bindings |
| `protected_material_audit_event` | 97 | `ProtectedMaterialAuditEventCatalogRecord` | Redacted, append-only audit evidence |

The four record kinds form a layered model:

```
protected_material (catalog entry)
│   protected_material_uuid
│   lifecycle_state: "active" | "retained_no_active_version"
│   active_version_uuid → points to the currently active version
│   purpose_class, storage_class
│   policy UUIDs (retention, access, release, purge, audit)
│
├── protected_material_version (one or more)
│   │   protected_material_version_uuid
│   │   version_number (monotonically increasing)
│   │   rotation_state: "active" | "rotated" | "purged"
│   │   protected_reference (cleared on purge)
│   │   envelope_reference (cleared on purge)
│   │   payload_hash (retained after purge for integrity)
│   │   retention_until_epoch_millis, legal_hold
│   │   valid_from / valid_until transaction IDs (MGA visibility)
│   │
│   └── protected_material_policy_binding (per version, per policy kind)
│
├── protected_material_policy_binding (material-level, per policy kind)
│
└── protected_material_audit_event (append-only; never deleted by purge)
```

### Catalog Entry Fields

Source: `ProtectedMaterialCatalogRecord` (catalog_records.hpp) and
`EngineProtectedMaterialCatalogEntry` (protected_material_api.hpp).

| Field | Purpose |
|-------|---------|
| `protected_material_uuid` | Stable UUID identity |
| `object_class` | Material class; defaults to `"protected_material"` |
| `owner_scope_uuid` | Owning database, schema, principal, or security scope |
| `purpose_class` | Default purpose class for release policy checks |
| `storage_class` | `"direct"`, `"wrapped"`, `"split"`, `"external_reference"`, `"derived"`, or `"redacted"` |
| `lifecycle_state` | `"active"` or `"retained_no_active_version"` |
| `active_version_uuid` | UUID of the currently active version |
| `retention_policy_uuid` | Retention policy UUID |
| `access_policy_uuid` | Metadata visibility policy UUID |
| `release_policy_uuid` | Purpose-bound release policy UUID |
| `purge_policy_uuid` | Purge/destruction policy UUID |
| `audit_policy_uuid` | Audit evidence policy UUID |
| `catalog_generation_id` | Visible catalog generation |
| `security_epoch` | Security epoch for visibility and release |

### Version Record Fields

Source: `ProtectedMaterialVersionCatalogRecord` (catalog_records.hpp).

| Field | Purpose |
|-------|---------|
| `protected_material_version_uuid` | Stable version UUID |
| `protected_material_uuid` | Parent material |
| `version_number` | Monotonically increasing per material |
| `protected_reference_hash` | Digest of the protected reference; not the reference itself |
| `protected_envelope_hash` | Digest of envelope or wrapping metadata |
| `payload_hash` | Content-integrity hash (retained after purge) |
| `storage_class` | Version-level storage class |
| `rotation_state` | `"active"`, `"rotated"`, or `"purged"` |
| `valid_from_local_transaction_id` | MGA transaction that makes the version visible |
| `valid_until_local_transaction_id` | MGA transaction that ends active visibility (0 = still active) |
| `retention_until_epoch_millis` | Earliest permitted purge time |
| `legal_hold` | If true, purge is refused regardless of retention epoch |
| `purged` | True when protected reference reachability has been removed |
| `catalog_generation_id` | Visible catalog generation |
| `security_epoch` | Security epoch |

### Policy Binding Fields

Source: `ProtectedMaterialPolicyBindingCatalogRecord` (catalog_records.hpp).

| Field | Purpose |
|-------|---------|
| `policy_uuid` | Policy object UUID |
| `policy_kind` | `"retention"`, `"access"`, `"release"`, `"purge"`, or `"audit"` |
| `diagnostic_state` | Engine-assigned diagnostic state for the binding |

Policy bindings are evaluated before every protected-material operation. A
missing required policy binding is a refusal, not permission to proceed.

### Audit Event Fields

Source: `ProtectedMaterialAuditEventCatalogRecord` (catalog_records.hpp) and
`EngineProtectedMaterialAuditEvent` (protected_material_api.hpp).

| Field | Purpose |
|-------|---------|
| `audit_event_uuid` | Stable event identity (derived by SHA-256 of event parameters) |
| `actor_uuid` | Effective principal who triggered the event |
| `event_kind` | `"create"`, `"add_version"`, `"resolve"`, `"release"`, `"purge"`, `"inspect"`, or `"support_export"` |
| `decision` | `"allow"` or `"deny"` |
| `diagnostic_code` | Diagnostic code emitted for denied events |
| `redacted_detail` | Human-readable, policy-redacted detail |
| `event_epoch_millis` | Event time |
| `local_transaction_id` | MGA transaction ID |
| `redaction_applied` | Always `true` in the engine; set at audit-append time |

Audit events are append-only. Purging a version's protected reference
reachability does not delete the audit events for that version.

## Release Policy and Purpose-Binding

Release is the operation through which a caller obtains authority to use a
protected value. It is not the same as returning plaintext; the engine returns
an opaque `release_handle` rather than the raw value.

Source: `EngineReleaseProtectedMaterial` in `protected_material_api.cpp`.

The release path:

1. Requires `PROTECTED_MATERIAL_RELEASE` right.
2. Resolves the active version visible to the caller's MGA snapshot.
3. Calls `PurposeAllowed()` to test whether the requested purpose appears in
   the version's (or material's) `release_purposes` allowlist. If the allowlist
   is empty, the purpose must match `material.purpose_class` exactly.
4. On denial, writes a `"deny"` audit event with
   `SECURITY.PROTECTED_MATERIAL.POLICY_DENIED` and returns
   `policy_denied = true` with `plaintext_material_returned = false`.
5. On admission, writes an `"allow"` audit event, then returns a
   `release_handle` (an HMAC-SHA-256 token) and sets
   `plaintext_material_returned = false`.

The release handle is a stable, purpose-bound, generation-scoped token. It
does not embed the raw protected value.

`EngineRequestProtectedMaterial` follows a similar path but operates on cache
entries by key handle rather than catalog versions. It also requires an audit
evidence event (`AppendSecurityEvidenceEvent`) to be written before the result
is returned.

## Encryption Key Admission and Cache TTL

Source: `EngineAdmitEncryptionKey`, `EngineAdmitEncryptionKeyRequest` in
`protected_material_api.hpp` and `protected_material_api.cpp`.

Encryption key admission is the process of presenting secret evidence to the
engine and receiving a key handle and fingerprint in return.

Key admission fields:

| Field | Default | Purpose |
|-------|---------|---------|
| `key_uuid` | required | UUID of the key to admit |
| `key_label` | required | Human-readable label (stored as `"redacted-key"` if missing) |
| `filespace_uuid` | required | Filespace the key belongs to |
| `secret_evidence` | required | Opaque evidence material; refused if plaintext markers detected |
| `cache_ttl_millis` | **300000** | Cache lifetime in milliseconds (5 minutes) |

The `PlaintextEvidenceRefused()` gate tests the incoming evidence against a
set of known plaintext markers including `"plaintext:"`, `"cleartext:"`,
`"password:"`, `"password="`, `"passwd="`, `"secret="`, `"private_key="`,
`"key_material="`, `"raw_key="`, and `"kms_plaintext:"`. If any match is
found, admission is refused with `SECURITY.KEY.PLAINTEXT_REFUSED`.

On successful admission, the cache entry holds:

| Field | Contents |
|-------|---------|
| `key_fingerprint` | `"fingerprint:v1:hmac-sha256:<hmac>"` over database UUID, key UUID, filespace UUID, and evidence |
| `key_handle` | `"protected-material-handle:v1:hmac-sha256:<hmac>:<generation>"` |
| `admitted_at_epoch_millis` | Admission timestamp |
| `expires_at_epoch_millis` | `admitted_at_epoch_millis + max(1, cache_ttl_millis)` |
| `active` | `true` until expired or purged |

Cache expiry is checked on every cache-touching operation via
`ExpireActiveEntriesLocked()`. Expired entries have `active = false` and
`expired = true`. They are not removed from the cache but are excluded from
active lookups.

`EngineRotateEncryptionKey` uses the same 300000 ms TTL default for the
replacement key entry.

## Version States and Rotation

Source: `EngineAddProtectedMaterialVersion` in `protected_material_api.cpp`
and `ProtectedMaterialVersionCatalogRecord.rotation_state`.

Three rotation states are valid for a version:

| State | Meaning |
|-------|---------|
| `"active"` | This is the current active version for new releases and resolution |
| `"rotated"` | A newer version has become active; this version is retained for policy and audit |
| `"purged"` | Protected reference reachability has been removed; payload_hash and audit evidence are retained |

Adding a version via `EngineAddProtectedMaterialVersion` sets the new version
to `rotation_state = "active"` and closes the previous active version by
recording `valid_until_local_transaction_id`. The material's `active_version_uuid`
pointer is updated atomically in the same persisted catalog mutation.

After purge, the engine clears `protected_reference` and `envelope_reference`
from the version record and sets `rotation_state = "purged"`. If the purged
version was the active version and no other version is available, the material's
`lifecycle_state` transitions to `"retained_no_active_version"`.

## Legal Hold and Retention

Source: `EnginePurgeProtectedMaterialVersion` in `protected_material_api.cpp`.

Purge is refused at the engine level when either of the following is true:

```
version.policy.legal_hold == true
  OR
(version.policy.retention_until_epoch_millis != 0 AND
 version.policy.retention_until_epoch_millis > now)
```

Both conditions are checked before any physical erase or reference clearing is
attempted. The refusal produces `SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED`
and writes a `"deny"` audit event with `refused_by_retention = true`.

Physical erase (zero-overwrite of the referenced file) requires four additional
flags to be set by an authorized caller:
- `physical_erase_authorized = true`
- `physical_erase_retention_satisfied = true`
- `physical_erase_legal_hold_clear = true`
- `physical_erase_path` must be non-empty

Physical erase is performed by overwriting the file with zeros in 8192-byte
chunks, flushing, and then verifying that every byte is zero before the
operation is declared complete. Missing any of the precondition flags produces
a specific diagnostic code.

## The No-Plaintext Guarantee

Every public entry point in the protected material API carries an explicit
`plaintext_material_returned = false` invariant in its result struct. The
engine enforces this in several ways:

1. **Admission gate**: `PlaintextEvidenceRefused()` and
   `ContainsPlaintextSecretMarker()` reject inputs that embed plaintext
   markers. Admission returns only a key handle and fingerprint.

2. **Storage gate**: `ProtectedPayloadInputRefused()` rejects create and
   version-add requests whose `protected_reference`, `envelope_reference`, or
   `payload_hash` embed known plaintext markers.

3. **Result gate**: All result structs set `plaintext_material_returned = false`
   and `protected_material_redacted = true`. The engine never populates a raw
   plaintext field in a public result.

4. **Redact-on-read**: `RedactedVersion()` replaces `protected_reference` and
   `envelope_reference` with `"<protected-material-redacted>"` before any
   version is placed in a public result or diagnostic row.

5. **Diagnostic gate**: `RedactProtectedMaterialForDiagnostics()` scans any
   string passed to a diagnostic or result row and replaces it with
   `"<protected-material-redacted>"` if it contains a known protected-material
   marker (`"secret"`, `"password"`, `"credential"`, `"private_key"`,
   `"key_material"`, `"plaintext"`, `"cleartext"`, `"encryption_key"`,
   `"decryption_key"`, `"protected_material"`, `"bearer "`, `"token="`,
   `"apikey"`, `"api_key"`, `"kms_plaintext"`).

6. **Late payload gate**: `FetchLateMaterializationPayload()` in
   `late_payload_fetch.cpp` refuses to expose payload bytes for any reference
   where `reference.redaction_required` is true or where
   `reference.protected_payload` is true and
   `reference.unredacted_payload_authorized_by_security` is false. The
   diagnostic is `SB_LATE_PAYLOAD_FETCH.UNREDACTED_PROTECTED_PAYLOAD` and
   `result.fail_closed = true`.

The diagnostic code `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` is also
documented in `security_model_overview.md` as a global invariant:
`SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` fires when a call path
attempted to return plaintext protected material.

## Visibility: Intersection of MGA and Policy

Protected material visibility follows the same rule as all catalog objects in
ScratchBird:

> Visibility = intersection of MGA visibility and materialized policy.

MGA visibility is determined by the transaction snapshot. A version that was
created in a transaction whose `valid_from_local_transaction_id` is beyond the
reader's snapshot is not visible. Similarly, a version whose
`valid_until_local_transaction_id` is less than or equal to the snapshot is no
longer visible.

Policy visibility is enforced by the policy binding chain. A principal who has
not been granted access through the material's `access_policy_uuid` cannot see
metadata even if the MGA snapshot would include it.

`ResolveActiveVersionLocked()` implements this by combining:
- `ReadVisibilityPoint(context)` — the observer's snapshot boundary
- `VersionVisibleAt(version, visibility_point)` — tests `purged`,
  `valid_from`, and `valid_until`

If no version passes both tests, resolution fails with
`SECURITY.PROTECTED_MATERIAL.VERSION_NOT_VISIBLE`.

## Redaction in SELECT / SHOW / DESCRIBE

The engine does not have a dedicated SELECT path for protected material in the
manner of an ordinary table. The protected-material catalog tables
(`sys.security.protected_material_catalog`, `sys.security.protected_material_version`,
etc.) are engine-authority-only records (`engine_authority = true` in their
`CatalogRecordDescriptor`) and `parser_visible = false`. Ordinary SBsql `SELECT`
statements targeting these tables go through the full deep enforcement chain.

The engine's result row helpers always call `RedactProtectedMaterialForDiagnostics()`
on any value before it enters a result row (via `AddProtectedMaterialRow()`),
and always replace `protected_reference` and `envelope_reference` with
`"<protected-material-redacted>"` via `RedactedVersion()`.

The catalog inspect function (`EngineInspectProtectedMaterialCatalog`) calls
`RedactedVersion()` on every version before adding it to the result, so even
authorized callers who hold `PROTECTED_MATERIAL_RELEASE` see only
`"<protected-material-redacted>"` in place of the live protected reference.

## Redaction in Diagnostics and Support Bundles

Two separate redaction functions operate on diagnostic text and support-bundle
content:

### Engine Diagnostic Redaction

`RedactProtectedMaterialForDiagnostics(std::string text)` (in
`protected_material_api.cpp`) is called at every point where string data from
a protected-material operation enters a diagnostic row or result. It checks
`ContainsProtectedMaterialMarker()` and replaces the entire string with
`"<protected-material-redacted>"` if any marker is found.

### Manager Support-Bundle Redaction

`RedactManagerSupportBundleText(std::string text)` (in
`manager_support_bundle.cpp`) scans lines written to support-bundle files and
replaces values following sensitive keys with `"[redacted]"`. The sensitive
keys are:

| Key |
|-----|
| `password` |
| `passwd` |
| `secret` |
| `token` |
| `private_key` |
| `credential` |
| `verifier` |
| `encryption_key` |
| `decryption_key` |
| `key_handle` |

The same function also replaces filesystem paths (any token starting with `/`)
with `"[path-redacted]"`.

The manager support bundle manifest explicitly records:

```
excluded_protected_material=password,secret,token,private_key,credential,verifier,encryption_key,decryption_key,key_handle
```

### Listener Support-Bundle Redaction

`RedactListenerSupportabilityText(std::string_view text)` (in
`listener_support_bundle.cpp`) checks for the same sensitive key set plus
`"auth"`. If any match is found, the entire value is replaced with
`"[redacted:security]"`. Local filesystem paths are replaced with
`"[path-redacted]"`. The listener support bundle JSON explicitly declares:

```json
"excluded_protected_material": ["password","secret","token","private_key",
  "credential","verifier","encryption_key","decryption_key","key_handle"]
```

Both support-bundle generators also record `"redaction_profile"` and
`"local_path_policy": "redacted"` to document that redaction was applied.

## Audit Events

Every protected-material operation that reaches its conclusion appends an audit
event via `AppendMaterialAuditLocked()`. Denied operations also write audit
events before returning the failure result.

The audit event UUID is a stable, SHA-256-derived identifier:

```
"protected-material-audit:v1:sha256:<sha256-hex>"
```

where the hash input includes: `database_uuid | protected_material_uuid |
protected_material_version_uuid | event_kind | decision | event_epoch_millis |
sequence_number`.

The `redacted_detail` field of every audit event is processed through
`RedactProtectedMaterialForDiagnostics()` before the event is appended. The
`redaction_applied` flag is always `true` in the engine.

The current engine emits audit events for these operations:

| Operation | Event Kind | Decision |
|-----------|-----------|---------|
| Create protected material | `"create"` | `"allow"` |
| Add version (rotation) | `"add_version"` | `"allow"` |
| Resolve (reference lookup) | `"resolve"` | `"allow"` or `"deny"` |
| Release (purpose-bound) | `"release"` | `"allow"` or `"deny"` |
| Purge version | `"purge"` | `"allow"` or `"deny"` |
| Inspect catalog | `"inspect"` | `"allow"` |

Audit events are never deleted by version purge. They survive the lifecycle of
the protected material they describe, subject only to the audit retention
policy.

## Required Rights

| Operation | Required Right |
|-----------|---------------|
| Admit or rotate an encryption key | `KEY_RELEASE_APPROVE` |
| Resolve, release, or inspect catalog | `PROTECTED_MATERIAL_RELEASE` |
| Create protected material or add version | `KEY_RELEASE_APPROVE` |
| Purge a version | `KEY_RELEASE_APPROVE` |
| Inspect the in-memory cache | `PROTECTED_MATERIAL_RELEASE` |
| Purge the in-memory cache | `KEY_RELEASE_APPROVE` |
| Shutdown purge (engine shutdown) | `KEY_RELEASE_APPROVE` or shutdown authority tag |
| Export/import a package | `PROTECTED_MATERIAL_RELEASE` (export), `KEY_RELEASE_APPROVE` (import) |

All operations also require passing `ValidateEngineAuthorityBoundary()`, which
refuses any request that presents a non-engine authority prefix
(`auth_authority:`, `key_authority:`, etc.) or that carries a trace tag
implying parser, driver, reference, or SQLite authority.

## Package Export and Import

The engine supports exporting and importing a protected material package in a
binary format with the identifier
`"scratchbird.protected_material.reference_package.v1"`. The package contains
catalog entries, version records, and optionally audit events, all encoded in
the same record-based binary format used for the on-disk protected material
catalog file (`.sb.protected_material_catalog`).

Export does not include plaintext values: `plaintext_material_returned = false`
and `protected_material_redacted = true` are set on the export result. The
package digest is a `"sha256:<hex>"` string that callers can verify at import
time.

Import requires `import_authorized = true` and validates every version record
against `ProtectedPayloadInputRefused()` to ensure no plaintext leaks in.

## Diagnostic Codes

| Code | Meaning |
|------|---------|
| `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` | A call path attempted to return or store plaintext protected material |
| `SECURITY.KEY.PLAINTEXT_REFUSED` | Encryption key admission or rotation refused plaintext evidence |
| `SECURITY.KEY.UNAVAILABLE` | No active cache entry for the requested key UUID or handle |
| `SECURITY.KEY.EXPIRED` | A cache entry for the key exists but has expired |
| `SECURITY.KEY.WRONG` | The key handle references the wrong key |
| `SECURITY.KEY.SCOPE_MISMATCH` | Key handle is not valid for the requested database or filespace |
| `SECURITY.KEY.ADMISSION_INVALID` | Admission request missing required fields |
| `SECURITY.KEY.ROTATION_INVALID` | Rotation request missing required fields |
| `SECURITY.PROTECTED_MATERIAL.AUTHORITY_BYPASS_REFUSED` | A non-engine authority prefix was detected |
| `SECURITY.PROTECTED_MATERIAL.AUTHORITY_DENIED` | Caller lacks the required right for the operation |
| `SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID` | Required catalog fields absent or malformed |
| `SECURITY.PROTECTED_MATERIAL.MGA_CONTEXT_REQUIRED` | Operation requires an active MGA transaction |
| `SECURITY.PROTECTED_MATERIAL.NOT_FOUND` | Protected material UUID not found in catalog |
| `SECURITY.PROTECTED_MATERIAL.VERSION_NOT_VISIBLE` | No version visible under the current MGA snapshot and policy |
| `SECURITY.PROTECTED_MATERIAL.VERSION_NOT_FOUND` | Specific version UUID not found |
| `SECURITY.PROTECTED_MATERIAL.VERSION_DUPLICATE` | A version with this UUID already exists |
| `SECURITY.PROTECTED_MATERIAL.VERSION_INVALID` | Version request missing required fields |
| `SECURITY.PROTECTED_MATERIAL.ALREADY_EXISTS` | Protected material UUID conflicts with existing entry |
| `SECURITY.PROTECTED_MATERIAL.POLICY_DENIED` | Release refused because the requested purpose is not in the allowlist |
| `SECURITY.PROTECTED_MATERIAL.DENIED` | General protected-material denial (e.g., purpose not provided) |
| `SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED` | Purge refused by legal hold or retention epoch |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_AUTHORITY_REQUIRED` | Physical erase requires engine authority flag |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_RETENTION_PROOF_REQUIRED` | Physical erase requires retention satisfied flag |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_LEGAL_HOLD_PROOF_REQUIRED` | Physical erase requires legal hold cleared flag |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_PATH_REQUIRED` | Physical erase path not provided |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_FAILED` | Physical erase write or verify step failed |
| `SECURITY.PROTECTED_MATERIAL.DURABLE_CATALOG_INVALID` | On-disk catalog file is corrupt or mismatched |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_INVALID` | Export/import package is malformed or missing fields |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_DIGEST_MISMATCH` | Import package digest does not match expected value |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_UUID_CONFLICT` | Import package contains a UUID already present in the catalog |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_IMPORT_AUTHORITY_REQUIRED` | Import requires `import_authorized = true` |
| `SECURITY.FILESPACE.OPEN_INVALID` | Encrypted filespace open request missing required fields |
| `SB_LATE_PAYLOAD_FETCH.UNREDACTED_PROTECTED_PAYLOAD` | Late-fetch gate refused to expose protected payload bytes |
| `SB_LATE_PAYLOAD_FETCH.REDACTION_GATE_REQUIRED` | Security snapshot and redaction policy must be bound before late fetch |

## Invariants

- **Fail-closed**: Any missing, stale, ambiguous, or structurally invalid
  protected-material state refuses the operation rather than defaulting to
  allow. Missing policy bindings, missing active versions, and stale security
  epochs all produce refusals.

- **Plaintext never returned**: No public API entry point returns or stores a
  plaintext secret. Every result struct carries `plaintext_material_returned = false`
  as a stated invariant. The admission, storage, diagnostic, and late-payload
  gates enforce this independently.

- **Visibility = intersection of MGA visibility and materialized policy**: A
  version that is not visible to the caller's transaction snapshot cannot be
  resolved or released even if the caller holds the required rights. A version
  that is policy-hidden is similarly unreachable even when MGA-visible.

- **Purpose-bound release**: Release is refused unless the declared purpose
  appears in the version's or material's `release_purposes` allowlist, or
  matches the material's `purpose_class` when no explicit allowlist is set.
  Denied releases produce audit events before returning.

- **Legal hold blocks purge unconditionally**: If `legal_hold = true` on a
  version, purge is refused regardless of the retention epoch value. Both
  conditions must be clear before a physical erase can be requested.

- **Audit events are retained through version purge**: Purging a version's
  protected reference clears `protected_reference` and `envelope_reference`
  but does not delete audit events. Audit evidence survives the material it
  describes, subject to audit retention policy.

- **Support bundles are redacted**: Both the manager and listener support-bundle
  generators apply independent redaction functions before writing any line
  containing a sensitive key. The redaction coverage includes key handles,
  credentials, tokens, and private keys.

## Related Pages

- [security_model_overview.md](security_model_overview.md) — three-layer model,
  fail-closed invariant, and the `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED`
  diagnostic code
- [domain_and_column_security.md](domain_and_column_security.md) — column-level
  grants, masking, and the `hidden_as_missing` decision used when protected
  catalog objects are not visible
- [grants_and_privileges.md](grants_and_privileges.md) — how `KEY_RELEASE_APPROVE`
  and `PROTECTED_MATERIAL_RELEASE` rights are granted and revoked
- [Language Reference: sys.security.protected_material_catalog](../Language_Reference/catalog_reference/sys_security_protected_material_catalog.md)
- [Language Reference: sys.security.protected_material_version](../Language_Reference/catalog_reference/sys_security_protected_material_version.md)
- [Language Reference: sys.security.protected_material_policy_binding](../Language_Reference/catalog_reference/sys_security_protected_material_policy_binding.md)
- [Language Reference: sys.security.protected_material_audit](../Language_Reference/catalog_reference/sys_security_protected_material_audit.md)
- [Operations and Administration: Identity, Security, and Policy](../Operations_Administration/identity_security_and_policy.md)
- [Operations and Administration: Diagnostics, Message Vectors, and Support Bundles](../Operations_Administration/diagnostics_message_vectors_and_support_bundles.md)
