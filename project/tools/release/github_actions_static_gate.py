#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public GitHub Actions release automation shape."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


REQUIRED_WORKFLOWS = {
    "ci-linux.yml": (
        "workflow_dispatch:",
        "cmake --preset public-release-linux",
        "ctest --preset public-release-linux",
        "verify_public_release_bundle.py",
        "public_packaging_history_gate.py",
    ),
    "ci-windows.yml": (
        "workflow_dispatch:",
        "cmake --preset public-release-windows",
        "ctest --preset public-release-windows",
        "verify_public_release_bundle.py",
        "public_packaging_history_gate.py",
    ),
    "ci-macos.yml": (
        "workflow_dispatch:",
        "SB_MACOS_CI_ENABLED",
        "macos-15-intel",
        "macos-15",
        "cmake --preset public-release-macos",
        "run_ctest_chunks.py",
        "--preset public-release-macos",
        "verify_public_release_bundle.py",
        "build_installers.py",
        "make_macos_universal.py",
        "verify_installer_artifacts.py",
        "smoke_install_macos.sh",
        "smoke_macos_launchd_credential.sh",
    ),
    "verify-installers.yml": (
        "workflow_dispatch:",
        "build_installers.py",
        "verify_installer_artifacts.py",
        "smoke_install_linux.sh",
        "smoke_install_windows.ps1",
        "smoke_install_macos.sh",
        "smoke_macos_launchd_credential.sh",
        "make_macos_universal.py",
        "public-release-macos",
    ),
    "nightly-installers.yml": (
        "schedule:",
        "workflow_dispatch:",
        "SB_NIGHTLY_INSTALLERS_ENABLED",
        "build_installers.py",
        "verify_installer_artifacts.py",
        "name: scratchbird-linux-installers",
        "name: scratchbird-windows-installers",
        "name: scratchbird-macos-x86_64-installers",
        "name: scratchbird-macos-arm64-installers",
        "name: scratchbird-macos-universal-installers",
        "create_nightly_release_bundle.py",
        "publish_rolling_nightly.py",
        "scratchbird-nightly-SHA256SUMS",
        "macos",
    ),
    "nightly-linux-installers.yml": (
        "push:",
        "workflow_dispatch:",
        "verify-installers.yml",
        "platform: linux",
        "publish-platform-nightly.yml",
        "release_scope: linux",
    ),
    "nightly-macos-installers.yml": (
        "push:",
        "workflow_dispatch:",
        "verify-installers.yml",
        "platform: macos",
        "publish-platform-nightly.yml",
        "release_scope: macos",
    ),
    "nightly-windows-installers.yml": (
        "push:",
        "workflow_dispatch:",
        "verify-installers.yml",
        "platform: windows",
        "publish-platform-nightly.yml",
        "release_scope: windows",
    ),
    "publish-platform-nightly.yml": (
        "workflow_call:",
        "contents: write",
        "create_nightly_release_bundle.py",
        "publish_rolling_nightly.py",
        "scratchbird-nightly-$SB_RELEASE_SCOPE-SHA256SUMS",
    ),
    "release-candidate.yml": (
        "workflow_dispatch:",
        "gh release",
        "verify_installer_artifacts.py",
        "INSTALLER_ARTIFACT_MANIFEST.json",
        "macos",
    ),
    "webserver-package-export.yml": (
        "workflow_dispatch:",
        "contents: read",
        "verify-installers.yml",
        "create_web_distribution_bundle.py",
        "name: scratchbird-linux-installers",
        "name: scratchbird-windows-installers",
        "name: scratchbird-macos-x86_64-installers",
        "name: scratchbird-macos-arm64-installers",
        "name: scratchbird-macos-universal-installers",
        "scratchbird-webserver-package-export",
        "Create webserver upload bundle",
    ),
}


FORBIDDEN_TOKENS = (
    "packaging/",
    "packaging\\",
    "ScratchBird" + "-Private",
    "/home/",
    "docs/workplans",
    "docs/specifications",
)

PUBLICATION_WORKFLOWS = {
    "release-candidate.yml",
    "nightly-installers.yml",
    "nightly-linux-installers.yml",
    "nightly-macos-installers.yml",
    "nightly-windows-installers.yml",
    "publish-platform-nightly.yml",
}

# Public distribution is intentionally native-server-only until a separate
# completed-component controller exists.  A driver/adaptor/MCP cannot be made
# public merely by adding a workflow that bypasses the quarantined legacy
# gates or by routing it through the web-export workflow.
PUBLIC_DISTRIBUTION_WORKFLOWS = PUBLICATION_WORKFLOWS | {
    "webserver-package-export.yml",
}
FORBIDDEN_UNADMITTED_COMPONENT_PUBLICATION_TOKENS = (
    "stage_driver_beta_artifacts.py",
    "verify_driver_beta_release.py",
    "driver_release_artifact_manifest_gate.py",
    "promote_prerelease_bundle.py",
    "verify_prerelease_packaging_bundle.py",
    "project/drivers/driver/",
    "project/drivers/adaptor/",
    "project/drivers/adapter/",
    "project/ai/",
    "libscratchbird_client",
    "scratchbird_mojo_client_bridge",
)


