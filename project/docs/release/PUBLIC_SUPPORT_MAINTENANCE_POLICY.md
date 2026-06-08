# Public Support And Maintenance Policy

PUBLIC_SUPPORT_MAINTENANCE_POLICY

Authority: public_release_evidence_only.

This policy defines support and maintenance commitments for the public
engine/listener release surface. It does not define storage authority,
transaction finality, authorization, recovery classification, optimizer
authority, parser authority, or cluster-positive execution authority.

## SUPPORT_LIFECYCLE_POLICY

Support lifecycle status is published per release line. Public Core Beta is
supported for the Linux proof lane in this repository state. Windows x64 and
FreeBSD remain target platform lanes until platform proof is complete.
Unsupported platforms or unsupported feature families must fail closed or be
reported as unsupported before operation.

Windows x64 and FreeBSD remain target platform lanes until platform proof is complete.

Evidence anchors: `PUBLIC_SUPPORT_RELEASE_LIFECYCLE`,
`public_platform_matrix_gate`, `public_release_attestation_gate`.

## SECURITY_UPDATE_PROCESS

Security updates are accepted only when the issue is reproducible against
public release artifacts or a supported enterprise package. Each security fix
must include public or redacted project-tests proof for the affected
engine/listener surface. Support evidence cannot replace authorization,
protected-material, or MGA transaction authority.

Evidence anchors: `public_enterprise_threat_gate`,
`public_security_provider_contract_protected_material_gate`,
`engine_listener_adversarial_security_validation_gate`.

## CVE_HANDLING_PROCESS

CVE handling requires intake, severity classification, affected-version
inventory, reproduction or fail-closed proof, fix ownership, regression test,
release note, and patch artifact tracking. A CVE row is not closed until the
fix has regenerated public proof or an explicit unsupported diagnostic proves
safe refusal.

CVE handling requires affected-version inventory, fix ownership, and patch artifact tracking.

Evidence anchors: `public_dependency_sbom_gate`,
`public_artifact_signature_gate`, `public_release_attestation_gate`.

## DISCLOSURE_POLICY

Disclosure policy requires coordinated disclosure for exploitable security
defects, private diagnostic handling for protected material, redacted public
advisory text, and public release notes after a patch is available. Disclosure
material must not include secrets, private paths, protected material, or raw
support-bundle payloads.

Disclosure uses redacted public advisory text after a patch is available.

Evidence anchors: `public_audit_privacy_gate`,
`public_crypto_entropy_policy_gate`, `engine_listener_support_bundle_redaction_gate`.

## PATCH_RELEASE_PROCESS

Patch releases require deterministic version metadata, reproducible public
artifacts, checksum/signature-ready evidence, SBOM/license/vulnerability scan
evidence, release notes, upgrade notes, migration notes, and no new unsupported
runtime claims. Patch releases must not silently alter durable format policy.

Evidence anchors: `public_release_version_metadata_gate`,
`public_artifact_reproducibility_gate`, `public_artifact_signature_gate`.

## COMPATIBILITY_POLICY

Compatibility policy follows the public API/ABI compatibility policy and the
engine/listener durable format compatibility gates. Unsupported old formats,
unknown newer formats, downgrade requests, parser contract mismatches, and
plan-cache compatibility mismatches fail closed with stable diagnostics.

Evidence anchors: `PUBLIC_API_COMPATIBILITY_POLICY`,
`public_api_abi_compat_gate`, `engine_listener_compatibility_upgrade_downgrade_gate`.

## DATA_LOSS_ESCALATION_POLICY

Potential data-loss issues are escalated immediately when evidence suggests
silent inconsistency, transaction visibility drift, index divergence, TOAST
reachability loss, recovery uncertainty, backup/restore identity drift, or
support-bundle evidence mismatch. The first response is to fence writes or
require recovery/repair inspection rather than to guess state.

Evidence anchors: `engine_listener_crash_recovery_certification_gate`,
`public_disaster_recovery_gate`, `public_backup_update_coverage_gate`.

## RELEASE_ROLLBACK_POLICY

Release rollback policy requires signed/checksum-addressed artifacts, a
documented downgrade-refusal boundary, migration notes, backup/restore
interaction guidance, and explicit refusal when rollback would imply an
unsupported durable-format downgrade. Rollback instructions are operator
process evidence and do not alter engine recovery authority.

Evidence anchors: `public_upgrade_migration_gate`,
`public_disaster_recovery_gate`, `public_admin_runbook_gate`.

## DIAGNOSTIC_COLLECTION_POLICY

Diagnostic collection uses redacted support bundles, stable diagnostic codes,
audit/privacy retention controls, support-bundle incident matrices, and
operator runbooks. Diagnostic evidence is evidence-only and must not include
protected material, secrets, or private local paths.

Evidence anchors: `public_support_bundle_incident_gate`,
`public_diagnostic_stability_gate`, `public_audit_privacy_gate`.

## ENTERPRISE_SLA_BOUNDARIES

Enterprise SLA boundaries are scoped to supported release lines, supported
platform lanes, public or contracted package artifacts, and defects that can be
reproduced or diagnosed from approved evidence. SLA commitments do not cover
unsupported platforms, unsupported feature families, private cluster
implementation claims, untrusted donor behavior, or parser-side finality.

Evidence anchors: `PUBLIC_SUPPORT_RELEASE_LIFECYCLE`,
`public_platform_matrix_gate`, `public_unsupported_feature_gate`.

## ALPHA_BETA_GA_SUPPORT_BOUNDARIES

Alpha and developer-preview builds are not enterprise support promises. Public
Core Beta can be used for release-candidate validation where gates pass. GA
support requires final gold aggregation, platform proof, soak proof, external
review closure, signed artifacts, support policy approval, and explicit known
limitations.

GA support requires final gold aggregation, platform proof, soak proof, external review closure, signed artifacts, support policy approval, and explicit known limitations.

Evidence anchors: `engine_listener_enterprise_documentation_gate`,
`engine_listener_release_artifact_trust_gate`, `engine_listener_enterprise_foundation_check`.

## COMMUNITY_COMMERCIAL_SUPPORT_BOUNDARY

Community support covers public documentation, public diagnostics, public issue
triage, and reproducible public proof. Commercial support may add private
support channels, contracted SLAs, provider-specific package support, and
enterprise incident response. Commercial support cannot turn an unsupported
feature into runtime authority without implementation and regenerated proof.

Evidence anchors: `public_support_policy_gate`,
`public_enterprise_documentation_gate`, `public_support_bundle_incident_gate`.
