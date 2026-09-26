# Preserved UUID repair source queue

This directory preserves prepared ScratchBird source changes during a worktree
transfer. **The patches are data, not applied implementation or passing-test
evidence.** They are not build inputs. Do not apply every patch indiscriminately.

The source checkpoint is recorded in `manifest.json`. It includes the committed
UUID repair branch and the subsequent durability/planner changes. The last
recorded full suite had 3,834 tests: 3,071 passed and 763 failed. A selected
socket/workspace rerun had 283 passed and 58 failed out of 341. The later all-target
build failed in `optimizer_deficiency_odf_074_gate.cpp`, whose vector corpus still
initializes binary row identities with text. No new build or test success is
claimed by this archival commit.

## Contents and integrity

The manifest records 90 original/prepared source pairs in 39 groups. Each entry
contains its actual repository target, the original/prepared/checkpoint SHA-256
digests, and a comparison with the checkpoint. The directory retains older drafts
as well as queued changes so unique work is not silently discarded.

- `prepared_equals_checkpoint`: this exact prepared file is already present.
- `original_equals_checkpoint`: the checkpoint matches the prepared patch's base.
- `diverged_from_checkpoint`: reconcile the patch with later edits first; this
  includes intentionally sequential repairs and superseded draft versions.

`historical_queue_claims` maps groups to the old queue; it does not mark any claim
implemented. A group can include an unqueued draft as well as its queued file.
The patches have zero context. Check the original hashes before using
`git apply --check --unidiff-zero`, review the actual changes, and run the relevant
tests after applying a reconciled patch. Never treat a successful patch application
as runtime verification. Preserve all existing recovery/security assertions.

## Pending sequence

| Claim | Prepared work |
| --- | --- |
| 2002 | Shared binary UUID digest path keys |
| 2003 | Source-artifact process-test deadlines |
| 2004 | Aggregate function UUID matching and REAL64 fixture |
| 2005 | Bulk-import cancellation fixture |
| 2006 | Three short Unix-socket workspace allocators |
| 2007 | Catalog/CABI-issued bulk binding fixture |
| 2008 | Exact admitted DELETE datatype cohort |
| 2009 | Two additional short socket workspace allocators |
| 2010 | Twelve fixture UUID timestamp unit corrections |
| 2011 | Runtime session issuance and descriptor negatives |
| 2012 | Binary agent/filespace/observability evidence fixtures |
| 2013 | Binary shutdown acknowledgement fixture |
| 2014 | Existing Windows portability repair, preserved only; Linux remains the active scope |
| 2015 | SHOW CREATE binary column reader and fixture |
| 2016 | UPDATE/DELETE binary readers and actual-receipt DML fixture |
| 2017 | Published narrow-query fixture and engine linkage |
| 2018 | Binary table metadata frame, dependent on 2015 |
| 2019 | Actual serializable isolation fixture authority |

The old continuation stopped before 2002. Its script required the literal token
`is_aggregate_v` in `uuid_binary_key_test.cpp`, but that token was absent from the
checkpoint. Subsequent source inspection confirmed the non-aggregate constructor
and the `AcceptsShortTextUuid`/text-construction static assertions were already
present: this was a brittle script spelling check, not missing protection.
An explicit non-aggregate static assertion now complements those existing checks.
Do not reapply the already-present constructor patch or remove its safeguards.
The old waiting processes have been stopped.

The prepared detach fixture is not queued and still needs the actual default
filespace binding. The prepared COMMENT ON fixture does not implement the missing
catalog mutation. Default metric catalog binding and executor native descriptor
propagation also remain open. No feature is closed or deferred by this archive.

The older overlapping Sandbox UPDATE draft is separately committed and pushed on
`codex/sandbox-uuid-draft-20260926` at `36d3aaff9`. Do not merge that draft wholesale:
the current repair branch supersedes portions of its production changes. Retain
its additional binding-helper/component assertions for deliberate reconciliation.

Historical orchestration scripts, full prepared source copies and private logs
remain in the local handoff archive. They contain old absolute paths and must not
be executed unchanged. This public directory contains only ScratchBird source
patches and their integrity/continuation metadata, not private logs or binaries.
