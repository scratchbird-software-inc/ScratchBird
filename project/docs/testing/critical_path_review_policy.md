# Critical-path independent review policy

## Current pre-public-beta authority

Independent review enforcement is staged but intentionally **advisory** until
the public beta transition. `@DaltonCalford` is the sole current code owner and
has sole development and merge authority. `@Herb0t` and `@moloquin` are planned
future independent reviewers; they are not current code owners and are not
required for any pre-beta change.

The repository therefore has no Issue 5 merge lockdown in this phase:

- `.github/critical-path-reviewers.json` declares `advisory` and
  `pre_public_beta`;
- every critical domain currently names only `DaltonCalford` as owner;
- `.github/CODEOWNERS` names only `@DaltonCalford`;
- `.github/rulesets/main-critical-path.json` is disabled; and
- `configure_critical_review_ruleset.py --apply` refuses to run until both the
  policy and ruleset are explicitly changed for public beta.

The advisory workflow may still report missing future-review evidence, but it
publishes a successful status and cannot block a merge. Infrastructure or API
failures can make the workflow itself red, but its status is not a required
merge check during this phase.

## Staged public-beta policy

ScratchBird has predeclared critical domains for storage, transactions/MGA,
engine internal APIs, security and authentication, the optimizer,
parser/binder code, and CI/release automation. The path map is in
`.github/critical-path-reviewers.json`; `.github/CODEOWNERS` mirrors the active
owner assignment.

Once public-beta enforcement is deliberately activated, an approval qualifies
only when all of these are true:

- the account is in the curated human owner allowlist and GitHub reports it as
  a `User`, not a bot or AI reviewer;
- the reviewer is not the pull-request author;
- the review state is `APPROVED` on the pull request's current head commit;
- the review body records specific invariants, runtime evidence, and remaining
  uncertainty using the fields in the pull-request template; and
- the reviewer owns every affected domain for which their approval is counted.

One qualifying review may cover multiple affected domains when that reviewer
owns each domain. A later `CHANGES_REQUESTED` or dismissed review invalidates
that review. Every push changes the head SHA, so approval of an older SHA
becomes stale immediately.

Maintainers must keep automation-controlled accounts out of the future human
allowlist; GitHub's `User` type alone cannot prove human operation.

## Safe workflow design

`.github/workflows/critical-path-review.yml` uses `pull_request_target` so it
can read review metadata and publish a status for forked pull requests. It
checks out only the repository's default branch and never checks out or
executes pull-request-controlled code. The trusted gate publishes the
`critical-path/independent-review` status directly on the PR head SHA.

Noncritical changes pass automatically. During pre-beta development,
unfulfilled critical review is advisory and also passes. In required mode,
critical changes fail closed if policy data, PR data, or qualifying approval
evidence is missing or malformed.

## Pull-request and review records

The pull-request template provides these fields now so the team can exercise
the future process without making it mandatory:

1. `Invariants manually reviewed`
2. `Runtime tests executed`
3. `Remaining uncertainty`

When required mode is active, the independent reviewer must submit an approval
whose review body contains:

```text
Invariants reviewed:
<the exact architectural, durability, security, or semantic invariants inspected>

Runtime evidence reviewed:
<commands, environments, results, and failure artifacts examined>

Remaining uncertainty:
<known risk/follow-up or a specific statement that none was identified>
```

General text such as “looks good,” “reviewed,” or “tests pass” does not satisfy
the required-mode gate.

## Public-beta activation procedure

The staged ruleset blueprint will require, once activated:

- one native GitHub approval for every `main` pull request and code-owner
  approval when a CODEOWNERS path changes;
- dismissal of approvals on every push;
- approval from someone other than the last pusher;
- resolution of review conversations;
- the independent-review status; and
- the fail-closed Linux, Windows, and both macOS release-CI aggregate statuses.

Every required context is pinned to GitHub Actions application ID `15368`, as
verified against this repository's live check runs. A similarly named status
published from a personal token cannot satisfy the ruleset.

Activation is a deliberate public-beta change, not an ordinary setup step:

1. Confirm the planned reviewers are human-operated and authorized.
2. Give every selected reviewer repository write permission.
3. Change the policy to `enforcement_mode: required` and the appropriate beta
   release phase; move the selected people into every applicable domain's
   `owners` list.
4. Add the same active owners to `.github/CODEOWNERS` and remove them from the
   planned-reviewer list.
5. Change the versioned ruleset from `disabled` to `active`.
6. Update the static policy gate's expected release phase and owner inventory.
7. Enable all platform pull-request CI variables.
8. Merge and push that explicit transition under Dalton's pre-beta authority.
9. Run the ruleset tool with `--apply`, then run it read-only to verify the
   live configuration.

The anticipated permission and CI-variable commands are:

```sh
gh api --method PUT repos/scratchbird-software-inc/ScratchBird/collaborators/Herb0t -f permission=push
gh api --method PUT repos/scratchbird-software-inc/ScratchBird/collaborators/moloquin -f permission=push
gh variable set SB_LINUX_CI_ENABLED --repo scratchbird-software-inc/ScratchBird --body true
gh variable set SB_WINDOWS_CI_ENABLED --repo scratchbird-software-inc/ScratchBird --body true
gh variable set SB_MACOS_CI_ENABLED --repo scratchbird-software-inc/ScratchBird --body true
python3 project/tools/ci/configure_critical_review_ruleset.py --apply
python3 project/tools/ci/configure_critical_review_ruleset.py
```

Do not run those mutation commands before the authorized public-beta
transition. If either planned account should not own critical code, designate a
different verified human in the transition change.

## Policy maintenance and tests

The current advisory state and the future strict evaluation semantics are both
covered by repository-policy tests:

```sh
python3 -m unittest discover -s project/tests/repository_policy -p 'test_*.py'
python3 project/tools/release/github_actions_static_gate.py
```

Changes to the workflow, ownership policy, ruleset, gate, or release CI remain
part of the `ci-release` critical domain. They are advisory before public beta
and independently review-gated after the explicit transition.
