#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static release preflight for the ScratchBird DBeaver management adapter.

The gate validates the DBeaver-specific release controller inputs that can be
checked without launching DBeaver: supported-version policy, API drift anchors,
installer lifecycle fixtures, license/IP boundaries, and runtime network egress
policy. Release mode can additionally require built package artifacts. This
preflight does not prove live-server, stock-GUI, manual-QA, or mutation
apply/verify closure by itself.
"""

from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Iterable
import xml.etree.ElementTree as ET
import zipfile


SCHEMA_ID = "scratchbird.dbeaver_management.release_contract.v1"
FIXTURE_SCHEMA_ID = "scratchbird.dbeaver_management.install_lifecycle_fixture.v1"
REPORT_SCHEMA_ID = "scratchbird.dbeaver_management.release_gate_report.v1"
ADAPTER_REL = Path("project/drivers/adaptor/scratchbird-dbeaver-driver")
DEFAULT_CONTRACT_REL = Path("project/tools/release/dbeaver_management_release_contract.json")
DEFAULT_FIXTURE_REL = Path(
    "project/tests/release/dbeaver_management_install_lifecycle_fixture.json"
)
DEFAULT_REPORT_REL = Path("build/reports/dbeaver_management_platform_gate.json")

SUPPORTED_WORKPLAN_IDS = (
    "DBEAVER-MGMT-003",
    "DBEAVER-MGMT-006",
    "DBEAVER-MGMT-023",
    "DBEAVER-MGMT-026",
    "DBEAVER-MGMT-027",
    "DBEAVER-MGMT-028",
    "DBEAVER-MGMT-029",
    "DBEAVER-MGMT-034",
    "DBEAVER-MGMT-043",
    "DBEAVER-MGMT-044",
    "DBEAVER-MGMT-046",
)

ISSUE_WORKPLAN_PREFIX_IDS = {
    "version_matrix": ("DBEAVER-MGMT-026", "DBEAVER-MGMT-046"),
    "api_drift": ("DBEAVER-MGMT-028", "DBEAVER-MGMT-046"),
    "install_lifecycle": (
        "DBEAVER-MGMT-023",
        "DBEAVER-MGMT-027",
        "DBEAVER-MGMT-043",
        "DBEAVER-MGMT-046",
    ),
    "secure_properties": ("DBEAVER-MGMT-006", "DBEAVER-MGMT-029", "DBEAVER-MGMT-046"),
    "license_ip": ("DBEAVER-MGMT-034", "DBEAVER-MGMT-046"),
    "network_egress": ("DBEAVER-MGMT-044", "DBEAVER-MGMT-046"),
    "release_artifacts": ("DBEAVER-MGMT-023", "DBEAVER-MGMT-046"),
}

REQUIRED_LIFECYCLE_PHASES = {
    "p2_update_site_build",
    "stock_bundle_build",
    "source_checkout_install",
    "stock_install",
    "upgrade_replace_existing",
    "downgrade_refusal",
    "stock_uninstall",
    "stock_reinstall",
    "workspace_profile_cleanup",
    "driver_cache_cleanup",
    "secret_cleanup",
}

REQUIRED_MIGRATION_CASES = {
    "old_plugin_id",
    "old_preference_key",
    "backup_restore",
    "corrupted_preferences",
    "reinstall_recovery",
    "redaction_safe_cleanup",
}

DEFAULT_SENSITIVE_PROVIDER_PROPERTIES = {
    "manager_auth_token",
    "auth_token",
    "auth_method_payload",
    "auth_payload_json",
    "auth_payload_b64",
    "workload_identity_token",
    "proxy_principal_assertion",
    "dormant_reattach_token",
}

FORBIDDEN_PRIVATE_FRAGMENTS = (
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

FORBIDDEN_REQUIRED_PROOF_TOKENS = (
    "-DskipTests",
    "-Dmaven.test.skip",
    "-DskipITs",
    "<skipTests>true",
    "<skipITs>true",
    "maven.test.skip=true",
)

ACCEPTED_PROOF_STATUSES = {"pass", "passed", "verified", "complete", "completed"}
REJECTED_PROOF_STATUSES = {
    "skip",
    "skipped",
    "xfail",
    "xfailed",
    "waived",
    "waiver",
    "deferred",
    "pending",
    "todo",
    "not_run",
    "not_required",
    "blocked",
}

JAVA_RUNTIME_NETWORK_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("java_net_http", re.compile(r"\bjava\.net\.http\b")),
    ("http_client", re.compile(r"\bHttpClient\b")),
    ("url_connection", re.compile(r"\bURLConnection\b|\bopenConnection\s*\(")),
    ("socket_api", re.compile(r"\b(?:Socket|ServerSocket|DatagramSocket)\s*\(")),
    ("nio_socket_channel", re.compile(r"\b(?:SocketChannel|AsynchronousSocketChannel)\b")),
    ("apache_http_client", re.compile(r"\borg\.apache\.http\b|\bHttpGet\b|\bHttpPost\b")),
    ("okhttp", re.compile(r"\bokhttp3\b")),
)

PLUGIN_EGRESS_POINT_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("equinox_p2_runtime", re.compile(r"org\.eclipse\.equinox\.p2")),
    ("marketplace_runtime", re.compile(r"marketplace", re.IGNORECASE)),
    ("telemetry_runtime", re.compile(r"telemetry|metricsUpload|diagnosticUpload", re.IGNORECASE)),
    ("update_runtime", re.compile(r"updateSite|updateCheck|automaticUpdate", re.IGNORECASE)),
)

URL_RE = re.compile(r"https?://[^\"'\s<>]+")
SOURCE_SUFFIXES_REQUIRING_SPDX = {".py", ".sh", ".bat", ".java", ".ps1"}
REQUIRED_NOTICE_LICENSES = {
    "DBeaver": "Apache-2.0",
    "Eclipse Platform": "EPL-2.0",
    "Tycho": "EPL-2.0",
    "SWT": "EPL-2.0",
    "JFace": "EPL-2.0",
    "ScratchBird JDBC": "MPL-2.0",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def fail(message: str) -> int:
    print(f"dbeaver_management_platform_gate=fail:{message}", file=sys.stderr)
    return 1


def load_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"json_invalid:{path}:{exc}") from exc
    if not isinstance(payload, dict):
        raise ValueError(f"json_not_object:{path}")
    return payload


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def local_name(tag: str) -> str:
    if "}" in tag:
        return tag.rsplit("}", 1)[1]
    return tag


def parse_xml(path: Path) -> ET.Element:
    try:
        return ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise ValueError(f"xml_invalid:{path}:{exc}") from exc


def parse_pom_properties(path: Path) -> dict[str, str]:
    root = parse_xml(path)
    properties: dict[str, str] = {}
    for child in root:
        if local_name(child.tag) == "version" and child.text:
            properties["project.version"] = child.text.strip()
        if local_name(child.tag) != "properties":
            continue
        for entry in child:
            if entry.text:
                properties[local_name(entry.tag)] = entry.text.strip()
    return properties


def parse_manifest(path: Path) -> dict[str, str]:
    lines: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.startswith(" ") and lines:
            lines[-1] += raw[1:]
        else:
            lines.append(raw)
    result: dict[str, str] = {}
    for line in lines:
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        result[key.strip()] = value.strip()
    return result


def split_manifest_list(value: str) -> set[str]:
    result: set[str] = set()
    for item in value.split(","):
        token = item.strip()
        if not token:
            continue
        result.add(token.split(";", 1)[0].strip())
    return result


def rel(path: Path, repo_root: Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def reject_private_value(value: str, context: str, issues: list[str]) -> None:
    if Path(value).is_absolute():
        issues.append(f"{context}:absolute_path:{value}")
    if ".." in Path(value).parts:
        issues.append(f"{context}:parent_path_escape:{value}")
    for fragment in FORBIDDEN_PRIVATE_FRAGMENTS:
        if fragment in value:
            issues.append(f"{context}:private_fragment:{value}")


def require_keys(row: dict[str, Any], keys: Iterable[str], context: str, issues: list[str]) -> None:
    for key in keys:
        value = row.get(key)
        if value is None or value == "" or value == []:
            issues.append(f"{context}:missing_{key}")


def as_str_list(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, str)]


def find_urls(text: str) -> set[str]:
    return set(URL_RE.findall(text))


def normalize_proof_status(value: Any) -> str:
    return str(value or "").strip().lower().replace("-", "_").replace(" ", "_")


def proof_artifact_from_row(row: dict[str, Any]) -> str:
    for key in ("proof_artifact", "proof_log", "proof_path", "evidence_artifact"):
        value = row.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def resolve_artifact_root(repo_root: Path, artifact_root: Path | None) -> Path | None:
    if artifact_root is None:
        return None
    return artifact_root if artifact_root.is_absolute() else repo_root / artifact_root


def resolve_proof_artifact(
    repo_root: Path,
    artifact_root: Path | None,
    artifact: str,
) -> Path:
    path = Path(artifact)
    if path.is_absolute():
        return path
    repo_candidate = repo_root / path
    if repo_candidate.is_file():
        return repo_candidate
    root = resolve_artifact_root(repo_root, artifact_root)
    if root is not None:
        artifact_candidate = root / path
        if artifact_candidate.is_file():
            return artifact_candidate
    return repo_candidate


def validate_required_proof(
    repo_root: Path,
    artifact_root: Path | None,
    row: dict[str, Any],
    context: str,
    require_release_proof: bool,
    issues: list[str],
) -> bool:
    status = normalize_proof_status(row.get("proof_status") or row.get("proof_result"))
    artifact = proof_artifact_from_row(row)
    artifact_ok = False

    if status in REJECTED_PROOF_STATUSES or status.startswith("skip"):
        issues.append(f"{context}:required_proof_rejected_status:{status}")
    elif require_release_proof and status not in ACCEPTED_PROOF_STATUSES:
        issues.append(f"{context}:required_proof_status_missing_or_not_passed:{status or '<missing>'}")

    if artifact:
        reject_private_value(artifact, f"{context}:proof_artifact", issues)
        artifact_path = resolve_proof_artifact(repo_root, artifact_root, artifact)
        if require_release_proof and not artifact_path.is_file():
            issues.append(f"{context}:proof_artifact_missing:{artifact}")
        else:
            artifact_ok = artifact_path.is_file()
    elif require_release_proof:
        issues.append(f"{context}:required_proof_artifact_missing")

    return artifact_ok


def all_files(root: Path) -> list[Path]:
    if not root.exists():
        return []
    skip_dirs = {"target", "dist", ".git", "__pycache__", ".pytest_cache"}
    result: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name not in skip_dirs]
        for filename in filenames:
            result.append(Path(dirpath) / filename)
    return result


def zip_member_names(path: Path, issues: list[str], context: str) -> set[str]:
    try:
        with zipfile.ZipFile(path) as archive:
            return set(archive.namelist())
    except (OSError, zipfile.BadZipFile) as exc:
        issues.append(f"{context}:zip_invalid:{path}:{exc}")
        return set()


def zip_member_text(path: Path, member_suffix: str) -> str:
    with zipfile.ZipFile(path) as archive:
        for member in archive.namelist():
            if member.endswith(member_suffix):
                return archive.read(member).decode("utf-8", errors="replace")
    return ""


def zip_contains_basename(members: set[str], basename: str) -> bool:
    return any(Path(member).name == basename for member in members)


def zip_contains_prefix_suffix(members: set[str], prefix: str, suffix: str) -> bool:
    return any(Path(member).name.startswith(prefix) and member.endswith(suffix) for member in members)


def inspect_update_site_zip(path: Path, issues: list[str], context: str) -> dict[str, Any]:
    members = zip_member_names(path, issues, context)
    summary = {"members": len(members), "nested_jdbc_jar": False}
    if not members:
        return summary

    for required in ("content", "artifacts"):
        if not any(Path(member).name in {f"{required}.jar", f"{required}.xml"} for member in members):
            issues.append(f"{context}:update_site_missing_{required}_metadata")
    for prefix in (
        "org.jkiss.dbeaver.ext.scratchbird.feature_",
        "org.jkiss.dbeaver.ext.scratchbird_",
        "org.jkiss.dbeaver.ext.scratchbird.ui_",
    ):
        if not zip_contains_prefix_suffix(members, prefix, ".jar"):
            issues.append(f"{context}:update_site_missing_installable:{prefix}*.jar")

    core_plugin_members = [
        member
        for member in members
        if Path(member).name.startswith("org.jkiss.dbeaver.ext.scratchbird_")
        and Path(member).name.endswith(".jar")
        and ".ui_" not in Path(member).name
    ]
    try:
        with zipfile.ZipFile(path) as archive:
            for member in core_plugin_members:
                try:
                    with zipfile.ZipFile(io.BytesIO(archive.read(member))) as plugin_archive:
                        if "drivers/scratchbird/scratchbird-jdbc.jar" in plugin_archive.namelist():
                            summary["nested_jdbc_jar"] = True
                            break
                except zipfile.BadZipFile:
                    issues.append(f"{context}:core_plugin_jar_invalid:{member}")
    except (OSError, zipfile.BadZipFile) as exc:
        issues.append(f"{context}:zip_invalid:{path}:{exc}")

    if not summary["nested_jdbc_jar"]:
        issues.append(f"{context}:update_site_core_plugin_missing_scratchbird_jdbc_jar")
    return summary


def inspect_stock_bundle_zip(path: Path, issues: list[str], context: str) -> dict[str, Any]:
    members = zip_member_names(path, issues, context)
    summary = {"members": len(members), "bundled_update_site": False, "notice_file": False}
    if not members:
        return summary

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
            issues.append(f"{context}:stock_bundle_missing_member:{required}")
    if zip_contains_prefix_suffix(members, "scratchbird-dbeaver-update-site-", ".zip"):
        summary["bundled_update_site"] = True
    else:
        issues.append(f"{context}:stock_bundle_missing_update_site_zip")

    notice_text = zip_member_text(path, "THIRD-PARTY-NOTICES.txt")
    if notice_text:
        summary["notice_file"] = True
        for component, license_id in REQUIRED_NOTICE_LICENSES.items():
            if component not in notice_text or license_id not in notice_text:
                issues.append(f"{context}:stock_bundle_notice_missing:{component}:{license_id}")
    else:
        issues.append(f"{context}:stock_bundle_notice_file_unreadable")
    return summary


def proof_json_passes(path: Path, expected_kind: str, issues: list[str], context: str) -> bool:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        issues.append(f"{context}:proof_json_invalid:{path}:{exc}")
        return False
    if not isinstance(payload, dict):
        issues.append(f"{context}:proof_json_not_object:{path}")
        return False
    status = normalize_proof_status(payload.get("status") or payload.get("proof_status"))
    kind = str(payload.get("artifact_type") or payload.get("proof_kind") or "")
    if expected_kind and kind != expected_kind:
        issues.append(f"{context}:proof_kind_mismatch:{path}:{kind}")
    if status not in ACCEPTED_PROOF_STATUSES:
        issues.append(f"{context}:proof_status_not_passed:{path}:{status or '<missing>'}")
        return False
    return True


def check_version_matrix(
    repo_root: Path,
    adapter_root: Path,
    contract: dict[str, Any],
) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    summary: dict[str, Any] = {}

    if contract.get("schema_id") != SCHEMA_ID:
        issues.append("contract:schema_id_mismatch")

    pom_props = parse_pom_properties(adapter_root / "pom.xml")
    expected = contract.get("expected_build", {})
    if not isinstance(expected, dict):
        issues.append("contract:expected_build_not_object")
        expected = {}

    for key, pom_key in (
        ("tycho_version", "tycho.version"),
        ("eclipse_release", "eclipse.version"),
    ):
        if expected.get(key) != pom_props.get(pom_key):
            issues.append(
                f"version_matrix:{key}_mismatch:"
                f"contract={expected.get(key)!r}:pom={pom_props.get(pom_key)!r}"
            )

    project_version = pom_props.get("project.version", "")
    expected_version_prefix = str(expected.get("adapter_version_prefix", ""))
    if expected_version_prefix and not project_version.startswith(expected_version_prefix):
        issues.append(
            f"version_matrix:adapter_version_mismatch:"
            f"contract={expected_version_prefix}:pom={project_version}"
        )

    core_manifest = parse_manifest(
        adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird" / "META-INF" / "MANIFEST.MF"
    )
    ui_manifest = parse_manifest(
        adapter_root
        / "plugins"
        / "org.jkiss.dbeaver.ext.scratchbird.ui"
        / "META-INF"
        / "MANIFEST.MF"
    )
    java_envs = {
        core_manifest.get("Bundle-RequiredExecutionEnvironment", ""),
        ui_manifest.get("Bundle-RequiredExecutionEnvironment", ""),
    }
    expected_java = str(expected.get("java_execution_environment", ""))
    if java_envs != {expected_java}:
        issues.append(
            "version_matrix:java_execution_environment_mismatch:"
            f"contract={expected_java}:manifests={sorted(java_envs)}"
        )

    rows = contract.get("supported_version_matrix")
    if not isinstance(rows, list) or not rows:
        issues.append("version_matrix:missing_rows")
        rows = []

    required_fields = (
        "target_id",
        "dbeaver_ce_version",
        "java_execution_environment",
        "eclipse_release",
        "tycho_version",
        "os",
        "arch",
        "support_status",
        "expected_outcome",
        "proof_anchor",
    )
    target_ids: set[str] = set()
    supported_tested = 0
    supported_static_contract = 0
    unsupported_reasons: set[str] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            issues.append(f"version_matrix:row_{index}:not_object")
            continue
        context = f"version_matrix:{row.get('target_id', index)}"
        require_keys(row, required_fields, context, issues)
        target_id = str(row.get("target_id", ""))
        if target_id in target_ids:
            issues.append(f"{context}:duplicate_target_id")
        target_ids.add(target_id)
        reject_private_value(str(row.get("proof_anchor", "")), f"{context}:proof_anchor", issues)
        if row.get("java_execution_environment") != expected_java:
            issues.append(f"{context}:java_execution_environment_not_current_manifest")
        if row.get("eclipse_release") != expected.get("eclipse_release"):
            issues.append(f"{context}:eclipse_release_not_current_pom")
        if row.get("tycho_version") != expected.get("tycho_version"):
            issues.append(f"{context}:tycho_version_not_current_pom")
        status = row.get("support_status")
        if status == "supported_tested":
            supported_tested += 1
        if status == "supported_static_contract":
            supported_static_contract += 1
        if status == "unsupported_refusal":
            reason = str(row.get("refusal_reason", "")).strip()
            if not reason:
                issues.append(f"{context}:missing_refusal_reason")
            unsupported_reasons.add(reason)
            if "refus" not in str(row.get("expected_outcome", "")).lower():
                issues.append(f"{context}:expected_outcome_not_refusal")
        elif status not in {
            "supported_tested",
            "supported_static_contract",
            "supported_packaged_offhost",
        }:
            issues.append(f"{context}:unknown_support_status:{status}")

    if supported_tested + supported_static_contract == 0:
        issues.append("version_matrix:no_supported_contract_target")
    for required_reason in ("unsupported_dbeaver_version", "unsupported_java_version"):
        if required_reason not in unsupported_reasons:
            issues.append(f"version_matrix:missing_refusal_case:{required_reason}")

    summary.update(
        {
            "rows": len(rows),
            "supported_tested": supported_tested,
            "supported_static_contract": supported_static_contract,
            "unsupported_refusals": len(
                [
                    row
                    for row in rows
                    if isinstance(row, dict) and row.get("support_status") == "unsupported_refusal"
                ]
            ),
            "pom_project_version": project_version,
            "pom_tycho_version": pom_props.get("tycho.version"),
            "pom_eclipse_release": pom_props.get("eclipse.version"),
            "manifest_java_environments": sorted(java_envs),
        }
    )
    return issues, summary


def extension_points(plugin_xml: Path) -> set[str]:
    root = parse_xml(plugin_xml)
    return {
        value
        for value in (element.get("point") for element in root.findall(".//extension"))
        if value
    }


def element_ids(plugin_xml: Path, element_name: str) -> set[str]:
    root = parse_xml(plugin_xml)
    return {
        value
        for value in (element.get("id") for element in root.findall(f".//{element_name}"))
        if value
    }


def check_api_drift(adapter_root: Path, contract: dict[str, Any]) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    summary: dict[str, Any] = {}
    api = contract.get("api_drift")
    if not isinstance(api, dict):
        return ["api_drift:contract_not_object"], summary

    feature = parse_xml(
        adapter_root
        / "features"
        / "org.jkiss.dbeaver.ext.scratchbird.feature"
        / "feature.xml"
    )
    category = parse_xml(adapter_root / "repository" / "category.xml")
    feature_id = str(api.get("feature_id", ""))
    if feature.get("id") != feature_id:
        issues.append(f"api_drift:feature_id_mismatch:{feature.get('id')}")
    feature_plugins = {
        value
        for value in (plugin.get("id") for plugin in feature.findall("plugin"))
        if value
    }
    expected_plugins = set(as_str_list(api.get("plugin_ids")))
    if feature_plugins != expected_plugins:
        issues.append(
            "api_drift:feature_plugin_set_mismatch:"
            f"expected={sorted(expected_plugins)}:actual={sorted(feature_plugins)}"
        )
    category_features = {
        value
        for value in (element.get("id") for element in category.findall("feature"))
        if value
    }
    if feature_id not in category_features:
        issues.append("api_drift:repository_category_missing_feature")

    manifests = {
        "core": parse_manifest(
            adapter_root
            / "plugins"
            / "org.jkiss.dbeaver.ext.scratchbird"
            / "META-INF"
            / "MANIFEST.MF"
        ),
        "ui": parse_manifest(
            adapter_root
            / "plugins"
            / "org.jkiss.dbeaver.ext.scratchbird.ui"
            / "META-INF"
            / "MANIFEST.MF"
        ),
    }
    expected_symbols = {
        "core": "org.jkiss.dbeaver.ext.scratchbird;singleton:=true",
        "ui": "org.jkiss.dbeaver.ext.scratchbird.ui;singleton:=true",
    }
    for key, expected_symbol in expected_symbols.items():
        actual = manifests[key].get("Bundle-SymbolicName")
        if actual != expected_symbol:
            issues.append(f"api_drift:{key}:bundle_symbolic_name_mismatch:{actual}")
    for key, required in (
        ("core", set(as_str_list(api.get("core_require_bundles")))),
        ("ui", set(as_str_list(api.get("ui_require_bundles")))),
    ):
        actual = split_manifest_list(manifests[key].get("Require-Bundle", ""))
        missing = sorted(required - actual)
        if missing:
            issues.append(f"api_drift:{key}:missing_require_bundle:{','.join(missing)}")
    core_exports = split_manifest_list(manifests["core"].get("Export-Package", ""))
    expected_exports = set(as_str_list(api.get("core_export_packages")))
    missing_exports = sorted(expected_exports - core_exports)
    if missing_exports:
        issues.append(f"api_drift:core:missing_export_package:{','.join(missing_exports)}")

    core_xml = adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird" / "plugin.xml"
    ui_xml = adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird.ui" / "plugin.xml"
    core_points = extension_points(core_xml)
    ui_points = extension_points(ui_xml)
    for point in as_str_list(api.get("core_extension_points")):
        if point not in core_points:
            issues.append(f"api_drift:core:missing_extension_point:{point}")
    for point in as_str_list(api.get("ui_extension_points")):
        if point not in ui_points:
            issues.append(f"api_drift:ui:missing_extension_point:{point}")

    core_root = parse_xml(core_xml)
    driver = core_root.find(".//driver[@id='scratchbird_jdbc']")
    if driver is None:
        issues.append("api_drift:driver_id_missing:scratchbird_jdbc")
    else:
        if driver.get("class") != api.get("driver_class"):
            issues.append(f"api_drift:driver_class_mismatch:{driver.get('class')}")
        if driver.get("defaultPort") != str(api.get("default_port")):
            issues.append(f"api_drift:driver_default_port_mismatch:{driver.get('defaultPort')}")
    datasource = None
    for candidate in core_root.findall(".//datasource[@id='scratchbird']"):
        if candidate.get("parent") or candidate.get("class"):
            datasource = candidate
            break
    if datasource is None:
        issues.append("api_drift:datasource_id_missing:scratchbird")
    elif datasource.get("parent") != "generic":
        issues.append(f"api_drift:datasource_parent_mismatch:{datasource.get('parent')}")

    required_commands = set(as_str_list(api.get("ui_command_ids")))
    actual_commands = element_ids(ui_xml, "command")
    missing_commands = sorted(required_commands - actual_commands)
    if missing_commands:
        issues.append(f"api_drift:ui:missing_command_ids:{','.join(missing_commands)}")

    summary.update(
        {
            "feature_id": feature.get("id"),
            "feature_plugins": sorted(feature_plugins),
            "core_extension_points": sorted(core_points),
            "ui_extension_points": sorted(ui_points),
            "ui_commands": len(actual_commands),
        }
    )
    return issues, summary


def check_secure_properties(
    adapter_root: Path,
    contract: dict[str, Any],
) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    secure_contract = contract.get("secure_properties")
    if isinstance(secure_contract, dict):
        sensitive_properties = set(as_str_list(secure_contract.get("sensitive_provider_properties")))
    else:
        sensitive_properties = set()
    if not sensitive_properties:
        sensitive_properties = set(DEFAULT_SENSITIVE_PROVIDER_PROPERTIES)

    core_xml = adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird" / "plugin.xml"
    root = parse_xml(core_xml)
    driver = root.find(".//driver[@id='scratchbird_jdbc']")
    driver_properties: set[str] = set()
    if driver is None:
        issues.append("secure_properties:driver_missing:scratchbird_jdbc")
    else:
        for parameter in driver.findall("parameter"):
            if parameter.get("name") == "driver-properties":
                driver_properties.update(
                    token.strip()
                    for token in str(parameter.get("value", "")).split(",")
                    if token.strip()
                )
    provider_properties = {
        property_node.get("id"): property_node
        for property_node in root.findall(".//provider-properties//property")
        if property_node.get("id")
    }

    for property_id in sorted(sensitive_properties):
        if property_id not in driver_properties:
            issues.append(f"secure_properties:driver_property_missing:{property_id}")
        node = provider_properties.get(property_id)
        if node is None:
            issues.append(f"secure_properties:provider_property_missing:{property_id}")
            continue
        features = {
            feature.strip()
            for feature in str(node.get("features", "")).split(",")
            if feature.strip()
        }
        if {"secured", "password"} - features:
            issues.append(f"secure_properties:provider_property_not_secured_password:{property_id}")
        if "defaultValue" in node.attrib or "value" in node.attrib:
            issues.append(f"secure_properties:provider_property_has_default_secret:{property_id}")
        if node.get("type") != "string":
            issues.append(f"secure_properties:provider_property_type_not_string:{property_id}")

    sensitive_name_pattern = re.compile(r"(?i)(token|secret|password|assertion|auth_.*payload)")
    for property_id, node in sorted(provider_properties.items()):
        if not sensitive_name_pattern.search(property_id):
            continue
        features = {
            feature.strip()
            for feature in str(node.get("features", "")).split(",")
            if feature.strip()
        }
        if {"secured", "password"} - features:
            issues.append(f"secure_properties:sensitive_named_property_not_secured:{property_id}")
        if "defaultValue" in node.attrib or "value" in node.attrib:
            issues.append(f"secure_properties:sensitive_named_property_has_default:{property_id}")

    summary = {
        "sensitive_provider_properties": sorted(sensitive_properties),
        "driver_properties": len(driver_properties),
        "provider_properties": len(provider_properties),
    }
    return issues, summary


def check_install_lifecycle(
    repo_root: Path,
    adapter_root: Path,
    fixture: dict[str, Any],
    artifact_root: Path | None,
    require_release_proof: bool,
) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    summary: dict[str, Any] = {}
    if fixture.get("schema_id") != FIXTURE_SCHEMA_ID:
        issues.append("install_lifecycle:fixture_schema_id_mismatch")

    cases = fixture.get("lifecycle_cases")
    if not isinstance(cases, list) or not cases:
        issues.append("install_lifecycle:missing_lifecycle_cases")
        cases = []

    phases: set[str] = set()
    script_checked = 0
    proof_artifacts_checked = 0
    for index, row in enumerate(cases):
        if not isinstance(row, dict):
            issues.append(f"install_lifecycle:row_{index}:not_object")
            continue
        context = f"install_lifecycle:{row.get('case_id', index)}"
        require_keys(row, ("case_id", "phase", "expected_outcome", "proof_policy"), context, issues)
        proof_policy = str(row.get("proof_policy", "")).strip().lower()
        if any(token in proof_policy for token in ("skip", "xfail", "waiver", "defer")):
            issues.append(f"{context}:required_proof_policy_not_fail_closed:{proof_policy}")
        if validate_required_proof(repo_root, artifact_root, row, context, require_release_proof, issues):
            proof_artifacts_checked += 1
        phase = str(row.get("phase", ""))
        phases.add(phase)
        for value in row.values():
            if isinstance(value, str):
                reject_private_value(value, context, issues)
        script_rel = row.get("script_path")
        if script_rel:
            reject_private_value(str(script_rel), f"{context}:script_path", issues)
            script = repo_root / str(script_rel)
            if not script.is_file():
                issues.append(f"{context}:script_missing:{script_rel}")
                continue
            script_text = script.read_text(encoding="utf-8", errors="replace")
            script_checked += 1
            for token in FORBIDDEN_REQUIRED_PROOF_TOKENS:
                if token in script_text:
                    issues.append(f"{context}:script_skips_required_proof:{script_rel}:{token}")
            anchors = as_str_list(row.get("required_script_tokens"))
            if not anchors:
                issues.append(f"{context}:required_script_tokens_missing")
            for token in anchors:
                if token not in script_text:
                    issues.append(f"{context}:script_token_missing:{token}")
        elif row.get("requires_external_proof") is not True:
            issues.append(f"{context}:no_script_or_external_proof_policy")

    missing_phases = sorted(REQUIRED_LIFECYCLE_PHASES - phases)
    for phase in missing_phases:
        issues.append(f"install_lifecycle:missing_required_phase:{phase}")

    migration_rows = fixture.get("migration_fixtures")
    if not isinstance(migration_rows, list) or not migration_rows:
        issues.append("workspace_migration:missing_migration_fixtures")
        migration_rows = []
    migration_cases: set[str] = set()
    for index, row in enumerate(migration_rows):
        if not isinstance(row, dict):
            issues.append(f"workspace_migration:row_{index}:not_object")
            continue
        context = f"workspace_migration:{row.get('fixture_id', index)}"
        require_keys(
            row,
            ("fixture_id", "case_type", "input_shape", "expected_outcome", "redaction_policy"),
            context,
            issues,
        )
        migration_cases.add(str(row.get("case_type", "")))
        for value in row.values():
            if isinstance(value, str):
                reject_private_value(value, context, issues)
                if re.search(r"(?i)(password|secret|token)\s*[:=]\s*[^<\s]+", value):
                    issues.append(f"{context}:literal_secret_like_value")
        if row.get("redaction_policy") != "no_plaintext_secret_output":
            issues.append(f"{context}:redaction_policy_not_strict")

    for case_type in sorted(REQUIRED_MIGRATION_CASES - migration_cases):
        issues.append(f"workspace_migration:missing_required_case:{case_type}")

    build_p2 = adapter_root / "scripts" / "build-p2-update-site.sh"
    build_bundle = adapter_root / "scripts" / "build-stock-test-bundle.sh"
    if "scratchbird-jdbc.jar" not in build_p2.read_text(encoding="utf-8", errors="replace"):
        issues.append("packaging:build_p2_does_not_stage_jdbc_jar")
    bundle_text = build_bundle.read_text(encoding="utf-8", errors="replace")
    for required in (
        "install-into-stock-dbeaver.sh",
        "install-into-stock-dbeaver.bat",
        "uninstall-from-stock-dbeaver.sh",
        "uninstall-from-stock-dbeaver.bat",
        "SHA256SUMS.txt",
        "THIRD-PARTY-NOTICES.txt",
    ):
        if required not in bundle_text:
            issues.append(f"packaging:stock_bundle_missing_payload_anchor:{required}")

    summary.update(
        {
            "lifecycle_cases": len(cases),
            "lifecycle_phases": sorted(phases),
            "script_checked": script_checked,
            "proof_artifacts_checked": proof_artifacts_checked,
            "release_proof_required": require_release_proof,
            "migration_fixtures": len(migration_rows),
            "migration_case_types": sorted(migration_cases),
        }
    )
    return issues, summary


def check_license_ip(
    repo_root: Path,
    adapter_root: Path,
    contract: dict[str, Any],
) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    summary: dict[str, Any] = {}
    license_ip = contract.get("license_ip")
    if not isinstance(license_ip, dict):
        return ["license_ip:contract_not_object"], summary

    checked_sources = 0
    dbeaver_notice_files = 0
    for path in all_files(adapter_root):
        rel_path = rel(path, repo_root)
        if path.suffix in SOURCE_SUFFIXES_REQUIRING_SPDX:
            checked_sources += 1
            text = path.read_text(encoding="utf-8", errors="replace")
            if "SPDX-License-Identifier:" not in text[:1200]:
                issues.append(f"license_ip:missing_spdx:{rel_path}")
            has_dbeaver_notice = (
                "DBeaver - Universal Database Manager" in text[:1600]
                or "Copyright (C) 2010-2026 DBeaver Corp" in text[:1600]
            )
            if has_dbeaver_notice:
                dbeaver_notice_files += 1
                if "Apache License, Version 2.0" not in text[:2200]:
                    issues.append(f"license_ip:dbeaver_notice_missing_apache_license:{rel_path}")
                if "/org/jkiss/dbeaver/ext/scratchbird/" not in "/" + rel_path:
                    issues.append(f"license_ip:dbeaver_notice_outside_scratchbird_package:{rel_path}")
                if path.suffix == ".java" and not path.name.startswith("ScratchBird"):
                    issues.append(f"license_ip:dbeaver_notice_non_scratchbird_class:{rel_path}")

    feature_text = (
        adapter_root
        / "features"
        / "org.jkiss.dbeaver.ext.scratchbird.feature"
        / "feature.xml"
    ).read_text(encoding="utf-8")
    if "Apache License, Version 2.0" not in feature_text:
        issues.append("license_ip:feature_license_missing_apache_2")

    notices = license_ip.get("third_party_notice_components")
    if not isinstance(notices, list):
        issues.append("license_ip:third_party_notice_components_missing")
        notices = []
    notice_by_name = {
        str(item.get("component", "")): item
        for item in notices
        if isinstance(item, dict)
    }
    notice_names = set(notice_by_name)
    for required, expected_license in REQUIRED_NOTICE_LICENSES.items():
        if required not in notice_names:
            issues.append(f"license_ip:third_party_notice_missing:{required}")
            continue
        row = notice_by_name[required]
        if row.get("license") != expected_license:
            issues.append(
                f"license_ip:third_party_notice_license_mismatch:"
                f"{required}:{row.get('license')}:{expected_license}"
            )
        if not str(row.get("boundary", "")).strip():
            issues.append(f"license_ip:third_party_notice_boundary_missing:{required}")

    for jar in adapter_root.rglob("*.jar"):
        if "target" in jar.parts or "build" in jar.parts:
            continue
        issues.append(f"license_ip:unexpected_checked_in_jar:{rel(jar, repo_root)}")

    summary.update(
        {
            "checked_sources": checked_sources,
            "dbeaver_notice_files": dbeaver_notice_files,
            "third_party_notice_components": sorted(notice_names),
        }
    )
    return issues, summary


def check_network_egress(
    repo_root: Path,
    adapter_root: Path,
    contract: dict[str, Any],
) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    summary: dict[str, Any] = {}
    policy = contract.get("network_egress_policy")
    if not isinstance(policy, dict):
        return ["network_egress:contract_not_object"], summary

    allowed_runtime_urls = set(as_str_list(policy.get("allowed_runtime_urls")))
    allowed_build_urls = set(as_str_list(policy.get("allowed_build_time_urls")))
    runtime_policy_text = str(policy.get("runtime_egress_policy", ""))
    for required in ("telemetry", "marketplace", "update", "diagnostic"):
        if required not in runtime_policy_text.lower():
            issues.append(f"network_egress:runtime_policy_missing_class:{required}")
    runtime_pattern_hits: list[str] = []
    plugin_roots = [
        adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird",
        adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird.ui",
    ]
    for root in plugin_roots:
        for path in all_files(root):
            rel_path = rel(path, repo_root)
            if path.suffix == ".java":
                text = path.read_text(encoding="utf-8", errors="replace")
                for name, pattern in JAVA_RUNTIME_NETWORK_PATTERNS:
                    if pattern.search(text):
                        runtime_pattern_hits.append(f"{rel_path}:{name}")
            if path.name == "plugin.xml":
                text = path.read_text(encoding="utf-8", errors="replace")
                for name, pattern in PLUGIN_EGRESS_POINT_PATTERNS:
                    if pattern.search(text):
                        runtime_pattern_hits.append(f"{rel_path}:{name}")
                for url in find_urls(text):
                    if url not in allowed_runtime_urls:
                        issues.append(f"network_egress:runtime_url_not_allowlisted:{rel_path}:{url}")

    for xml in (
        adapter_root / "features" / "org.jkiss.dbeaver.ext.scratchbird.feature" / "feature.xml",
        adapter_root / "repository" / "category.xml",
    ):
        for url in find_urls(xml.read_text(encoding="utf-8", errors="replace")):
            if url not in allowed_runtime_urls:
                issues.append(f"network_egress:metadata_url_not_allowlisted:{rel(xml, repo_root)}:{url}")

    for pom in [
        adapter_root / "pom.xml",
        adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird" / "pom.xml",
        adapter_root / "plugins" / "org.jkiss.dbeaver.ext.scratchbird.ui" / "pom.xml",
        adapter_root / "features" / "org.jkiss.dbeaver.ext.scratchbird.feature" / "pom.xml",
        adapter_root / "repository" / "pom.xml",
    ]:
        text = pom.read_text(encoding="utf-8", errors="replace")
        for url in find_urls(text):
            if url not in allowed_build_urls:
                issues.append(f"network_egress:build_url_not_allowlisted:{rel(pom, repo_root)}:{url}")

    for hit in runtime_pattern_hits:
        issues.append(f"network_egress:runtime_network_api_present:{hit}")

    network_policy_source = (
        adapter_root
        / "plugins"
        / "org.jkiss.dbeaver.ext.scratchbird"
        / "src"
        / "org"
        / "jkiss"
        / "dbeaver"
        / "ext"
        / "scratchbird"
        / "model"
        / "ScratchBirdNetworkPolicy.java"
    )
    if not network_policy_source.is_file():
        issues.append("network_egress:network_policy_source_missing")
    else:
        source = network_policy_source.read_text(encoding="utf-8", errors="replace")
        for token in (
            "telemetryEnabledByDefault",
            "updateChecksEnabledByDefault",
            "diagnosticsUploadEnabledByDefault",
            "plugin-telemetry",
            "diagnostic-upload",
            "marketplace-call",
            "implicit-update-check",
            "ScratchBirdSecurityRedactor.redactPropertyValue",
        ):
            if token not in source:
                issues.append(f"network_egress:network_policy_source_missing_token:{token}")

    summary.update(
        {
            "allowed_runtime_urls": sorted(allowed_runtime_urls),
            "allowed_build_time_urls": sorted(allowed_build_urls),
            "runtime_network_pattern_hits": runtime_pattern_hits,
            "runtime_policy_text_present": bool(runtime_policy_text),
        }
    )
    return issues, summary


def check_release_artifacts(
    repo_root: Path,
    artifact_root: Path | None,
    require_artifacts: bool,
) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    summary: dict[str, Any] = {"required": require_artifacts}
    if not require_artifacts:
        summary["status"] = "not_required_in_static_mode"
        return issues, summary
    if artifact_root is None:
        issues.append("release_artifacts:artifact_root_required_in_release_mode")
        return issues, summary
    root = resolve_artifact_root(repo_root, artifact_root)
    if root is None:
        issues.append("release_artifacts:artifact_root_required_in_release_mode")
        return issues, summary
    summary["artifact_root"] = rel(root, repo_root)
    if not root.is_dir():
        issues.append(f"release_artifacts:artifact_root_missing:{root}")
        return issues, summary
    update_sites = sorted(root.glob("scratchbird-dbeaver-update-site-*.zip"))
    stock_bundles = sorted(root.glob("scratchbird-dbeaver-stock-test-bundle-*.zip"))
    source_checkout_proofs = sorted(root.glob("scratchbird-dbeaver-source-checkout-proof-*.json"))
    stock_install_proofs = sorted(root.glob("scratchbird-dbeaver-stock-install-proof-*.json"))
    summary["update_site_zips"] = [rel(path, repo_root) for path in update_sites]
    summary["stock_bundle_zips"] = [rel(path, repo_root) for path in stock_bundles]
    summary["source_checkout_proofs"] = [rel(path, repo_root) for path in source_checkout_proofs]
    summary["stock_install_proofs"] = [rel(path, repo_root) for path in stock_install_proofs]
    if not update_sites:
        issues.append("release_artifacts:update_site_zip_missing")
    else:
        summary["update_site_inspection"] = [
            inspect_update_site_zip(path, issues, f"release_artifacts:{path.name}")
            for path in update_sites
        ]
    if not stock_bundles:
        issues.append("release_artifacts:stock_bundle_zip_missing")
    else:
        summary["stock_bundle_inspection"] = [
            inspect_stock_bundle_zip(path, issues, f"release_artifacts:{path.name}")
            for path in stock_bundles
        ]
    if not any(path.name.upper().startswith("SHA256") for path in root.iterdir() if path.is_file()):
        issues.append("release_artifacts:sha256_manifest_missing")
    if not source_checkout_proofs:
        issues.append("release_artifacts:source_checkout_proof_missing")
    else:
        summary["source_checkout_proof_pass_count"] = sum(
            1
            for path in source_checkout_proofs
            if proof_json_passes(
                path,
                "source_checkout_verify",
                issues,
                "release_artifacts:source_checkout_proof",
            )
        )
    if not stock_install_proofs:
        issues.append("release_artifacts:stock_install_proof_missing")
    else:
        summary["stock_install_proof_pass_count"] = sum(
            1
            for path in stock_install_proofs
            if proof_json_passes(
                path,
                "stock_install_lifecycle",
                issues,
                "release_artifacts:stock_install_proof",
            )
        )
    return issues, summary


def build_report(
    repo_root: Path,
    contract_path: Path,
    fixture_path: Path,
    mode: str,
    artifact_root: Path | None,
) -> dict[str, Any]:
    contract = load_json(contract_path)
    fixture = load_json(fixture_path)
    adapter_root = repo_root / ADAPTER_REL
    issues: list[str] = []
    summaries: dict[str, Any] = {}

    if not adapter_root.is_dir():
        issues.append(f"adapter_root_missing:{ADAPTER_REL.as_posix()}")
    else:
        checks = (
            ("version_matrix", check_version_matrix(repo_root, adapter_root, contract)),
            ("api_drift", check_api_drift(adapter_root, contract)),
            ("secure_properties", check_secure_properties(adapter_root, contract)),
            (
                "install_lifecycle",
                check_install_lifecycle(
                    repo_root,
                    adapter_root,
                    fixture,
                    artifact_root,
                    mode == "release",
                ),
            ),
            ("license_ip", check_license_ip(repo_root, adapter_root, contract)),
            ("network_egress", check_network_egress(repo_root, adapter_root, contract)),
            (
                "release_artifacts",
                check_release_artifacts(repo_root, artifact_root, mode == "release"),
            ),
        )
        for name, (check_issues, summary) in checks:
            issues.extend(f"{name}:{issue}" for issue in check_issues)
            summaries[name] = summary

    blocking_workplan_ids: set[str] = set()
    for issue in issues:
        prefix = issue.split(":", 1)[0]
        blocking_workplan_ids.update(ISSUE_WORKPLAN_PREFIX_IDS.get(prefix, SUPPORTED_WORKPLAN_IDS))

    return {
        "schema_id": REPORT_SCHEMA_ID,
        "command": "dbeaver_management_platform_gate.py",
        "mode": mode,
        "status": "fail" if issues else "pass",
        "supported_workplan_ids": list(SUPPORTED_WORKPLAN_IDS),
        "gate_support": {
            "DBEAVER-MGMT-GATE-004": "dbeaver_release_controller_static_gate",
            "DBEAVER-MGMT-GATE-006": "secure_dbeaver_provider_property_check",
            "DBEAVER-MGMT-GATE-023": "packaging_script_and_artifact_mode_checks",
            "DBEAVER-MGMT-GATE-026": "supported_version_matrix_contract_check",
            "DBEAVER-MGMT-GATE-027": "install_lifecycle_fixture_and_script_check",
            "DBEAVER-MGMT-GATE-028": "api_drift_manifest_feature_plugin_check",
            "DBEAVER-MGMT-GATE-029": "secure_storage_and_redaction_property_contract_check",
            "DBEAVER-MGMT-GATE-034": "license_ip_spdx_notice_boundary_check",
            "DBEAVER-MGMT-GATE-043": "workspace_migration_fixture_check",
            "DBEAVER-MGMT-GATE-044": "network_egress_policy_check",
            "DBEAVER-MGMT-GATE-046": "static_or_release_preflight_summary_not_final_closure",
        },
        "summary": summaries,
        "closure_support": {
            "can_support_static_preflight": mode == "static" and not issues,
            "can_support_release_preflight": mode == "release" and not issues,
            "can_support_final_closure": False,
            "final_closure_requires": [
                "live_server_dbeaver_management_corpus",
                "stock_dbeaver_gui_automation_and_screenshots",
                "manual_qa_signoff",
                "server_authorized_management_apply_verify_evidence",
                "workspace_redaction_and_cleanup_evidence",
            ],
            "blocking_workplan_ids": sorted(blocking_workplan_ids),
            "issue_count": len(issues),
        },
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--contract", type=Path)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--mode", choices=("static", "release"), default="static")
    parser.add_argument("--artifact-root", type=Path)
    parser.add_argument("--output", "--evidence-output", dest="output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    contract_path = args.contract or repo_root / DEFAULT_CONTRACT_REL
    if not contract_path.is_absolute():
        contract_path = repo_root / contract_path
    fixture_path = args.fixture or repo_root / DEFAULT_FIXTURE_REL
    if not fixture_path.is_absolute():
        fixture_path = repo_root / fixture_path
    output = args.output or repo_root / DEFAULT_REPORT_REL
    if not output.is_absolute():
        output = repo_root / output

    try:
        report = build_report(repo_root, contract_path, fixture_path, args.mode, args.artifact_root)
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"dbeaver_management_platform_gate={report['status']}:{args.mode}:{output}")
    if report["issues"]:
        for issue in report["issues"][:100]:
            print(f"- {issue}", file=sys.stderr)
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