def fail(message: str) -> None:
    print(f"github_actions_static_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require_token(text: str, token: str, rel: str) -> None:
    if token not in text:
        fail(f"missing_token:{rel}:{token}")


def check_permissions(text: str, rel: str) -> None:
    if "permissions:" not in text:
        fail(f"missing_permissions:{rel}")
    if re.search(r"contents:\s+write", text) and rel not in PUBLICATION_WORKFLOWS:
        fail(f"unexpected_contents_write:{rel}")
    if rel == "webserver-package-export.yml":
        for token in ("contents: write", "gh release", "git tag", "git push origin"):
            if token in text:
                fail(f"webserver_export_publication_token_forbidden:{token}")


def check_workflow_inventory(workflow_root: Path) -> None:
    """Fail when a new workflow is added outside the reviewed inventory."""

    actual = {
        path.name
        for pattern in ("*.yml", "*.yaml")
        for path in workflow_root.glob(pattern)
        if path.is_file()
    }
    expected = set(REQUIRED_WORKFLOWS)
    unexpected = sorted(actual - expected)
    if unexpected:
        fail(f"workflow_inventory_unreviewed:{','.join(unexpected)}")


def check_unadmitted_component_publication(text: str, rel: str) -> None:
    """Keep client release routes closed until explicit component admission."""

    if rel not in PUBLIC_DISTRIBUTION_WORKFLOWS:
        return
    for token in FORBIDDEN_UNADMITTED_COMPONENT_PUBLICATION_TOKENS:
        if token in text:
            fail(f"unadmitted_component_publication_token:{rel}:{token}")


def check_portable_only_public_workflow(text: str, rel: str) -> None:
    """Forbid retired receipt/system-package publication switches in CI."""

    if rel not in PUBLIC_DISTRIBUTION_WORKFLOWS | {
        "verify-installers.yml",
        "ci-macos.yml",
    }:
        return
    for token in (
        "--write-native-admission-receipts",
        "--require-rpm",
    ):
        if token in text:
            fail(f"portable_only_public_workflow_forbidden:{rel}:{token}")


def check_failure_uploads_are_textual(text: str, rel: str) -> None:
    """Keep ``always()`` diagnostics from publishing raw payload bytes.

    Installer smoke work roots contain extracted binaries.  A failure upload
    may carry only the separately materialized textual-proof root; the same
    rule closes direct staged-native output uploads from ordinary platform CI.
    """

    # Split the workflow into complete steps before looking for uploads.  A
    # prior expression began at the first ``- name`` before an upload and
    # searched forward for ``uses: upload-artifact``.  That accidentally
    # associated a staging step's raw build path with the following textual
    # proof upload.  Each candidate here is exactly one YAML step instead.
    for candidate in re.finditer(
        r"(?ms)^      - name: [^\n]+\n(?:(?!^      - name:).)*", text
    ):
        step = candidate.group(0)
        if "uses: actions/upload-artifact@v4" not in step:
            continue
        if "always()" not in step:
            continue
        for forbidden in (
            "build/native-release-",
            "build/installers/",
            "build/install-smoke",
        ):
            if forbidden in step:
                fail(f"raw_binary_failure_upload_forbidden:{rel}:{forbidden}")
        if re.search(r"build/public-release-[^/\s]+/output/", step):
            fail(f"raw_binary_failure_upload_forbidden:{rel}:public-release-output")
        if "path: build/failure-proofs/" not in step:
            fail(f"textual_failure_upload_root_missing:{rel}")


def check_webserver_export_native_boundary(text: str, rel: str) -> None:
    """Require the web route to consume only reviewed installer artifacts."""

    block = workflow_job_block(text, "package-webserver-export", rel)
    for token in (
        "actions/download-artifact@v4",
        "name: scratchbird-linux-installers",
        "name: scratchbird-windows-installers",
        "name: scratchbird-macos-x86_64-installers",
        "name: scratchbird-macos-arm64-installers",
        "name: scratchbird-macos-universal-installers",
        "path: package-artifacts/scratchbird-linux-installers",
        "path: package-artifacts/scratchbird-windows-installers",
        "path: package-artifacts/scratchbird-macos-x86_64-installers",
        "path: package-artifacts/scratchbird-macos-arm64-installers",
        "path: package-artifacts/scratchbird-macos-universal-installers",
        "Verify downloaded installer manifests",
        "verify_installer_artifacts.py",
        "create_web_distribution_bundle.py",
    ):
        require_token(block, token, rel)
    if "pattern:" in block or "merge-multiple:" in block:
        fail(f"webserver_export_broad_artifact_download_forbidden:{rel}")
    for candidate in re.finditer(
        r"(?ms)^      - name: [^\n]+\n(?:(?!^      - name:).)*", block
    ):
        step = candidate.group(0)
        if "uses: actions/download-artifact@v4" not in step:
            continue
        if "name: scratchbird-" not in step or "path: package-artifacts/scratchbird-" not in step:
            fail(f"webserver_export_unscoped_artifact_download:{rel}")


def check_main_only_public_dispatch(
    text: str,
    rel: str,
    build_job: str,
    publish_job: str,
    context_step: str,
) -> None:
    """Require manually dispatched public distribution routes to stay on main."""

    build_block = workflow_job_block(text, build_job, rel)
    publish_block = workflow_job_block(text, publish_job, rel)
    for block, label in ((build_block, build_job), (publish_block, publish_job)):
        require_token(block, "github.ref == 'refs/heads/main'", rel)
        if "if:" not in block:
            fail(f"public_dispatch_main_guard_missing:{rel}:{label}")
    for token in (
        context_step,
        'test "$GITHUB_REF" = "refs/heads/main"',
        'test "$(git rev-parse HEAD)" = "$GITHUB_SHA"',
        "ref: ${{ github.sha }}",
    ):
        require_token(publish_block, token, rel)


def workflow_job_block(text: str, job_name: str, rel: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(job_name)}:\s*\n.*?(?=^  [A-Za-z0-9_-]+:\s*$|\Z)",
        text,
    )
    if match is None:
        fail(f"workflow_job_missing:{rel}:{job_name}")
    return match.group(0)


def check_push_to_main_trigger(text: str, rel: str) -> None:
    trigger_match = re.search(
        r"(?ms)^on:\s*\n.*?(?=^permissions:\s*$)", text
    )
    if trigger_match is None:
        fail(f"workflow_trigger_block_missing:{rel}")
    if re.search(
        r"(?m)^  push:\s*\n    branches:\s*\n      - main\s*$",
        trigger_match.group(0),
    ) is None:
        fail(f"main_push_trigger_missing:{rel}")


def check_main_push_ci_policy(
    text: str,
    rel: str,
    job_name: str,
    opt_in_variable: str,
) -> None:
    check_push_to_main_trigger(text, rel)
    block = workflow_job_block(text, job_name, rel)
    expected_condition = (
        "if: ${{ github.event_name != 'pull_request' || "
        f"vars.{opt_in_variable} == 'true' }}"
    )
    require_token(block, expected_condition, rel)


def check_public_installer_materialization(
    text: str,
    rel: str,
    job_name: str,
    platform: str,
    step_name: str,
    public_artifact_path: str,
) -> None:
    """Require uploads to consume an exact clean portable publication root."""

    block = workflow_job_block(text, job_name, rel)
    require_token(block, "verify_installer_artifacts.py", rel)
    require_token(block, f"--platform {platform}", rel)
    require_token(
        block, f"--materialize-public-root {public_artifact_path}", rel
    )
    step = re.search(
        rf"(?ms)^      - name: {re.escape(step_name)}\s*\n.*?(?=^      - name:|\Z)",
        block,
    )
    if step is None:
        fail(
            "public_installer_upload_step_missing:"
            f"{rel}:{job_name}:{step_name}"
        )
    upload = step.group(0)
    for token in (
        "uses: actions/upload-artifact@v4",
        f"path: {public_artifact_path}",
        "if-no-files-found: error",
    ):
        require_token(upload, token, rel)
    if "success()" not in upload or "always()" in upload:
        fail(f"public_installer_upload_condition_invalid:{rel}:{job_name}:{step_name}")
    if "path: build/installers/" in upload:
        fail(f"raw_installer_upload_forbidden:{rel}:{job_name}:{step_name}")


