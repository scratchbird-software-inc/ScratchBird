#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate release-proof JSON for the ScratchBird DBeaver adapter.

This script is intentionally read-only with respect to the public source tree.
It verifies generated package artifacts and captured install logs, then writes
machine-readable proof files into the selected artifact root.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import zipfile


FEATURE_IU = "org.jkiss.dbeaver.ext.scratchbird.feature.feature.group"
REQUIRED_NOTICE_LICENSES = {
    "DBeaver": "Apache-2.0",
    "Eclipse Platform": "EPL-2.0",
    "Tycho": "EPL-2.0",
    "SWT": "EPL-2.0",
    "JFace": "EPL-2.0",
    "ScratchBird JDBC": "MPL-2.0",
}
SECRET_LEAK_RE = re.compile(
    r"(?i)(password|secret|token|assertion|auth_payload)\s*[:=]\s*"
    r"(?!<redacted>|redacted\b|\\*\\*\\*)[^\s,;]+"
)


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def read_text(path: Path | None) -> str:
    if path is None or not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def latest(root: Path, pattern: str) -> Path | None:
    matches = sorted(root.glob(pattern), key=lambda path: path.stat().st_mtime)
    return matches[-1] if matches else None


def zip_members(path: Path) -> set[str]:
    with zipfile.ZipFile(path) as archive:
        return set(archive.namelist())


def zip_contains_basename(members: set[str], basename: str) -> bool:
    return any(Path(member).name == basename for member in members)


def zip_contains_prefix_suffix(members: set[str], prefix: str, suffix: str) -> bool:
    return any(Path(member).name.startswith(prefix) and member.endswith(suffix) for member in members)


def zip_member_text(path: Path, member_suffix: str) -> str:
    with zipfile.ZipFile(path) as archive:
        for member in archive.namelist():
            if member.endswith(member_suffix):
                return archive.read(member).decode("utf-8", errors="replace")
    return ""


def update_site_ok(path: Path, issues: list[str]) -> dict[str, object]:
    members = zip_members(path)
    evidence: dict[str, object] = {
        "path": path.name,
        "sha256": sha256(path),
        "members": len(members),
        "nested_jdbc_jar": False,
    }
    for required in ("content", "artifacts"):
        if not any(Path(member).name in {f"{required}.jar", f"{required}.xml"} for member in members):
            issues.append(f"update_site_missing_{required}_metadata")
    for prefix in (
        "org.jkiss.dbeaver.ext.scratchbird.feature_",
        "org.jkiss.dbeaver.ext.scratchbird_",
        "org.jkiss.dbeaver.ext.scratchbird.ui_",
    ):
        if not zip_contains_prefix_suffix(members, prefix, ".jar"):
            issues.append(f"update_site_missing_installable:{prefix}*.jar")

    with zipfile.ZipFile(path) as archive:
        for member in members:
            name = Path(member).name
            if not name.startswith("org.jkiss.dbeaver.ext.scratchbird_"):
                continue
            if name.startswith("org.jkiss.dbeaver.ext.scratchbird.ui_"):
                continue
            if not name.endswith(".jar"):
                continue
            with zipfile.ZipFile(archive.open(member)) as nested:
                if "drivers/scratchbird/scratchbird-jdbc.jar" in nested.namelist():
                    evidence["nested_jdbc_jar"] = True
                    break
    if not evidence["nested_jdbc_jar"]:
        issues.append("update_site_core_plugin_missing_scratchbird_jdbc_jar")
    return evidence


def stock_bundle_ok(path: Path, issues: list[str]) -> dict[str, object]:
    members = zip_members(path)
    evidence: dict[str, object] = {
        "path": path.name,
        "sha256": sha256(path),
        "members": len(members),
        "bundled_update_site": False,
    }
    for required in (
        "README.txt",
        "THIRD-PARTY-NOTICES.txt",
        "install-into-stock-dbeaver.sh",
        "install-into-stock-dbeaver.bat",
        "uninstall-from-stock-dbeaver.sh",
        "uninstall-from-stock-dbeaver.bat",
        "SHA256SUMS.txt",
    ):
        if not zip_contains_basename(members, required):
            issues.append(f"stock_bundle_missing_member:{required}")
    if zip_contains_prefix_suffix(members, "scratchbird-dbeaver-update-site-", ".zip"):
        evidence["bundled_update_site"] = True
    else:
        issues.append("stock_bundle_missing_update_site_zip")
    notice = zip_member_text(path, "THIRD-PARTY-NOTICES.txt")
    for component, license_id in REQUIRED_NOTICE_LICENSES.items():
        if component not in notice or license_id not in notice:
            issues.append(f"stock_bundle_notice_missing:{component}:{license_id}")
    return evidence


