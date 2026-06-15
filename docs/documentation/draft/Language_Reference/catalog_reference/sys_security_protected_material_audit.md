# sys.security.protected_material_audit Catalog Reference

This page documents the authorized catalog surface that records redacted audit
events for protected-material lifecycle, access, release, denial, purge,
policy, and inspection activity.

Generation task: `catalog_sys_security_protected_material_audit`

Related pages: [sys.security.protected_material_catalog](sys_security_protected_material_catalog.md),
[sys.security.protected_material_version](sys_security_protected_material_version.md),
[sys.security.protected_material_policy_binding](sys_security_protected_material_policy_binding.md),
[Security And Sandboxing](../core_paradigms/security_and_sandboxing.md), and
[Refusal Vectors](../syntax_reference/refusal_vectors.md).

## Role

`sys.security.protected_material_audit` is the durable, redacted
evidence surface for protected-material decisions. It lets authorized security,
support, and operations users answer questions such as:

- who attempted to inspect protected material metadata;
- whether a release was allowed or denied;
- which policy kind controlled the decision;
- which protected material and version were involved;
- which diagnostic was returned;
- whether redaction was applied;
- which transaction or catalog generation the event belongs to.

Audit events are evidence. They do not expose raw protected material and do not
grant release authority.

## Keys And Columns

Primary key: `audit_event_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `audit_event_uuid` | UUID/text | Stable audit event identity. |
| `protected_material_uuid` | UUID | Material involved in the event. |
| `protected_material_version_uuid` | nullable UUID | Version involved, if the event is version-specific. |
| `actor_uuid` | UUID | Effective user, role, agent, or system actor. Redacted in projections where policy requires it. |
| `event_kind` | enum | `create`, `add_version`, `rotate`, `resolve`, `release`, `deny`, `purge`, `policy_change`, `inspect`, `quarantine`, or `support_export`. |
| `decision` | enum | `allow`, `deny`, `redact`, `quarantine`, or `not_applicable`. |
| `diagnostic_code` | nullable text | Message-vector code emitted for denial, refusal, quarantine, or warning. |
| `redacted_detail` | text | Human-readable, policy-redacted event detail. |
| `event_epoch_millis` | uint64 | Event time from engine audit context. |
| `local_transaction_id` | uint64 | Local MGA transaction ID associated with the event, or zero when no user transaction applies. |
| `catalog_generation_id` | uint64 | Catalog generation associated with the event. |
| `redaction_applied` | boolean | True when protected-material redaction policy was applied to event details. |

## Event Kinds

| Event Kind | Meaning |
| --- | --- |
| `create` | Protected material identity was created. |
| `add_version` | A new protected material version was added. |
| `rotate` | Active protected material version changed. |
| `resolve` | A protected reference was resolved without releasing raw material. |
| `release` | Raw material or release-controlled value was admitted for a purpose. |
| `deny` | Access, resolution, release, purge, export, or support collection was denied. |
| `purge` | Protected reference reachability was removed under purge policy. |
| `policy_change` | A policy binding or policy epoch changed. |
| `inspect` | Metadata was inspected through an authorized projection. |
| `quarantine` | Material or version was fenced because integrity or policy state was uncertain. |
| `support_export` | Redacted evidence was included in support output. |

## Redaction Rules

Audit rows are sensitive even when they contain no raw protected value.

Redaction can apply to:

- actor UUID;
- material UUID;
- version UUID;
- policy names or UUIDs;
- endpoint, bridge, stream, backup, replication, migration, or support context;
- diagnostic detail;
- hashes or reference metadata;
- timing information where policy requires it.

`redaction_applied` must be true for rows where protected-material redaction
policy affected rendering. A false value is allowed only when policy confirms
that the rendered audit row contains no protected detail for the caller.

## Visibility And Mutation

Audit rows are append-only evidence from the public user's point of view.
Engine-managed security and protected-material operations create them.
Ordinary catalog queries, support export, diagnostics rendering, and parser
metadata requests must not directly mutate the base audit table.

Retention and purge of audit rows are governed by audit and retention policy.
Purging protected reference reachability must not remove audit rows that policy
requires to remain.

## Example Inspection

```sql
select audit_event_uuid,
       protected_material_uuid,
       event_kind,
       decision,
       diagnostic_code,
       event_epoch_millis,
       redaction_applied
from sys.security.protected_material_audit
where protected_material_uuid = :protected_material_uuid
order by event_epoch_millis;
```

Returned rows and columns depend on the caller's disclosure policy.

## Support-Bundle Behavior

Support bundles may include protected-material audit evidence only through
redacted projections. A support bundle must not include raw secrets, raw
protected payloads, credential text, unredacted protected references, or
unredacted release evidence unless a specific release policy admits that
content for that support purpose.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Audit row hidden by policy | Redact or omit row according to disclosure policy. |
| Actor hidden | Redact `actor_uuid` or render a policy-safe actor class. |
| Material hidden | Redact material identity while preserving authorized event class. |
| Diagnostic detail protected | Render `diagnostic_code` and redacted summary only. |
| Audit append fails for required event | Fail closed or quarantine according to audit policy. |
| Support export requests raw audit detail | Deny or redact. |
| Retention policy blocks deletion | Refuse deletion or purge. |

## Verification Checklist

Proof should demonstrate:

- protected-material lifecycle operations emit audit events where policy
  requires them;
- release denials and release approvals are distinguishable without leaking raw
  material;
- unauthorized users cannot infer hidden protected material through audit
  queries;
- support bundles include only redacted audit evidence;
- purging protected reference reachability preserves required audit rows;
- redaction policy controls actor, material, version, diagnostic, and endpoint
  fields;
- audit append failure does not silently allow an operation that requires audit
  evidence;
- audit rows are transactionally and catalog-generation consistent.