def check_public_universal_materialization(
    text: str,
    rel: str,
    job_name: str,
    public_artifact_path: str,
) -> None:
    """Universal macOS output must be written directly to its clean root."""

    block = workflow_job_block(text, job_name, rel)
    for token in (
        "make_macos_universal.py",
        f"--output-root {public_artifact_path}",
        "x86_tars=()",
        "arm_tars=()",
        'test "${#x86_tars[@]}" -eq 1',
        'test "${#arm_tars[@]}" -eq 1',
    ):
        require_token(block, token, rel)
    step = re.search(
        r"(?ms)^      - name: Upload universal macOS QA artifact\s*\n.*?(?=^      - name:|\Z)",
        block,
    )
    if step is None:
        fail(f"public_universal_upload_step_missing:{rel}:{job_name}")
    upload = step.group(0)
    for token in (
        "uses: actions/upload-artifact@v4",
        f"path: {public_artifact_path}",
        "if-no-files-found: error",
    ):
        require_token(upload, token, rel)
    if "success()" not in upload or "always()" in upload:
        fail(f"public_universal_upload_condition_invalid:{rel}:{job_name}")


def check_installer_reusable_policy(text: str, rel: str) -> None:
    """Keep the build/smoke workflow callable without duplicating push runs."""

    if "workflow_call:" not in text or "workflow_dispatch:" not in text:
        fail(f"installer_reusable_entrypoints_missing:{rel}")
    if re.search(r"(?m)^  push:\s*$", text):
        fail(f"installer_direct_push_trigger_forbidden:{rel}")
    for job_name in (
        "linux-installers",
        "windows-installers",
        "macos-installers",
        "macos-universal-installers",
    ):
        block = workflow_job_block(text, job_name, rel)
        if "github.event_name == 'push'" in block:
            fail(f"installer_push_condition_forbidden:{rel}:{job_name}")


def check_linux_privileged_bootstrap_smoke(text: str, rel: str) -> None:
    """Require hosted installer verification to execute, not skip, bootstrap."""

    block = workflow_job_block(text, "linux-installers", rel)
    required_tokens = (
        "Smoke Linux system-package lifecycle",
        "smoke_install_linux_system.py",
        "--artifact-root build/installers/linux",
        "--work-root build/install-smoke/linux/system-package-proof",
        "--run-privileged-deb-install",
        "--require-privileged-deb-install",
        "set -o pipefail",
        "build/install-smoke/linux/system-package-workflow.log",
    )
    for token in required_tokens:
        require_token(block, token, rel)


def check_macos_system_installer_failure_diagnostics(
    text: str,
    rel: str,
    job_name: str,
) -> None:
    """Keep real pkg postinstall failures diagnosable from the smoke proof."""

    block = workflow_job_block(text, job_name, rel)
    required_tokens = (
        'installer -verboseR -pkg "$package" -target /',
        "installer_status=${PIPESTATUS[0]}",
        "sudo tail -n 400 /var/log/install.log",
        "system-installer-install.log",
        "sudo log show --style syslog --last 5m",
        "system-installer-unified.log",
        'eventMessage CONTAINS[c] "scratchbird-installer"',
        "system-installer-stage.log",
    )
    for token in required_tokens:
        require_token(block, token, rel)


def check_macos_system_payload_verifier_authority(
    text: str,
    rel: str,
    job_name: str,
) -> None:
    """Require the system-payload verifier to retain root read authority.

    The macOS package deliberately protects its configuration root as
    ``root:scratchbird`` mode ``0750``.  The hosted verifier must use the
    existing package-install sudo authority, rather than loosening that
    installed security boundary merely for a test read.
    """

    block = workflow_job_block(text, job_name, rel)
    privileged_verifier = (
        'sudo "$(command -v python3)" '
        "project/tools/release/verify_native_installed_payload.py"
    )
    require_token(block, privileged_verifier, rel)
    unprivileged_verifier = (
        "          python3 project/tools/release/verify_native_installed_payload.py"
    )
    if unprivileged_verifier in block:
        fail(f"macos_system_payload_verifier_not_privileged:{rel}:{job_name}")


def check_macos_nonmember_probe(
    text: str,
    rel: str,
    job_name: str,
) -> None:
    """Keep the macOS false checkmember predicate explicit and fail-closed."""

    block = workflow_job_block(text, job_name, rel)
    required_tokens = (
        'sudo env LC_ALL=C dseditgroup -n . -o checkmember -m "$host_user" scratchbird',
        "host_user_membership_status=$?",
        "host-user-group-membership-status.txt",
        "host-user-group-membership.stderr.txt",
        '[ "$host_user_membership_status" -ne 67 ]',
        'grep -Fx "no $host_user is NOT a member of scratchbird"',
        "Hosted-runner service-group non-membership response was malformed.",
    )
    for token in required_tokens:
        require_token(block, token, rel)


def check_macos_launchd_credential_probe(
    text: str,
    rel: str,
    job_name: str,
    repo_root: Path,
) -> None:
    """Require a hosted proof of the final launchd process credential."""

    block = workflow_job_block(text, job_name, rel)
    required_tokens = (
        "sb_bootstrap_os_authority_tests",
        "build/macos-credential-probe/${{ matrix.arch }}",
        "--verify-installed-service-identity",
        "installed-service-identity.txt",
        "launchd-credential",
        "smoke_macos_launchd_credential.sh",
        "com.scratchbird.credential-probe",
        "macos_system_launchd_bootstrap_identity_mismatch",
        "macos_system_launchd_initgroups_mismatch",
        "macos_system_launchd_launcher_mismatch",
        "macos_system_launchd_working_directory_mismatch",
        "macos_system_launchd_stdout_mismatch",
        "macos_system_launchd_stderr_mismatch",
    )
    for token in required_tokens:
        require_token(block, token, rel)
    if 'data.get("UserName") != "scratchbird"' in block:
        fail(f"macos_stale_direct_service_identity_forbidden:{rel}:{job_name}")

    credential_smoke = (
        repo_root
        / "project"
        / "tools"
        / "installers"
        / "smoke_macos_launchd_credential.sh"
    )
    if not credential_smoke.is_file():
        fail("macos_launchd_credential_smoke_missing")
    smoke_text = credential_smoke.read_text(encoding="utf-8")
    for token in (
        "credential-probe",
        "launchctl bootstrap system",
        "launchd_service_process_credential=passed",
        "host_computed_authority_canaries=refused",
        "bootstrap_authority_regain=refused",
    ):
        require_token(smoke_text, token, credential_smoke.name)


