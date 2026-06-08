# Backup, Restore, and PITR Protected Material Policy

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_BACKUP_RESTORE_PITR_PROTECTED_MATERIAL_POLICY`

Protected material references interact with backup/restore/PITR through MGA
archive evidence only. No external provider log, WAL, Kubernetes status, or CDN
stream is recovery authority.

Required behavior:

- Backup captures protected material identity/version metadata, policy links,
  redacted provider references, and audit lineage.
- Backup does not capture plaintext secret material.
- Restore validates that referenced KMS/provider profiles are present or enters
  restricted-open with deterministic diagnostics.
- PITR restores material/version visibility to the selected MGA point.
- Purged versions remain purged after restore unless policy explicitly restores
  reachability from a retained archive.
- Key-reference rotation after restore creates a new protected material version.
- All restore/rekey/purge decisions are audited.
