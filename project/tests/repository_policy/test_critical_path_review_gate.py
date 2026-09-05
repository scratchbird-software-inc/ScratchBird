#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "project" / "tools" / "ci" / "critical_path_review_gate.py"
SPEC = importlib.util.spec_from_file_location("critical_path_review_gate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


POLICY = {
    "schema": gate.POLICY_SCHEMA,
    "enforcement_mode": "required",
    "status_context": "critical-path/independent-review",
    "minimum_approvals_per_domain": 1,
    "domains": [
        {
            "name": "storage",
            "owners": ["author", "reviewer", "second-reviewer"],
            "paths": ["project/src/storage/**"],
        },
        {
            "name": "ci-release",
            "owners": ["reviewer"],
            "paths": [".github/**"],
        },
    ],
}

PR_BODY = """## Invariants manually reviewed

MGA visibility and page publication remain ordered by the engine transaction authority.

## Runtime tests executed

`ctest -R storage` completed with 18 of 18 tests passing on Linux.

## Remaining uncertainty

Crash recovery on macOS remains to be exercised by the nightly soak lane.
"""

REVIEW_BODY = """Invariants reviewed:
MGA visibility and page publication ordering were inspected in storage.cpp.

Runtime evidence reviewed:
The Linux `ctest -R storage` log reports 18 of 18 tests passing.

Remaining uncertainty:
The macOS crash-recovery schedule remains for the nightly soak lane.
"""


def review(
    login: str = "reviewer",
    *,
    state: str = "APPROVED",
    commit_id: str = "head-sha",
    body: str = REVIEW_BODY,
    user_type: str = "User",
    review_id: int = 1,
) -> dict:
    return {
        "id": review_id,
        "submitted_at": f"2026-09-05T12:00:{review_id:02d}Z",
        "state": state,
        "commit_id": commit_id,
        "body": body,
        "user": {"login": login, "type": user_type},
    }


class CriticalPathReviewGateTests(unittest.TestCase):
    def evaluate(self, paths, reviews, *, body=PR_BODY, author="author"):
        return gate.evaluate(
            policy=POLICY,
            changed_paths=paths,
            author_login=author,
            head_sha="head-sha",
            pr_body=body,
            reviews=reviews,
        )

    def test_noncritical_change_passes_without_review_record(self):
        passed, messages, affected = self.evaluate(["README.md"], [], body="")
        self.assertTrue(passed)
        self.assertEqual({}, affected)
        self.assertIn("no configured critical paths changed", messages)

    def test_current_independent_structured_approval_passes(self):
        passed, _, affected = self.evaluate(
            ["project/src/storage/page/page.cpp"], [review()]
        )
        self.assertTrue(passed)
        self.assertEqual({"storage"}, set(affected))

    def test_self_approval_cannot_satisfy_gate(self):
        passed, messages, _ = self.evaluate(
            ["project/src/storage/page/page.cpp"], [review("author")]
        )
        self.assertFalse(passed)
        self.assertIn("self-review", " ".join(messages))

    def test_bot_approval_cannot_satisfy_gate(self):
        passed, messages, _ = self.evaluate(
            ["project/src/storage/page/page.cpp"],
            [review("reviewer", user_type="Bot")],
        )
        self.assertFalse(passed)
        self.assertIn("not-human", " ".join(messages))

    def test_approval_on_previous_head_is_stale(self):
        passed, messages, _ = self.evaluate(
            ["project/src/storage/page/page.cpp"],
            [review(commit_id="previous-sha")],
        )
        self.assertFalse(passed)
        self.assertIn("stale", " ".join(messages))

    def test_unstructured_approval_cannot_satisfy_gate(self):
        passed, messages, _ = self.evaluate(
            ["project/src/storage/page/page.cpp"],
            [review(body="Looks good to me after a quick review.")],
        )
        self.assertFalse(passed)
        self.assertIn("missing-review-evidence", " ".join(messages))

    def test_pr_must_record_invariants_tests_and_uncertainty(self):
        passed, messages, _ = self.evaluate(
            ["project/src/storage/page/page.cpp"], [review()], body=""
        )
        self.assertFalse(passed)
        self.assertIn("PR body missing substantive sections", " ".join(messages))

    def test_latest_changes_requested_supersedes_approval(self):
        passed, messages, _ = self.evaluate(
            ["project/src/storage/page/page.cpp"],
            [review(review_id=1), review(state="CHANGES_REQUESTED", review_id=2)],
        )
        self.assertFalse(passed)
        self.assertIn("changes_requested", " ".join(messages))

    def test_domain_specific_owner_is_required(self):
        passed, messages, affected = self.evaluate(
            [".github/workflows/ci-linux.yml"],
            [review("second-reviewer")],
        )
        self.assertFalse(passed)
        self.assertEqual({"ci-release"}, set(affected))
        self.assertIn("from reviewer", " ".join(messages))

    def test_repository_policy_covers_overlapping_security_internal_api(self):
        policy = gate.load_policy(
            REPO_ROOT / ".github" / "critical-path-reviewers.json"
        )
        affected = gate.affected_domains(
            ["project/src/engine/internal_api/security/token.cpp"], policy
        )
        self.assertEqual({"engine-internal-api", "security"}, set(affected))

    def test_comment_after_approval_does_not_discard_decisive_approval(self):
        comment = review(state="COMMENTED", review_id=2)
        passed, _, _ = self.evaluate(
            ["project/src/storage/page/page.cpp"],
            [review(review_id=1), comment],
        )
        self.assertTrue(passed)

    def test_advisory_mode_never_blocks_an_unmet_review(self):
        policy = dict(POLICY)
        policy["enforcement_mode"] = "advisory"
        passed, label, messages = gate.apply_enforcement_mode(
            policy, False, ["storage requires independent approval"]
        )
        self.assertTrue(passed)
        self.assertEqual("advisory", label)
        self.assertIn("advisory only", messages[0])

    def test_repository_policy_is_pre_beta_and_dalton_only(self):
        policy = gate.load_policy(
            REPO_ROOT / ".github" / "critical-path-reviewers.json"
        )
        self.assertEqual("advisory", policy["enforcement_mode"])
        self.assertEqual(["DaltonCalford"], policy["current_authority"])
        self.assertEqual(
            {"Herb0t", "moloquin"}, set(policy["planned_independent_reviewers"])
        )
        for domain in policy["domains"]:
            self.assertEqual(["DaltonCalford"], domain["owners"])


if __name__ == "__main__":
    unittest.main()