def check_macos_real128_build_dependency(text: str, rel: str, job_name: str) -> None:
    block = workflow_job_block(text, job_name, rel)
    required_patterns = {
        "homebrew_boost": r"(?m)^\s+boost\s+\\?\s*$",
        "boost_root": r"BOOST_ROOT=\$\(brew --prefix boost\)",
        "boost_cmake_prefix": r"CMAKE_PREFIX_PATH=.*\$\(brew --prefix boost\)",
        "llvm_build_library": r"SB_LLVM_LIBRARY=\$\(brew --prefix llvm\)/lib/libLLVM[.]dylib",
        "llvm_runtime_library": r"SB_LLVM_RUNTIME_LIBRARY=\$\(brew --prefix llvm\)/lib/libLLVM[.]dylib",
        "llvm_runtime_load_proof": r"ctypes[.]CDLL\(sys[.]argv\[1\]\)",
        "llvm_build_argument": r"-DSB_LLVM_LIBRARY=\"\$SB_LLVM_LIBRARY\"",
        "llvm_runtime_argument": r"-DSB_LLVM_RUNTIME_LIBRARY=\"\$SB_LLVM_RUNTIME_LIBRARY\"",
    }
    for label, pattern in required_patterns.items():
        if re.search(pattern, block) is None:
            fail(f"macos_real128_dependency_missing:{rel}:{job_name}:{label}")


def check_macos_universal_smoke_llvm_runtime(
    text: str,
    rel: str,
    job_name: str,
) -> None:
    """Require the universal smoke host to supply its selected LLVM runtime.

    A universal macOS archive intentionally records one external Homebrew LLVM
    runtime path per architecture.  The smoke script resolves the map for the
    host architecture and performs a real ``ctypes.CDLL`` load, so this QA job
    must provision that host's LLVM package instead of skipping the runtime
    contract or hard-coding the other architecture's prefix.
    """

    block = workflow_job_block(text, job_name, rel)
    required_tokens = (
        "Install Homebrew LLVM runtime for universal smoke",
        "brew install llvm",
        'llvm_runtime="$(brew --prefix llvm)/lib/libLLVM.dylib"',
        'test -f "$llvm_runtime"',
        "ctypes.CDLL(sys.argv[1])",
        "smoke_install_macos.sh",
    )
    for token in required_tokens:
        require_token(block, token, rel)


def check_ctest_label_contract(
    text: str,
    rel: str,
    job_name: str,
    preset: str,
) -> None:
    block = workflow_job_block(text, job_name, rel)
    for label in ("public_release_correctness", "engine_listener_enterprise"):
        require_token(block, f"ctest --preset {preset} -L {label}", rel)
    for match in re.finditer(
        rf"(?m)^\s*ctest\s+--preset\s+{re.escape(preset)}(?:\s+.*)?$",
        block,
    ):
        if " -L " not in match.group(0):
            fail(f"ctest_unfiltered_preset_forbidden:{rel}:{job_name}:{preset}")


def check_linux_release_dependencies(text: str, rel: str, job_name: str) -> None:
    block = workflow_job_block(text, job_name, rel)
    for dependency in (
        "ninja-build",
        "clang-tidy-18",
        "cppcheck",
        "g++-mingw-w64-x86-64",
    ):
        if dependency not in block:
            fail(f"linux_release_dependency_missing:{rel}:{job_name}:{dependency}")
    check_ctest_label_contract(text, rel, job_name, "public-release-linux")


def check_windows_llvm_release_dependency(text: str, rel: str, job_name: str) -> None:
    block = workflow_job_block(text, job_name, rel)
    for dependency in (
        "mingw-w64-ucrt-x86_64-llvm",
        "mingw-w64-ucrt-x86_64-clang-tools-extra",
        "mingw-w64-ucrt-x86_64-cppcheck",
        "mingw-w64-ucrt-x86_64-boost",
    ):
        if dependency not in block:
            fail(f"windows_release_dependency_missing:{rel}:{job_name}:{dependency}")
    for discovery_token in (
        "LLVM_CONFIG=",
        "--cmakedir",
        "--libdir",
        "--version",
        "LLVM_REQUIRED_MAJOR=22",
        'test "$LLVM_MAJOR" -ge "$LLVM_REQUIRED_MAJOR"',
        "LLVM_BIN_DIR=",
        "libLLVM-${LLVM_MAJOR}.dll",
        '-DLLVM_DIR="$LLVM_CMAKE_DIR"',
        '-DSB_LLVM_LIBRARY="$LLVM_LIBRARY"',
        '-DSB_LLVM_RUNTIME_LIBRARY="$LLVM_RUNTIME_LIBRARY"',
        '-DSB_LLVM_MIN_MAJOR="$LLVM_REQUIRED_MAJOR"',
    ):
        if discovery_token not in block:
            fail(
                f"windows_llvm_discovery_missing:{rel}:{job_name}:"
                f"{discovery_token}"
            )
    if re.search(r"libLLVM[^\s'\"/\\]*[.]a\b", block):
        fail(f"windows_llvm_runtime_archive_forbidden:{rel}:{job_name}")
    check_ctest_label_contract(text, rel, job_name, "public-release-windows")


def check_windows_zip_only_release_path(text: str, rel: str, job_name: str) -> None:
    """Require the automated Windows release path to build and smoke only ZIP."""

    block = workflow_job_block(text, job_name, rel)
    for token in (
        "Build Windows portable ZIP package",
        "build_installers.py",
        "verify_installer_artifacts.py --platform windows",
        'Filter "scratchbird-windows-*.zip"',
        "Expected exactly one Windows ZIP package",
        "smoke_install_windows.ps1",
    ):
        require_token(block, token, rel)
    for token in (
        "Set up WiX and utility extension",
        "wix extension add -g",
        "dotnet tool install --global wix",
        "WixToolset.Util.wixext",
        "SB_WIX_TOOL_ROOT",
        "--require-msi",
        ".msi",
    ):
        if token in block:
            fail(f"windows_zip_only_forbidden_token:{rel}:{job_name}:{token}")


