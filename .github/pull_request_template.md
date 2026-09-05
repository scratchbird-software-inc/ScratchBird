## Change summary

<!-- State what changed and why. Identify AI-assisted implementation explicitly. -->

## Critical paths changed

<!-- Check every applicable domain. The gate also derives this from changed paths. -->

- [ ] None
- [ ] Storage
- [ ] Transactions / MGA
- [ ] Engine internal APIs
- [ ] Security / authentication
- [ ] Optimizer
- [ ] Parser / binder
- [ ] CI / release

## Invariants manually reviewed

<!-- Name concrete invariants and how the change preserves them. Do not write only "reviewed" or "tests pass". -->

## Runtime tests executed

<!-- List exact commands, environments, and results. If none ran, give a specific risk-based reason. -->

## Remaining uncertainty

<!-- Identify unresolved risk or state why no material uncertainty remains. -->

## Independent critical-path review

> Pre-public-beta status: advisory only. Dalton retains sole authority and this
> section does not block merging until the public-beta review policy is
> explicitly activated.

For a critical-path change, request approval from an owner named in
`.github/critical-path-reviewers.json`. The approving human must not be the PR
author and must put this completed record in the submitted GitHub review:

```text
Invariants reviewed:
<specific invariants inspected manually>

Runtime evidence reviewed:
<specific runtime commands/results examined>

Remaining uncertainty:
<unresolved risk, follow-up, or a specific statement that none was identified>
```

An approval on an earlier commit does not apply after another push.
