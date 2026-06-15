# Upgrade And Compatibility Policy

## Purpose

Before you move a ScratchBird deployment to a newer build, you need to know which on-disk structures might change, which downgrade paths are refused, and what evidence the new build requires before it will open an existing database. This chapter covers all of those topics so that an upgrade does not catch you by surprise.

The short version: ScratchBird refuses to open databases with a format version it does not understand. It also refuses to downgrade IPC protocol, configuration, or parser API versions. Upgrades that advance format versions may be irreversible from the older build's perspective. Plan accordingly.

---

## Format Version Surfaces

ScratchBird has several independently versioned on-disk and in-memory format surfaces. A change to any one of them may affect whether an older or newer build can open the relevant artifact.

| Surface | Version fields | Where checked |
|---|---|---|
| Database file format (page header) | `format_major` (currently `kScratchBirdDatabaseFormatMajor = 1`) | `src/storage/disk/database_format.hpp:26` |
| Startup state block | `format_major` / `format_minor` (current: `1.0`; supported range: `1.0`–`1.0`) | `src/storage/database/startup_state.hpp:33-38` |
| Catalog manifest format | `catalog_manifest_format_version` (currently `kDatabaseCatalogManifestFormatCurrent = 1`) | `src/storage/database/database_lifecycle.cpp:206` |
| Parser API major | `kCurrentParserApiMajor = 1`; compatible range `1`–`1` | `src/listener/control_plane.hpp:46-48` |
| SBLR surface | Tested by `tests/sblr_surface` and the `sblr_surface_guardrail_gate` | Build-time gate |

### How Version Refusals Work

When a database is opened, the engine reads the page header and the startup state block. If either format version is outside the supported range for this build:

- Format version **too new** (database written by a newer build): `FORMAT.VERSION_DOWNGRADE_REFUSED` or `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED`. The database will not be opened.
- Format version **too old** (database written by an older build, and no migration plan is present): `storage.startup_state.format_too_old` or `storage.startup_state.migration_required_without_plan`. The database will not be opened without a migration plan.
- Format version **in the future** (major/minor combination not yet reached): `storage.startup_state.format_future`. The database will not be opened.

This means that once a database has been opened and written by a newer build, you cannot safely open it with an older build. The older build will refuse.

Sources: `src/storage/disk/database_format.cpp:175-195`, `src/storage/database/startup_state.cpp:343-390`.

---

## The Downgrade Refusal Policy

ScratchBird applies a broad downgrade-refusal policy across multiple surfaces:

| Surface | Diagnostic code | Trigger |
|---|---|---|
| Database page format | `FORMAT.VERSION_DOWNGRADE_REFUSED` | Database format major is newer than this build's maximum |
| Engine database lifecycle | `ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED` | Same condition, observed at the lifecycle layer |
| Startup state | `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED` | Startup state format major/minor is newer than this build's maximum supported |
| IPC protocol | `IPC.LIFECYCLE.DOWNGRADE_REFUSED` | Client is using a newer IPC protocol version |
| Configuration | `CONFIG.DOWNGRADE_REFUSED` | Configuration value is not compatible with this build |
| Parser server IPC | `PARSER_SERVER_IPC.PROTOCOL_VERSION_DOWNGRADE_REFUSED` | Client is using a newer parser IPC protocol version |
| Memory policy | `MEMORY.POLICY_DOWNGRADE_REFUSED` | Memory policy cannot be downgraded |
| TLS session | `SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED` | TLS version downgrade attempt |

The consistent principle: ScratchBird will not silently accept an incompatible artifact. Every incompatibility produces a specific diagnostic code so the operator knows exactly what is mismatched.

---

## Upgrade Path

### Pre-Upgrade Checklist

Before upgrading:

1. **Identify all databases and their current format versions.** If you cannot determine the format version, open the database with the current build and observe the catalog evidence.
2. **Verify the new build passes its release gate tests.** See [Release Validation Checklist](release_validation_checklist.md).
3. **Back up all databases** using `BACKUP DATABASE TO <uri>`. A logical backup taken with the current build is readable by the new build regardless of whether the format version advances.
4. **Confirm the backup is restorable** with a restore drill on a staging system.
5. **Check whether the new build introduces a format version change.** Review the release notes or the `kScratchBirdDatabaseFormatMajor` constant in the new build's source.

### Upgrade Sequence

1. Drain active listeners with `listener.drain` to allow in-flight sessions to complete.
2. Wait for all sessions to close (observe `open_connections` reaching zero in the status snapshot).
3. Stop the listener and manager.
4. Replace binaries.
5. Start the new manager and listener.
6. Confirm the new build opens the database successfully (verify with `STATUS` and a smoke query).
7. Undrain with `listener.undrain` once confirmed healthy.

### After Upgrade

Once the new build has opened and written to a database, the database's format version may have advanced. Verify that:
- The old build is no longer needed for this database.
- Any disaster-recovery procedures are updated to reference the new build version.

---

## Catalog Manifest Compatibility

The catalog manifest (`catalog_manifest_format_version`) records the version of the catalog structure inside the database. The current value is `1`. The manifest is read during database open to verify the catalog is interpretable. If the manifest version is unknown, the open is refused.

---

## Parser Package Compatibility

Each parser package carries a `parser_package_version`. The engine uses this to confirm that a parser package is compatible with the engine ABI and the database it is being asked to serve. Parser package version mismatches appear as diagnostics during parser registration; see [Parser Registration And Routes](parser_registration_and_routes.md).

The parser API major version (`kCurrentParserApiMajor = 1`) is negotiated during the listener hello handshake. If a parser worker presents a major version outside the supported range, the listener refuses the hello with `HelloAckPayload::accepted = false`.

---

## SBLR Surface Compatibility

The SBLR (ScratchBird Logical Representation) surface is the bytecode/envelope format that carries query plans from parser to engine. SBLR surface compatibility is tested by the `tests/sblr_surface` test tree and is guarded by the `sblr_surface_guardrail_gate`. A build that fails these gates should not be shipped.

From an operator's perspective: if a parser package was compiled against an older SBLR surface, it may produce envelopes the engine cannot execute. The engine will refuse execution with a diagnostic rather than attempting to interpret an unknown SBLR version.

---

## Configuration Compatibility

Configuration files are validated at startup. A configuration value that was valid in an older build may be refused if:
- The key was removed or renamed.
- The value type changed.
- A range constraint was tightened.

The `CONFIG.DOWNGRADE_REFUSED` code signals that a configuration value is not acceptable to the new build. Review the [Configuration Reference](configuration_reference.md) for the new build before upgrading.

---

## Migration Notes for Firebird Operators

Operators migrating from Firebird should note:

- `GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, and `FBTRACEMGR` are not executed by the SBsql parser. Use ScratchBird logical backup/restore and management routes instead.
- `NBACKUP` (physical page-level incremental backup) is not supported. Use `BACKUP DATABASE TO <uri>` with logical streams.
- Shadow file management syntax is recognized but has no filesystem side effect in SBsql; it routes through `diagnostic.lifecycle.message_vector` with code `SBSQL.EMULATION.NON_FILE_OPERATION`.

---

## Related Pages

- [Database Lifecycle](database_lifecycle.md)
- [Release Validation Checklist](release_validation_checklist.md)
- [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md)
- [Troubleshooting](troubleshooting.md)
- [Language Reference](../Language_Reference/README.md)