def check_nightly_publisher(text: str, rel: str, repo_root: Path) -> None:
    if re.search(r"(?m)^permissions:\s*\n\s+contents:\s+read\s*$", text) is None:
        fail(f"nightly_default_permissions_not_read_only:{rel}")
    if re.search(r"(?m)^\s*group:\s+scratchbird-rolling-nightly\s*$", text) is None:
        fail(f"nightly_constant_concurrency_missing:{rel}")
    if "nightly-installers-${{ github.ref }}" in text:
        fail(f"nightly_ref_scoped_concurrency_forbidden:{rel}")

    gold_block = workflow_job_block(text, "gold-gate", rel)
    for token in (
        "All-platform native nightly enablement gate",
        "SB_EVENT_NAME: ${{ github.event_name }}",
        "SB_NIGHTLY_ENABLED: ${{ vars.SB_NIGHTLY_INSTALLERS_ENABLED }}",
        '"$SB_EVENT_NAME" = "schedule"',
        "value=rolling-prerelease",
        "REQUESTED_VERSION:",
        '[[ ! "$REQUESTED_VERSION" =~ ^[0-9A-Za-z.+~_-]+$ ]]',
        'echo "value=$REQUESTED_VERSION"',
    ):
        require_token(gold_block, token, rel)

    publish_block = workflow_job_block(text, "publish-nightly", rel)
    for token in (
        "contents: write",
        "success()",
        "github.ref == 'refs/heads/main'",
        "name: scratchbird-linux-installers",
        "name: scratchbird-windows-installers",
        "name: scratchbird-macos-x86_64-installers",
        "name: scratchbird-macos-arm64-installers",
        "name: scratchbird-macos-universal-installers",
        "create_nightly_release_bundle.py",
        "sha256sum --check --strict",
        "publish_rolling_nightly.py",
        "Native ScratchBird platform only",
        "SBmgr, SBgate, SBParser using native SBSQL, and SBsrv",
        "compatibility parser or emulation packages",
        "native ScratchBird listener default is TCP port 3092",
        "default local-password policy pack plus charset, collation, timezone, and native SBSQL language resources",
        "LLVM is mandatory",
        "Public nightly assets are exact manifest-derived portable archives only: Linux tar.gz, Windows ZIP, and macOS tar.gz.",
        "DEB, AUR, RPM, PKG, and MSI system-installer formats are internal-only and are not downloadable nightly assets.",
        '--checkout-root "$GITHUB_WORKSPACE"',
    ):
        require_token(publish_block, token, rel)
    if "pattern:" in publish_block or "merge-multiple:" in publish_block:
        fail(f"nightly_exact_artifact_download_required:{rel}")
    for pattern, label in (
        (r"\bgh\s+release\s+delete\s+", "release_delete"),
        (r"gh\s+release\s+upload[^\n]*--clobber", "release_upload_clobber"),
        (r"find\s+nightly-artifacts\s+-type\s+f", "recursive_artifact_dump"),
        (r"pattern:\s+scratchbird-\*\s*$", "broad_artifact_download"),
        (r"scratchbird-[^\s]*-install-smoke", "smoke_artifact_download"),
    ):
        if re.search(pattern, publish_block, re.MULTILINE):
            fail(f"nightly_unsafe_publication_forbidden:{rel}:{label}")

    bundle_tool = repo_root / "project" / "tools" / "installers" / "create_nightly_release_bundle.py"
    publisher_tool = repo_root / "project" / "tools" / "installers" / "publish_rolling_nightly.py"
    contract_tool = repo_root / "project" / "tools" / "installers" / "nightly_release_contract.py"
    for path in (bundle_tool, publisher_tool, contract_tool):
        if not path.is_file():
            fail(f"nightly_tool_missing:{path.name}")
    bundle_text = bundle_tool.read_text(encoding="utf-8")
    for token in (
        "scratchbird.native_nightly_release.v1",
        "scratchbird_native_no_emulation",
        '"native_parser": "SBSQL"',
        '"native_components"',
        'NATIVE_COMPONENTS = ("SBmgr", "SBgate", "SBParser", "SBsrv")',
        "emulation_layers_included",
        "scratchbird-nightly-linux-x86_64.tar.gz",
        "scratchbird-nightly-windows-x86_64.zip",
        "scratchbird-nightly-macos-x86_64.tar.gz",
        "scratchbird-nightly-macos-arm64.tar.gz",
        "scratchbird-nightly-macos-universal.tar.gz",
        "verify_native_installed_payload.py",
        "macos_architecture_mismatch",
        "exact_manifest_derived_native_portable_archives_only",
        "PUBLIC_EXCLUDED_PACKAGE_FORMATS",
        "installer_publication_tree_mismatch",
        "installer_publication_directory_mismatch",
        "installer_publication_manifest_set_invalid",
        "public_system_package_verification_forbidden",
        "windows_release_policy",
        "RELEASE_CONTRACTS",
        "release_scope",
        "release_contract_package_inventory_mismatch",
        "installer_manifest_sha256_mismatch",
        "package_cardinality",
    ):
        require_token(bundle_text, token, bundle_tool.name)
    for forbidden in (
        "scratchbird-nightly-linux-x86_64.deb",
        "scratchbird-nightly-linux-x86_64.rpm",
        "scratchbird-nightly-linux-x86_64-aur.tar.gz",
        "scratchbird-nightly-macos-x86_64.pkg",
        "scratchbird-nightly-macos-arm64.pkg",
        "scratchbird-nightly-windows-x86_64.msi",
    ):
        if forbidden in bundle_text:
            fail(f"nightly_system_installer_bundle_contract_forbidden:{forbidden}")

    contract_text = contract_tool.read_text(encoding="utf-8")
    for token in (
        'scope="all"',
        'scope="linux"',
        'scope="windows"',
        'scope="macos"',
        'tag="nightly"',
        'tag="nightly-linux"',
        'tag="nightly-windows"',
        'tag="nightly-macos"',
        "scratchbird-nightly-linux-manifest.json",
        "scratchbird-nightly-windows-manifest.json",
        "scratchbird-nightly-macos-manifest.json",
        "scratchbird-nightly-windows-x86_64.zip",
        "scratchbird-nightly-linux-x86_64.tar.gz",
        "scratchbird-nightly-macos-x86_64.tar.gz",
        "scratchbird-nightly-macos-arm64.tar.gz",
        "scratchbird-nightly-macos-universal.tar.gz",
    ):
        require_token(contract_text, token, contract_tool.name)
    for forbidden in (
        "scratchbird-nightly-linux-x86_64.deb",
        "scratchbird-nightly-linux-x86_64.rpm",
        "scratchbird-nightly-linux-x86_64-aur.tar.gz",
        "scratchbird-nightly-macos-x86_64.pkg",
        "scratchbird-nightly-macos-arm64.pkg",
        "scratchbird-nightly-windows-x86_64.msi",
    ):
        if forbidden in contract_text:
            fail(f"nightly_system_installer_contract_forbidden:{forbidden}")

    publisher_text = publisher_tool.read_text(encoding="utf-8")
    for token in (
        'DEFAULT_CONTRACT = get_release_contract("all")',
        "release_scope",
        "self.contract.tag",
        "sb-nightly-incoming-",
        "sb-nightly-backup-",
        "rollback_swaps",
        "remote_asset_digest_mismatch",
        "release_manifest_native_components_mismatch",
        "existing_nightly_release_unmanaged",
        "existing_nightly_tag_unmanaged",
        "origin_repository_mismatch",
        "delete_current_run_draft_release",
        "is_current_run_owned_release",
        "is_current_run_owned_draft",
        "recovery_release_ownership_lost",
        "LegacyDraftSnapshot",
        "legacy_draft_snapshot",
        "delete_observed_legacy_draft",
        "legacy_draft_changed_before_cleanup",
        "canonical_asset_inventory_not_exact",
        "irreversible_cleanup_started",
        "coherent_draft_retry_required",
        "wait_for_tag_target(self.target_sha)",
        "tag_visibility_timeout",
        "API_READ_TRANSIENT_ATTEMPTS",
        "github_api_read_transient_timeout",
        'method == "GET"',
        "INITIAL_RELEASE_CREATE_ATTEMPTS",
        "is_retryable_initial_release_create_failure",
        "validate_initial_draft_release",
        "create_initial_draft_release",
        '"target_commitish": self.target_sha',
        '"make_latest": "false"',
        "Content-Type: application/json",
        "exact manifest-derived portable archives only",
        "REQUIRED_ARTIFACT_VERIFICATION",
        '"draft": True',
        "draft=False,",
        "rolling_release_immutable_or_state_unknown",
        '"--method", "PATCH"',
        "Never delete backup-prefixed assets",
        "MANAGED_RELEASE_MARKER_SCHEMA",
        "MANAGED_RELEASE_AUTHOR",
        "predecessor_tag",
        "list_releases",
        "get_release_by_id",
        "discover_release_state",
        "release_marker",
        "contract_managed_marker_identity",
        "legacy_draft_provenance",
        "same_tag_draft_unmanaged",
        "same_tag_draft_owned_by_other_run",
        "reconcile_initial_create_response",
        "patch_release",
        "release_asset_upload_endpoint",
        "upload_incoming_asset",
        "RELEASE_ASSET_UPLOAD_ATTEMPTS",
        '"--config"',
        '"--disable"',
        "Content-Type: application/octet-stream",
        "os.fchmod(handle.fileno(), 0o600)",
        "curl_config.unlink(missing_ok=True)",
        "github_token_invalid_for_curl_config",
        "marker_provenance_mismatch",
        "draft_created_this_attempt",
        "resumed_existing_draft",
        "verify_canonical_inventory(release_id, assets, exact=True)",
        "--force-with-lease=refs/tags/",
        "tag_expected_sha",
        "restore_tag_if_transaction_owned",
        "tag_drift_during_recovery",
        "current_run_draft_tag_provenance_mismatch",
        "existing_nightly_tag_provenance_mismatch",
        "published_release_state_mismatch",
    ):
        require_token(publisher_text, token, publisher_tool.name)
    if "--clobber" in publisher_text:
        fail(f"nightly_publisher_clobber_forbidden:{publisher_tool.name}")
    for pattern, label in (
        (r'self[.]gh\(\s*"release"', "gh_release_subcommand"),
        (r'"release",\s*"(?:upload|edit|download|create|delete)"', "gh_release_operation"),
        (r"/releases/tags/", "tag_endpoint_for_drafts"),
        (r"--location(?:-trusted)?", "curl_redirect_following"),
        (r"--oauth2-bearer", "curl_token_argv"),
        (r"--force(?!-with-lease)", "unleased_tag_force"),
    ):
        if re.search(pattern, publisher_text):
            fail(f"nightly_publisher_unsafe_release_surface:{publisher_tool.name}:{label}")


