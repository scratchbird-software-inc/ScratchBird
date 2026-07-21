#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exercise the metadata-only manual prerelease boundary.

The manual route has no native payload provenance authority.  It must reject
every external copy request, then produce and verify its generated metadata
envelope without accepting a relabelled source/client/document payload.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


MANUAL_COPY_ERROR = "manual_bundle_payload_copy_forbidden"
ROLE = "generated_private_prerelease_metadata"
DESTINATION_HINT = "not_installable_metadata_attestation_only"
ROOT_METADATA = {"RELEASE_MANIFEST.json", "SHA256SUMS"}


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_rows(root: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix()
        if relative in ROOT_METADATA:
            continue
        rows.append(
            {
                "path": relative,
                "category": relative.split("/", 1)[0] if "/" in relative else "metadata",
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return rows


def rebuild_integrity_metadata(root: Path, release_date: str) -> None:
    """Make a malicious tree internally consistent before verifier testing."""

    location_rows = []
    for row in artifact_rows(root):
        if row["path"] == "FILE_LOCATION_MANIFEST.json":
            continue
        location_rows.append(
            {
                "path": row["path"],
                "role": ROLE,
                "installer_destination_hint": DESTINATION_HINT,
                "bytes": row["bytes"],
                "sha256": row["sha256"],
            }
        )
    (root / "FILE_LOCATION_MANIFEST.json").write_text(
        json.dumps(
            {
                "schema_id": "scratchbird.prerelease_file_location_manifest.v1",
                "release_date": release_date,
                "files": location_rows,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    release_manifest = json.loads(
        (root / "RELEASE_MANIFEST.json").read_text(encoding="utf-8")
    )
    rows = artifact_rows(root)
    release_manifest["artifacts"] = rows
    release_manifest["promoted_paths"] = []
    (root / "RELEASE_MANIFEST.json").write_text(
        json.dumps(release_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in rows),
        encoding="utf-8",
    )


def expect_failure(
    completed: subprocess.CompletedProcess[str], expected: str, label: str
) -> int:
    if completed.returncode == 0 or expected not in completed.stdout:
        print(f"{label}_not_rejected:{completed.stdout}", file=sys.stderr)
        return 1
    return 0


def promote_clean(
    promoter: Path,
    verifier: Path,
    public_repo: Path,
    fake_repo: Path,
    release_date: str,
) -> Path:
    completed = run(
        [
            sys.executable,
            str(promoter),
            "--repo-root",
            str(fake_repo),
            "--release-date",
            release_date,
        ],
        public_repo,
    )
    if completed.returncode != 0:
        print(completed.stdout, end="", file=sys.stderr)
        raise RuntimeError("clean promotion failed")
    root = fake_repo / "packaging" / release_date
    verified = run(
        [sys.executable, str(verifier), str(root), "--repo-root", str(fake_repo)],
        public_repo,
    )
    if verified.returncode != 0:
        print(verified.stdout, end="", file=sys.stderr)
        raise RuntimeError("clean promotion did not verify")
    return root


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args()
    public_repo = args.repo_root.resolve()
    promoter = public_repo / "project" / "tools" / "release" / "promote_prerelease_bundle.py"
    verifier = public_repo / "project" / "tools" / "release" / "verify_prerelease_packaging_bundle.py"
    if not promoter.is_file() or not verifier.is_file():
        print("prerelease_tools_missing", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="sb-prerelease-client-boundary-") as temp:
        fake_repo = Path(temp) / "repo"
        fake_repo.mkdir()
        (fake_repo / "packaging").mkdir()
        (fake_repo / "docs").mkdir()
        (fake_repo / "docs" / "wire.txt").write_text(
            "import socket\n# intentionally neutral-named functional client code\n",
            encoding="utf-8",
        )
        (fake_repo / "project" / "drivers" / "driver").mkdir(parents=True)
        (fake_repo / "project" / "drivers" / "driver" / "client.txt").write_text(
            "unadmitted client\n", encoding="utf-8"
        )
        (fake_repo / "project" / "dbeaver").mkdir(parents=True)
        (fake_repo / "project" / "dbeaver" / "bridge.txt").write_text(
            "unadmitted adaptor\n", encoding="utf-8"
        )

        for source_dest, label in (
            ("docs/wire.txt=docs/wire.txt", "neutral_source_copy"),
            ("project/drivers/driver/client.txt=docs/client.txt", "driver_source_copy"),
            ("project/dbeaver/bridge.txt=proofs/bridge.txt", "dbeaver_source_copy"),
        ):
            rejected = run(
                [
                    sys.executable,
                    str(promoter),
                    "--repo-root",
                    str(fake_repo),
                    "--release-date",
                    "2026.07.20",
                    "--copy",
                    source_dest,
                ],
                public_repo,
            )
            if expect_failure(rejected, MANUAL_COPY_ERROR, label):
                return 1
            if (fake_repo / "packaging" / "2026.07.20").exists():
                print(f"{label}_created_target", file=sys.stderr)
                return 1

        try:
            clean_root = promote_clean(
                promoter, verifier, public_repo, fake_repo, "2026.07.20"
            )
        except RuntimeError:
            return 1
        manifest = json.loads(
            (clean_root / "RELEASE_MANIFEST.json").read_text(encoding="utf-8")
        )
        policy = manifest.get("policy")
        if (
            not isinstance(policy, dict)
            or policy.get("client_artifacts_permitted") is not False
            or policy.get("admitted_driver_adaptor_mcp_components") != []
            or policy.get("dbeaver_hard_excluded") is not True
            or policy.get("manual_bundle_binary_payloads_permitted") is not False
            or policy.get("manual_bundle_payload_files_permitted") is not False
            or policy.get("manual_bundle_external_copy_permitted") is not False
            or manifest.get("promoted_paths") != []
            or not (clean_root / "FILE_LOCATION_MANIFEST.json").is_file()
        ):
            print("metadata_prerelease_policy_or_manifest_invalid", file=sys.stderr)
            return 1

        wire_root = promote_clean(
            promoter, verifier, public_repo, fake_repo, "2026.07.21"
        )
        (wire_root / "docs" / "wire.txt").write_text(
            "import socket\n# neutral-named functional client code\n", encoding="utf-8"
        )
        rebuild_integrity_metadata(wire_root, "2026.07.21")
        rejected = run(
            [sys.executable, str(verifier), str(wire_root), "--repo-root", str(fake_repo)],
            public_repo,
        )
        if expect_failure(
            rejected,
            "manual_bundle_payload_entry_forbidden:docs/wire.txt",
            "verifier_neutral_source_payload",
        ):
            return 1

        readme_root = promote_clean(
            promoter, verifier, public_repo, fake_repo, "2026.07.22"
        )
        (readme_root / "README.md").write_text(
            "import socket\n# source code disguised as metadata\n", encoding="utf-8"
        )
        rebuild_integrity_metadata(readme_root, "2026.07.22")
        rejected = run(
            [sys.executable, str(verifier), str(readme_root), "--repo-root", str(fake_repo)],
            public_repo,
        )
        if expect_failure(
            rejected,
            "manual_bundle_generated_text_mismatch:README.md",
            "verifier_root_source_payload",
        ):
            return 1
    print("prerelease_bundle_client_boundary_test=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
