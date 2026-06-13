# ScratchBird DBeaver Management Manual QA Checklist

Search key: `DBEAVER-MGMT-035-MANUAL-QA-CHECKLIST`

Use this checklist with a stock DBeaver CE install plus the ScratchBird update
site built from the current checkout.

## Required Evidence

- DBeaver version, Java version, OS, architecture, plugin build id, and update-site hash.
- ScratchBird server commit, connection profile name, principal, role, and auth method.
- Screenshot packet path and GUI log path.
- Result for each item: `passed`, `refused-as-designed`, or `failed`.

## Checklist

- Install the ScratchBird DBeaver plugin into a clean stock DBeaver profile.
- Open the connection wizard and create a ScratchBird connection without exposing secrets in screenshots or logs.
- Browse the navigator tree and confirm ScratchBird context actions appear only for supported ScratchBird objects.
- Open SQL editor validation and completion; capture diagnostics for accepted SQL and refused/unsupported SQL.
- Open a management dialog and capture Overview, Workflow, Authz, Live, Monitoring, Graph, History, Validation, and Reports tabs.
- Run Validate Preview and confirm the Workflow tab keeps parser validation separate from server authorization.
- Run Refresh Server Status and confirm authz/live refusal or admission is visible and recorded in History.
- Confirm Apply Requires Admission remains disabled until server admission for
  the exact preview hash and command hash is visible.
- When a server-admitted mutation is available, run Apply Admitted Preview and
  confirm the post-apply refresh comes from server truth.
- Confirm no preview-only path or screenshot claims an operation was applied
  without server admission and post-apply refresh evidence.
- Open the Data Editor tab and confirm insert, update, delete, refresh, transaction, type, and refusal contracts are visible.
- Open the Data Transfer tab and confirm import/export, encoding, batching, cancellation, and result-parity contracts are visible.
- Open the Graph tab and confirm dependency, search, generated DDL, generated SBsql, explain, invalidation, and authorization-filtered visibility contracts are visible.
- Confirm long-running refresh/probe operations run through DBeaver progress UI, cancellation remains non-applied, and partial failures remain auditable.
- Open dashboards and confirm stale, missing, unsupported, or denied server surfaces display as refusals.
- Check keyboard traversal through dialog buttons, task selector, history selector, and tab order.
- Check accessible labels/tooltips for dialog fields, task controls, history controls, and footer buttons.
- Switch locale/profile where available and confirm labels remain readable with canonical English fallback.
- Exercise install removal and confirm the ScratchBird plugin, cached driver entry, and profile secrets are removed or refused with evidence.