def check_platform_nightly_workflow(text: str, rel: str, scope: str) -> None:
    """Require independent build/publication runs for each native platform."""

    check_push_to_main_trigger(text, rel)
    expected_group = f"scratchbird-rolling-nightly-{scope}"
    if re.search(rf"(?m)^\s*group:\s+{re.escape(expected_group)}\s*$", text) is None:
        fail(f"platform_nightly_concurrency_missing:{rel}:{scope}")
    gold_block = workflow_job_block(text, "gold-gate", rel)
    scope_label = {"linux": "Linux", "windows": "Windows", "macos": "macOS"}[scope]
    for token in (
        f"{scope_label} native nightly enablement gate",
        "SB_NIGHTLY_INSTALLERS_ENABLED",
        '"$SB_EVENT_NAME" = "schedule"',
        "value=rolling-prerelease",
        "REQUESTED_VERSION:",
    ):
        require_token(gold_block, token, rel)
    build_block = workflow_job_block(text, "build-installers", rel)
    for token in (
        "verify-installers.yml",
        f"platform: {scope}",
    ):
        require_token(build_block, token, rel)
    if "require-msi" in build_block:
        fail(f"platform_nightly_windows_msi_request_forbidden:{rel}:{scope}")
    if "platform: all" in build_block:
        fail(f"platform_nightly_combined_build_forbidden:{rel}:{scope}")
    publish_block = workflow_job_block(text, "publish-nightly", rel)
    for token in (
        "contents: write",
        "success()",
        "github.ref == 'refs/heads/main'",
        "publish-platform-nightly.yml",
        f"release_scope: {scope}",
        "source_revision: ${{ github.sha }}",
        "source_run_id: ${{ github.run_id }}",
        "source_run_attempt: ${{ github.run_attempt }}",
    ):
        require_token(publish_block, token, rel)