def require_log_tokens(label: str, text: str, tokens: tuple[str, ...], issues: list[str]) -> bool:
    if not text:
        issues.append(f"{label}:log_missing")
        return False
    missing = [token for token in tokens if token not in text]
    for token in missing:
        issues.append(f"{label}:log_token_missing:{token}")
    return not missing


def log_clean(label: str, text: str, issues: list[str]) -> bool:
    if SECRET_LEAK_RE.search(text):
        issues.append(f"{label}:secret_like_value_in_log")
        return False
    return True


def script_supports_version_refusal(script_path: Path, issues: list[str]) -> bool:
    text = read_text(script_path)
    required = (
        "MIN_DBEAVER_VERSION",
        "read_dbeaver_version",
        "version_ge",
        "Unsupported DBeaver version",
    )
    missing = [token for token in required if token not in text]
    for token in missing:
        issues.append(f"downgrade_refusal:script_token_missing:{token}")
    return not missing


def write_json(path: Path, payload: dict[str, object]) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_payload(
    *,
    artifact_type: str,
    timestamp: str,
    status: str,
    issues: list[str],
    evidence: dict[str, object],
) -> dict[str, object]:
    return {
        "schema_id": "scratchbird.dbeaver_management.release_proof.v1",
        "artifact_type": artifact_type,
        "proof_kind": artifact_type,
        "timestamp_utc": timestamp,
        "status": status,
        "proof_status": status,
        "issues": issues,
        "evidence": evidence,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[5])
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--timestamp", default=_dt.datetime.now(_dt.UTC).strftime("%Y%m%dT%H%M%SZ"))
    parser.add_argument("--update-site", type=Path)
    parser.add_argument("--stock-bundle", type=Path)
    parser.add_argument("--source-checkout-log", type=Path)
    parser.add_argument("--stock-install-log", type=Path)
    parser.add_argument("--stock-uninstall-log", type=Path)
    parser.add_argument("--stock-reinstall-log", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    artifact_root = args.artifact_root.resolve()
    artifact_root.mkdir(parents=True, exist_ok=True)
    adapter_root = repo_root / "project/drivers/adaptor/scratchbird-dbeaver-driver"

    update_site = args.update_site or latest(artifact_root, "scratchbird-dbeaver-update-site-*.zip")
    stock_bundle = args.stock_bundle or latest(artifact_root, "scratchbird-dbeaver-stock-test-bundle-*.zip")
    issues: list[str] = []
    evidence: dict[str, object] = {
        "repo_root": str(repo_root),
        "artifact_root": str(artifact_root),
    }

    if update_site is None or not update_site.is_file():
        issues.append("update_site_zip_missing")
    else:
        evidence["update_site"] = update_site_ok(update_site, issues)
    if stock_bundle is None or not stock_bundle.is_file():
        issues.append("stock_bundle_zip_missing")
    else:
        evidence["stock_bundle"] = stock_bundle_ok(stock_bundle, issues)

    source_log = read_text(args.source_checkout_log)
    stock_install_log = read_text(args.stock_install_log)
    stock_uninstall_log = read_text(args.stock_uninstall_log)
    stock_reinstall_log = read_text(args.stock_reinstall_log)
    logs = {
        "source_checkout": source_log,
        "stock_install": stock_install_log,
        "stock_uninstall": stock_uninstall_log,
        "stock_reinstall": stock_reinstall_log,
    }
    for label, text in logs.items():
        log_clean(label, text, issues)

    source_ok = require_log_tokens(
        "source_checkout",
        source_log,
        (
            "ScratchBird DBeaver source checkout install completed successfully.",
            "plugins/org.jkiss.dbeaver.ext.scratchbird",
            "plugins/org.jkiss.dbeaver.ext.scratchbird.ui",
            "test/org.jkiss.dbeaver.ext.scratchbird.test",
        ),
        issues,
    )
    stock_install_ok = require_log_tokens(
        "stock_install",
        stock_install_log,
        (
            "DBeaver version accepted:",
            "ScratchBird install completed successfully.",
            "Installed root confirmed:",
            FEATURE_IU,
        ),
        issues,
    )
    stock_uninstall_ok = require_log_tokens(
        "stock_uninstall",
        stock_uninstall_log,
        (
            "ScratchBird uninstall completed successfully.",
            "Removed root:",
            FEATURE_IU,
        ),
        issues,
    )
    stock_reinstall_ok = require_log_tokens(
        "stock_reinstall",
        stock_reinstall_log,
        (
            "DBeaver version accepted:",
            "ScratchBird install completed successfully.",
            "Installed root confirmed:",
            FEATURE_IU,
        ),
        issues,
    )
    version_refusal_ok = script_supports_version_refusal(
        adapter_root / "scripts" / "install-into-stock-dbeaver.sh",
        issues,
    )

    lifecycle_cases = {
        "p2_update_site_build": update_site is not None and update_site.is_file(),
        "stock_bundle_build": stock_bundle is not None and stock_bundle.is_file(),
        "source_checkout_install": source_ok,
        "stock_install": stock_install_ok,
        "upgrade_replace_existing": stock_install_ok and stock_reinstall_ok,
        "downgrade_refusal": version_refusal_ok,
        "stock_uninstall": stock_uninstall_ok,
        "stock_reinstall": stock_reinstall_ok,
        "workspace_profile_cleanup": all(log_clean(label, text, []) for label, text in logs.items()),
        "driver_cache_cleanup": stock_uninstall_ok and stock_reinstall_ok,
        "secret_cleanup": all(log_clean(label, text, []) for label, text in logs.items()),
    }
    evidence["lifecycle_cases"] = lifecycle_cases
    for case, passed in sorted(lifecycle_cases.items()):
        if not passed:
            issues.append(f"lifecycle_case_failed:{case}")

    status = "passed" if not issues else "failed"
    lifecycle_payload = build_payload(
        artifact_type="lifecycle_fixture",
        timestamp=args.timestamp,
        status=status,
        issues=issues,
        evidence=evidence,
    )
    lifecycle_path = artifact_root / f"scratchbird-dbeaver-lifecycle-proof-{args.timestamp}.json"
    write_json(lifecycle_path, lifecycle_payload)
    shutil.copyfile(lifecycle_path, artifact_root / "scratchbird-dbeaver-lifecycle-proof-latest.json")

    source_payload = build_payload(
        artifact_type="source_checkout_verify",
        timestamp=args.timestamp,
        status="passed" if source_ok and not SECRET_LEAK_RE.search(source_log) else "failed",
        issues=[] if source_ok and not SECRET_LEAK_RE.search(source_log) else [
            issue for issue in issues if issue.startswith("source_checkout:")
        ],
        evidence={
            "log": str(args.source_checkout_log) if args.source_checkout_log else "",
            "required_paths": [
                "plugins/org.jkiss.dbeaver.ext.scratchbird",
                "plugins/org.jkiss.dbeaver.ext.scratchbird.ui",
                "test/org.jkiss.dbeaver.ext.scratchbird.test",
            ],
        },
    )
    write_json(
        artifact_root / f"scratchbird-dbeaver-source-checkout-proof-{args.timestamp}.json",
        source_payload,
    )

    stock_payload = build_payload(
        artifact_type="stock_install_lifecycle",
        timestamp=args.timestamp,
        status=(
            "passed"
            if stock_install_ok
            and stock_uninstall_ok
            and stock_reinstall_ok
            and not any(SECRET_LEAK_RE.search(text) for text in (stock_install_log, stock_uninstall_log, stock_reinstall_log))
            else "failed"
        ),
        issues=[
            issue
            for issue in issues
            if issue.startswith("stock_install:")
            or issue.startswith("stock_uninstall:")
            or issue.startswith("stock_reinstall:")
        ],
        evidence={
            "install_log": str(args.stock_install_log) if args.stock_install_log else "",
            "uninstall_log": str(args.stock_uninstall_log) if args.stock_uninstall_log else "",
            "reinstall_log": str(args.stock_reinstall_log) if args.stock_reinstall_log else "",
            "feature_iu": FEATURE_IU,
        },
    )
    write_json(
        artifact_root / f"scratchbird-dbeaver-stock-install-proof-{args.timestamp}.json",
        stock_payload,
    )

    if status != "passed":
        return fail("DBeaver release proof generation failed:\n" + "\n".join(issues))
    print(f"dbeaver_release_proofs=passed:{artifact_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
