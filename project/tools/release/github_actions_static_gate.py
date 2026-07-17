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
    ),
    "verify-installers.yml": (
        "workflow_dispatch:",
        "build_installers.py",
        "verify_installer_artifacts.py",
        "smoke_install_linux.sh",
        "smoke_install_windows.ps1",
        "smoke_install_macos.sh",
        "make_macos_universal.py",
        "public-release-macos",
    ),
    "nightly-installers.yml": (
        "schedule:",
        "workflow_dispatch:",
        "SB_NIGHTLY_INSTALLERS_ENABLED",
        "build_installers.py",
        "verify_installer_artifacts.py",
        "pattern: scratchbird-*-installers",
        "create_nightly_release_bundle.py",
        "publish_rolling_nightly.py",
        "scratchbird-nightly-SHA256SUMS",
        "macos",
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


def fail(message: str) -> None:
    print(f"github_actions_static_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require_token(text: str, token: str, rel: str) -> None:
    if token not in text:
        fail(f"missing_token:{rel}:{token}")


def check_permissions(text: str, rel: str) -> None:
    if "permissions:" not in text:
        fail(f"missing_permissions:{rel}")
    if re.search(r"contents:\s+write", text) and "release-candidate" not in rel and "nightly-installers" not in rel:
        fail(f"unexpected_contents_write:{rel}")
    if rel == "webserver-package-export.yml":
        for token in ("contents: write", "gh release", "git tag", "git push origin"):
            if token in text:
                fail(f"webserver_export_publication_token_forbidden:{token}")


def workflow_job_block(text: str, job_name: str, rel: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(job_name)}:\s*\n.*?(?=^  [A-Za-z0-9_-]+:\s*$|\Z)",
        text,
    )
    if match is None:
        fail(f"workflow_job_missing:{rel}:{job_name}")
    return match.group(0)


def workflow_step_block(block: str, step_name: str, rel: str, job_name: str) -> str:
    match = re.search(
        rf"(?ms)^      - name: {re.escape(step_name)}\s*\n.*?(?=^      - name: |\Z)",
        block,
    )
    if match is None:
        fail(f"workflow_step_missing:{rel}:{job_name}:{step_name}")
    return match.group(0)


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


def check_macos_system_installed_verifier(
    text: str,
    rel: str,
    job_name: str,
    step_name: str,
) -> None:
    job = workflow_job_block(text, job_name, rel)
    for diagnostic_token in ("/var/log/install.log", "tail -n 4000"):
        require_token(job, diagnostic_token, rel)
    step = workflow_step_block(job, step_name, rel, job_name)
    smoke = "project/tools/installers/smoke_install_macos.sh"
    installer = "sudo installer -pkg"
    verifier = "project/tools/release/verify_native_installed_payload.py"
    mode = "--config-mode system-installed"
    inventory = "sudo find /var/lib/scratchbird -xdev -type f -print"
    membership_check = "sudo dseditgroup -n . -o checkmember"
    membership_proof = "host-user-group-membership.txt"
    launchd_probe = "smoke_macos_launchd_credential.sh"
    launchd_proof = "launchd-service-credential.txt"
    installer_status = 'installer_status=${PIPESTATUS[0]}'
    failure_identity = "installer-failure-service-identity"
    resolved_group_ids = "resolved-group-ids.txt"
    resolved_group_names = "resolved-group-names.txt"
    forbidden_inventory = "macos-system-forbidden-artifacts.txt"
    inventory_proof = "macos-system-database-security-proof.txt"
    for token in (
        smoke,
        installer,
        verifier,
        'python_bin="$(command -v python3)"',
        'verifier_root="/tmp/scratchbird-native-verifier-',
        "project/tools/release/stage_native_release_bundle.py",
        'sudo install -d -o root -g scratchbird -m 0750 "$verifier_root"',
        'sudo -u scratchbird -g scratchbird "$python_bin"',
        '"$verifier_root/verify_native_installed_payload.py"',
        "cd /tmp",
        'sudo rm -rf "$verifier_root"',
        'sudo test ! -L "$verifier_root"',
        'if [ -e "$existing_path" ] || [ -L "$existing_path" ]',
        mode,
        inventory,
        forbidden_inventory,
        inventory_proof,
        membership_check,
        membership_proof,
        launchd_probe,
        launchd_proof,
        "launchd_service_process_credential=passed",
        "supplementary_group_policy=exact_scratchbird_only",
        "host_computed_authority_canaries=refused",
        "host_computed_authority_canary_count",
        "bootstrap_authority_regain=refused",
        "com.scratchbird.credential-probe",
        'sudo launchctl bootout "system/$label"',
        'data.get("InitGroups") is not False',
        "host_computed_directory_groups_not_copied_into_service_process",
        "service_authentication_authority_present",
        "service_shadow_hash_data_present",
        "service_user_record=",
        "dsAttrType(Native|Standard)",
        "AuthenticationAuthority",
        "ShadowHashData",
        'test "$service_password" = \'*\'',
        "authentication_authority_existing_account=refused",
        ";ScratchBirdRegressionAuth;",
        installer_status,
        failure_identity,
        resolved_group_ids,
        resolved_group_names,
        "sudo id -G scratchbird",
        "sudo id -Gn scratchbird",
        "-iname '*.sbdb'",
        "-iname '*.sbrd'",
        "-iname '*security_principal_events*'",
        "-iname '*local_password_auth*'",
        'if [ -s "$forbidden_path" ]',
        'echo "forbidden_matches=0"',
    ):
        require_token(step, token, rel)
    for forbidden in (
        "dscl . -read /Users/scratchbird AuthenticationAuthority",
        "dscl . -read /Users/scratchbird ShadowHashData",
    ):
        if forbidden in step:
            fail(
                f"macos_dscl_missing_attribute_exit_status_forbidden:"
                f"{rel}:{job_name}:{forbidden}"
            )
    verifier_offset = step.index(verifier)
    mode_offset = step.find(mode, verifier_offset)
    inventory_offset = step.find(inventory, mode_offset)
    if not (
        step.index(smoke)
        < step.index(installer)
        < step.index(installer_status)
        < step.index(failure_identity)
        < verifier_offset
        < mode_offset
        < inventory_offset
    ):
        fail(f"macos_system_verifier_order_invalid:{rel}:{job_name}")
    root_probe_offset = step.index("--verify-installed-service-identity")
    launchd_probe_offset = step.index(launchd_probe, root_probe_offset)
    identity_proof_offset = step.index("macos-system-identity.txt", launchd_probe_offset)
    if not root_probe_offset < launchd_probe_offset < identity_proof_offset:
        fail(f"macos_launchd_credential_probe_order_invalid:{rel}:{job_name}")
    membership_offset = step.index(membership_check)
    membership_proof_offset = step.index(membership_proof, membership_offset)
    tolerant_status_offset = step.find("2>&1 || true", membership_proof_offset)
    membership_result_offset = step.find("if grep -Eq '^yes", membership_proof_offset)
    if not (
        membership_proof_offset
        < tolerant_status_offset
        < membership_result_offset
    ):
        fail(f"macos_nonmember_status_handling_invalid:{rel}:{job_name}")


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


def check_linux_installed_authority_probe(
    text: str, rel: str, job_name: str
) -> None:
    job = workflow_job_block(text, job_name, rel)
    stage = workflow_step_block(
        job, "Stage and verify native-only SB bundle", rel, job_name
    )
    source = (
        "build/public-release-linux/output/linux/bin/"
        "sb_bootstrap_os_authority_tests"
    )
    retained = (
        "build/bootstrap-probes/sb_bootstrap_os_authority_tests-linux"
    )
    cleanup = "cmake -E remove_directory build/public-release-linux"
    for token in (source, retained, "chmod 0755", cleanup):
        require_token(stage, token, rel)
    if not stage.index(source) < stage.index(retained) < stage.index(cleanup):
        fail(f"linux_authority_probe_retention_order_invalid:{rel}:{job_name}")

    smoke = workflow_step_block(
        job, "Smoke Linux system-package lifecycle", rel, job_name
    )
    for token in (
        "smoke_install_linux_system.py",
        "--run-privileged-deb-install",
        "--authority-probe",
        "$GITHUB_WORKSPACE/build/bootstrap-probes/"
        "sb_bootstrap_os_authority_tests-linux",
    ):
        require_token(smoke, token, rel)


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
        "pattern: scratchbird-*-installers",
        "merge-multiple: false",
        "create_nightly_release_bundle.py",
        "sha256sum --check --strict",
        "publish_rolling_nightly.py",
        "Native ScratchBird platform only",
        "SBmgr, SBgate, SBParser using native SBSQL, and SBsrv",
        "compatibility parser or emulation packages",
        "native ScratchBird listener default is TCP port 3092",
        "default local-password policy pack plus charset, collation, timezone, and native SBSQL language resources",
        "LLVM is mandatory",
        "fully inspected portable tar.gz/ZIP bundles",
        "DEB, RPM, AUR, PKG, and MSI packages remain workflow artifacts",
        '--checkout-root "$GITHUB_WORKSPACE"',
    ):
        require_token(publish_block, token, rel)
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
    for path in (bundle_tool, publisher_tool):
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
        "fully_extracted_and_exact_native_payload_verified_portable_archives_only",
        "expected_count = 7",
        "installer_manifest_sha256_mismatch",
        "package_cardinality",
    ):
        require_token(bundle_text, token, bundle_tool.name)
    for forbidden_name in (
        "scratchbird-nightly-linux-amd64.deb",
        "scratchbird-nightly-linux-x86_64.rpm",
        "scratchbird-nightly-linux-x86_64-aur.tar.gz",
        "scratchbird-nightly-windows-x86_64.msi",
        "scratchbird-nightly-macos-x86_64.pkg",
        "scratchbird-nightly-macos-arm64.pkg",
    ):
        if forbidden_name in bundle_text:
            fail(f"nightly_unverified_system_package_forbidden:{forbidden_name}")

    publisher_text = publisher_tool.read_text(encoding="utf-8")
    for token in (
        'TAG = "nightly"',
        "sb-nightly-incoming-",
        "sb-nightly-backup-",
        "rollback_swaps",
        "remote_asset_digest_mismatch",
        "release_manifest_native_components_mismatch",
        "existing_nightly_release_unmanaged",
        "existing_nightly_tag_unmanaged",
        "edit_release_snapshot(release, draft_override=True)",
        "origin_repository_mismatch",
        '"--repo"',
        "delete_new_draft_release",
        "canonical_asset_inventory_not_exact",
        "irreversible_cleanup_started",
        "coherent_draft_retry_required",
        "verify_canonical_inventory(assets, exact=True)",
        "published_tag_revision_mismatch",
        '"--draft"',
        'f"--draft={',
        "draft=False,",
        "rolling_release_immutable_or_state_unknown",
        '"--method", "PATCH"',
        "Never delete backup-prefixed assets",
    ):
        require_token(publisher_text, token, publisher_tool.name)
    if "--clobber" in publisher_text:
        fail(f"nightly_publisher_clobber_forbidden:{publisher_tool.name}")
    if re.search(r"\bgh\s+release\s+delete\s+", publisher_text):
        fail(f"nightly_publisher_release_delete_forbidden:{publisher_tool.name}")


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
        require_token(block, '"${MINGW_PREFIX:-/ucrt64}/bin"', rel)
        require_token(block, "shell: msys2 {0}", rel)
        stage_block = workflow_step_block(
            block, "Stage and verify native-only SB bundle", rel, job_name
        )
        stage_shell = re.search(r"(?m)^        shell:\s*(.+?)\s*$", stage_block)
        if stage_shell is not None and stage_shell.group(1) != "msys2 {0}":
            fail(
                "windows_native_stage_shell_mismatch:"
                f"{rel}:{job_name}:{stage_shell.group(1)}"
            )
    if require_installer:
        require_token(block, "--require-native-only", rel)
        if platform == "windows":
            installer_block = workflow_step_block(
                block, "Build installers", rel, job_name
            )
            wix_path_bridge = (
                'export PATH="$(cygpath -u "$USERPROFILE")/.dotnet/tools:$PATH"'
            )
            wix_path_probe = "command -v wix"
            for token in (wix_path_bridge, wix_path_probe):
                require_token(installer_block, token, rel)
            builder = "python project/tools/installers/build_installers.py"
            require_token(installer_block, builder, rel)
            if not (
                installer_block.index(wix_path_bridge)
                < installer_block.index(wix_path_probe)
                < installer_block.index(builder)
            ):
                fail(f"windows_wix_path_bridge_order_invalid:{rel}:{job_name}")
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    workflow_root = repo_root / ("." + "github") / "workflows"
    if not workflow_root.is_dir():
        fail("workflow_root_missing")
    for name, tokens in REQUIRED_WORKFLOWS.items():
        path = workflow_root / name
        if not path.is_file():
            fail(f"workflow_missing:{name}")
        text = path.read_text(encoding="utf-8")
        check_permissions(text, name)
        for token in tokens:
            require_token(text, token, name)
        if name == "ci-macos.yml":
            check_macos_real128_build_dependency(text, name, "public-release-macos")
            check_macos_system_installed_verifier(
                text,
                name,
                "public-release-macos",
                "Smoke macOS system package install and cleanup",
            )
            check_native_release_stage(
                text,
                name,
                "public-release-macos",
                "macos",
                True,
                retain_native_proof_artifacts=True,
            )
        elif name == "ci-linux.yml":
            check_linux_release_dependencies(text, name, "public-release-linux")
            check_native_release_stage(
                text, name, "public-release-linux", "linux", False
            )
        elif name == "ci-windows.yml":
            check_windows_llvm_release_dependency(
                text, name, "public-release-windows"
            )
            check_native_release_stage(
                text, name, "public-release-windows", "windows", False
            )
        elif name == "verify-installers.yml":
            check_macos_real128_build_dependency(text, name, "macos-installers")
            check_macos_system_installed_verifier(
                text,
                name,
                "macos-installers",
                "Smoke system package install and cleanup",
            )
            check_linux_release_dependencies(text, name, "linux-installers")
            check_linux_installed_authority_probe(
                text, name, "linux-installers"
            )
            check_windows_llvm_release_dependency(text, name, "windows-installers")
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
        elif name == "nightly-installers.yml":
            check_nightly_publisher(text, name, repo_root)
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