def check_platform_nightly_publisher(text: str, rel: str) -> None:
    """Keep platform publication exact, scoped, and independent of other OSes."""

    if "workflow_call:" not in text:
        fail(f"platform_publisher_not_reusable:{rel}")
    if "workflow_dispatch:" in text or re.search(r"(?m)^  push:\s*$", text):
        fail(f"platform_publisher_direct_trigger_forbidden:{rel}")
    if re.search(r"(?m)^permissions:\s*\n\s+contents:\s+write\s*$", text) is None:
        fail(f"platform_publisher_write_permission_missing:{rel}")
    for token in (
        "release_scope:",
        "source_revision:",
        "source_run_id:",
        "source_run_attempt:",
        "linux|windows|macos",
        "github.ref == 'refs/heads/main'",
        'test "$GITHUB_REF" = "refs/heads/main"',
        'test "$SB_SOURCE_REVISION" = "$GITHUB_SHA"',
        'test "$SB_SOURCE_RUN_ID" = "$GITHUB_RUN_ID"',
        'test "$SB_SOURCE_RUN_ATTEMPT" = "$GITHUB_RUN_ATTEMPT"',
        "ref: ${{ github.sha }}",
        "actions/download-artifact@v4",
        "name: scratchbird-linux-installers",
        "name: scratchbird-windows-installers",
        "name: scratchbird-macos-x86_64-installers",
        "name: scratchbird-macos-arm64-installers",
        "name: scratchbird-macos-universal-installers",
        "verify_installer_artifacts.py",
        "create_nightly_release_bundle.py",
        '--release-scope "$SB_RELEASE_SCOPE"',
        "scratchbird-nightly-$SB_RELEASE_SCOPE-SHA256SUMS",
        "publish_rolling_nightly.py",
        "headless service account only",
        "native ScratchBird listener default is TCP port 3092",
        'not the complete cross-platform `nightly` release',
    ):
        require_token(text, token, rel)
    if "pattern:" in text or "merge-multiple:" in text:
        fail(f"platform_nightly_exact_artifact_download_required:{rel}")
    for pattern, label in (
        (r"\bgh\s+release\s+delete\s+", "release_delete"),
        (r"gh\s+release\s+upload[^\n]*--clobber", "release_upload_clobber"),
        (r"pattern:\s+scratchbird-\*\s*$", "broad_artifact_download"),
        (r"scratchbird-[^\s]*-install-smoke", "smoke_artifact_download"),
    ):
        if re.search(pattern, text, re.MULTILINE):
            fail(f"platform_nightly_unsafe_publication_forbidden:{rel}:{label}")


def check_native_release_stage(
    text: str,
    rel: str,
    job_name: str,
    platform: str,
    require_installer: bool,
    retain_native_proof_artifacts: bool = False,
) -> None:
    block = workflow_job_block(text, job_name, rel)
    public_cleanup = (
        f"cmake -E remove_directory build/public-release-{platform}"
        if require_installer and not retain_native_proof_artifacts
        else f"cmake -E remove_directory build/public-release-{platform}/output/{platform}"
    )
    for token in (
        "stage_native_release_bundle.py",
        "verify_native_release_bundle.py",
        f"build/native-release-{platform}/output/{platform}",
        public_cleanup,
    ):
        require_token(block, token, rel)
    if platform == "windows":
        require_token(block, "--runtime-search-root", rel)
        require_token(block, "cygpath -w", rel)
    if require_installer:
        require_token(block, "--require-native-only", rel)
        if not retain_native_proof_artifacts:
            require_token(
                block,
                f"cmake -E remove_directory build/native-release-{platform}/output/{platform}",
                rel,
            )
        if re.search(
            rf"--artifact-root\s+build/public-release-{re.escape(platform)}/output/{re.escape(platform)}",
            block,
        ):
            fail(f"raw_proof_output_packaging_forbidden:{rel}:{job_name}:{platform}")


def check_mkdtemp_header_contract(repo_root: Path) -> None:
    """Require every direct POSIX temporary-directory caller to include unistd.

    Apple documents mkdtemp in unistd.h.  Relying on a transitive declaration
    happens to work with some libc implementations but fails with current
    macOS SDK headers, so enforce the source-level dependency before platform
    builds begin.
    """

    tests_root = repo_root / "project" / "tests"
    for source in sorted(tests_root.rglob("*.cpp")):
        text = source.read_text(encoding="utf-8")
        if "::mkdtemp(" not in text:
            continue
        if re.search(r"(?m)^#include <unistd[.]h>\s*$", text) is None:
            fail(
                "mkdtemp_unistd_header_missing:"
                f"{source.relative_to(repo_root).as_posix()}"
            )


def check_native_installer_admission_contract(repo_root: Path) -> None:
    """Keep the portable-only public admission boundary fail-closed."""

    bundle = repo_root / "project/tools/installers/create_nightly_release_bundle.py"
    verifier = repo_root / "project/tools/installers/verify_installer_artifacts.py"
    admission = repo_root / "project/tools/installers/installer_native_admission.py"
    universal = repo_root / "project/tools/installers/make_macos_universal.py"
    failure_proof = repo_root / "project/tools/release/stage_textual_failure_proof.py"
    failure_proof_test = repo_root / "project/tests/release/textual_failure_proof_test.py"
    release_cmake = repo_root / "project/tests/release/CMakeLists.txt"
    for path in (
        bundle,
        verifier,
        admission,
        universal,
        failure_proof,
        failure_proof_test,
        release_cmake,
    ):
        if not path.is_file():
            fail(f"native_installer_admission_tool_missing:{path.name}")
    required = {
        bundle: (
            "installer_native_admission as native_admission",
            "verify_selected_package_native_admission",
            "PUBLIC_EXCLUDED_PACKAGE_FORMATS",
            "installer_publication_tree_mismatch",
            "installer_publication_directory_mismatch",
            "installer_publication_manifest_set_invalid",
            "public_system_package_verification_forbidden",
        ),
        verifier: (
            "verify_native_admission_packages",
            "--materialize-public-root",
            "materialize_publication_root",
            "assert_exact_publication_tree",
            "PUBLICATION_SURFACE_SCHEMA",
        ),
        admission: (
            "NATIVE_POLICY",
            "CLIENT_IDENTITY_MARKERS",
            "APPROVED_NONPAYLOAD_CLIENT_MARKER_PATHS",
            "driver_route_smoke.sh",
            "verify_portable_native_payload",
            "native_admission_client_payload_forbidden",
            "validate_payload_layout",
            "verify_declared_installed_payload",
            "native_admission_payload_layout_forbidden",
            "native_admission_payload_install_manifest_tree_mismatch",
        ),
        universal: (
            "assert_exact_publication_root",
            "verify_portable_native_payload",
            "universal_native_payload_reverification_failed",
        ),
        failure_proof: (
            "utf8_text_only_no_extracted_payload_or_binary_tree",
            "PAYLOAD_PATH_COMPONENTS",
            "FAILURE_PROOF_MANIFEST.json",
        ),
        failure_proof_test: (
            "result.log",
            "\\x7fELF",
            "archive",
            "extract",
            "symlink",
            "oversized.log",
            "MAX_TEXTUAL_PROOF_BYTES",
        ),
    }
    for path, tokens in required.items():
        source = path.read_text(encoding="utf-8")
        for token in tokens:
            require_token(source, token, path.name)
    for path in (bundle, verifier, admission):
        source = path.read_text(encoding="utf-8")
        for forbidden in (
            "native_admission_receipt",
            "receipt_for_opaque_system_package",
            "validate_opaque_system_package_receipt",
            "--write-native-admission-receipts",
        ):
            if forbidden in source:
                fail(f"native_installer_stale_receipt_path_forbidden:{path.name}:{forbidden}")
    require_token(
        release_cmake.read_text(encoding="utf-8"),
        "textual_failure_proof_test.py",
        release_cmake.name,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    workflow_root = repo_root / ("." + "github") / "workflows"
    if not workflow_root.is_dir():
        fail("workflow_root_missing")
    check_mkdtemp_header_contract(repo_root)
    check_native_installer_admission_contract(repo_root)
    check_workflow_inventory(workflow_root)
    for name, tokens in REQUIRED_WORKFLOWS.items():
        path = workflow_root / name
        if not path.is_file():
            fail(f"workflow_missing:{name}")
        text = path.read_text(encoding="utf-8")
        check_permissions(text, name)
        check_unadmitted_component_publication(text, name)
        check_portable_only_public_workflow(text, name)
        check_failure_uploads_are_textual(text, name)
        for token in tokens:
            require_token(text, token, name)
        if name == "ci-macos.yml":
            check_main_push_ci_policy(
                text, name, "public-release-macos", "SB_MACOS_CI_ENABLED"
            )
            check_macos_system_installer_failure_diagnostics(
                text, name, "public-release-macos"
            )
            check_macos_system_payload_verifier_authority(
                text, name, "public-release-macos"
            )
            check_macos_real128_build_dependency(text, name, "public-release-macos")
            check_macos_nonmember_probe(text, name, "public-release-macos")
            check_macos_launchd_credential_probe(
                text, name, "public-release-macos", repo_root
            )
            check_macos_universal_smoke_llvm_runtime(
                text, name, "macos-universal-artifact"
            )
            check_native_release_stage(
                text,
                name,
                "public-release-macos",
                "macos",
                True,
                retain_native_proof_artifacts=True,
            )
            check_public_installer_materialization(
                text,
                name,
                "public-release-macos",
                "macos",
                "Upload macOS installers",
                "build/public-installers/macos-${{ matrix.arch }}",
            )
            check_public_universal_materialization(
                text,
                name,
                "macos-universal-artifact",
                "build/public-installers/macos-universal",
            )
        elif name == "ci-linux.yml":
            check_main_push_ci_policy(
                text, name, "public-release-linux", "SB_LINUX_CI_ENABLED"
            )
            check_linux_release_dependencies(text, name, "public-release-linux")
            check_native_release_stage(
                text, name, "public-release-linux", "linux", False
            )
        elif name == "ci-windows.yml":
            check_main_push_ci_policy(
                text, name, "public-release-windows", "SB_WINDOWS_CI_ENABLED"
            )
            check_windows_llvm_release_dependency(
                text, name, "public-release-windows"
            )
            check_native_release_stage(
                text, name, "public-release-windows", "windows", False
            )
        elif name == "verify-installers.yml":
            check_installer_reusable_policy(text, name)
            check_linux_privileged_bootstrap_smoke(text, name)
            check_macos_system_installer_failure_diagnostics(
                text, name, "macos-installers"
            )
            check_macos_system_payload_verifier_authority(
                text, name, "macos-installers"
            )
            check_macos_nonmember_probe(text, name, "macos-installers")
            check_macos_launchd_credential_probe(
                text, name, "macos-installers", repo_root
            )
            check_macos_real128_build_dependency(text, name, "macos-installers")
            check_macos_universal_smoke_llvm_runtime(
                text, name, "macos-universal-installers"
            )
            check_linux_release_dependencies(text, name, "linux-installers")
            check_windows_llvm_release_dependency(text, name, "windows-installers")
            check_windows_zip_only_release_path(text, name, "windows-installers")
            check_ctest_label_contract(
                text, name, "macos-installers", "public-release-macos"
            )
            check_native_release_stage(
                text, name, "linux-installers", "linux", True
            )
            check_native_release_stage(
                text, name, "windows-installers", "windows", True
            )
            check_native_release_stage(
                text, name, "macos-installers", "macos", True
            )
            for job_name, platform, step_name, artifact_path in (
                (
                    "linux-installers",
                    "linux",
                    "Upload Linux installers",
                    "build/public-installers/linux",
                ),
                (
                    "windows-installers",
                    "windows",
                    "Upload Windows installers",
                    "build/public-installers/windows",
                ),
                (
                    "macos-installers",
                    "macos",
                    "Upload macOS installers",
                    "build/public-installers/macos-${{ matrix.arch }}",
                ),
            ):
                check_public_installer_materialization(
                    text, name, job_name, platform, step_name, artifact_path
                )
            check_public_universal_materialization(
                text,
                name,
                "macos-universal-installers",
                "build/public-installers/macos-universal",
            )
        elif name == "nightly-installers.yml":
            check_nightly_publisher(text, name, repo_root)
        elif name == "nightly-linux-installers.yml":
            check_platform_nightly_workflow(text, name, "linux")
        elif name == "nightly-macos-installers.yml":
            check_platform_nightly_workflow(text, name, "macos")
        elif name == "nightly-windows-installers.yml":
            check_platform_nightly_workflow(text, name, "windows")
        elif name == "publish-platform-nightly.yml":
            check_platform_nightly_publisher(text, name)
        elif name == "release-candidate.yml":
            check_main_only_public_dispatch(
                text,
                name,
                "build-installers",
                "publish",
                "Require protected-main release context",
            )
        elif name == "webserver-package-export.yml":
            check_webserver_export_native_boundary(text, name)
            check_main_only_public_dispatch(
                text,
                name,
                "build-packages",
                "package-webserver-export",
                "Require protected-main export context",
            )
        for token in FORBIDDEN_TOKENS:
            if token in text:
                fail(f"forbidden_token:{name}:{token}")
    dependabot = repo_root / ("." + "github") / "dependabot.yml"
    if not dependabot.is_file():
        fail("dependabot_missing")
    dependabot_text = dependabot.read_text(encoding="utf-8")
    for token in ("package-ecosystem: github-actions", "directory: /"):
        require_token(dependabot_text, token, "dependabot.yml")
    print("github_actions_static_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
