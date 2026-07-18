#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Binary package-identity checkpoint for standalone parser-family artifacts.

This checkpoint validates manifest shape, project-target link closure, symbol
ownership, and a synthetic staged identity probe.  It intentionally does not
claim a real install-prefix or operational parser-runtime isolation result.

Search keys:
  PARSER-STANDALONE-LINK-CLOSURE-GATE
  PARSER-STANDALONE-PACKAGE-CLOSURE-GATE
  PARSER-STANDALONE-RUNTIME-TRACE-GATE
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass


TARGET_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_])(?:sbl|sbp|sbu|sbup|sb)_[A-Za-z0-9_]+")
LIBRARY_TARGET_RE = re.compile(
    r"(?<![A-Za-z0-9_])lib(?P<target>(?:sbl|sbp|sbu|sbup|sb)_[A-Za-z0-9_]+)"
    r"(?:\.(?:a|so|dylib)|\.so(?:\.[0-9]+)*)"
)
SBSQL_BRANDED_LIBRARY_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?P<artifact>libSBParser_(?:pipeline|core|udr)"
    r"(?:\.(?:a|so|dylib)|\.so(?:\.[0-9]+)*))"
)
SBSQL_BRANDED_EXECUTABLE_PATH_RE = re.compile(
    r"(?P<artifact>(?:[A-Za-z0-9_.+~-]+/)+SBParser)"
    r"(?=$|[\s\"',)\]:])"
)
SBSQL_BRANDED_EXECUTABLE_LISTING_RE = re.compile(r"(?m)^(?P<artifact>SBParser)$")
CANONICAL_MANIFEST_FIELDS = (
    "parser_family_uuid",
    "standalone_package",
    "cross_parser_dependency_count",
    "same_family_library_set",
    "neutral_dependency_set",
    "parser_support_udr_family_uuid",
    "direct_sblr_lowering",
    "foreign_parser_fallback",
    "isolated_build_profile",
    "isolated_package_profile",
    "dependency_closure_evidence",
)
DEPENDENCY_EVIDENCE_CHANNELS = (
    "source",
    "build_graph",
    "link",
    "symbol",
    "package",
    "runtime",
)
DEPENDENCY_EVIDENCE_REFERENCES = {
    "source": "parser_family_isolation_evidence.json#source_ownership_scan",
    "build_graph": "parser_family_isolation_evidence.json#build_graph_ownership_scan",
    "link": (
        "parser_family_binary_isolation_evidence.json#project_target_link_command_scan"
    ),
    "symbol": (
        "parser_family_binary_isolation_evidence.json#binary_and_archive_symbol_scan"
    ),
    "package": (
        "parser_family_package_isolation_evidence.json#empty_prefix_package_closure"
    ),
    "runtime": (
        "parser_family_binary_isolation_evidence.json#staged_identity_probe_trace"
    ),
}
ISOLATED_BUILD_PROFILE = "parser-family-isolated-release-v1"
ISOLATED_PACKAGE_PROFILE = "parser-family-empty-prefix-v1"


@dataclass(frozen=True)
class Finding:
    family: str
    channel: str
    foreign_family: str
    evidence: str

    def to_json(self) -> dict[str, str]:
        return {
            "family": self.family,
            "channel": self.channel,
            "foreign_family": self.foreign_family,
            "evidence": self.evidence[:400],
        }


class DuplicateJsonKeyError(ValueError):
    def __init__(self, key: str):
        super().__init__(f"duplicate JSON key: {key}")
        self.key = key


def strict_json_loads(text: str) -> object:
    def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise DuplicateJsonKeyError(key)
            result[key] = value
        return result

    return json.loads(text, object_pairs_hook=reject_duplicate_keys)


class Registry:
    def __init__(self, payload: dict[str, object]):
        families = payload.get("families")
        if not isinstance(families, dict) or not families:
            raise ValueError("ownership map has no families")
        self.families = families
        strict = payload.get("strict_standalone_families", [])
        if not isinstance(strict, list):
            raise ValueError("strict_standalone_families must be an array")
        self.strict_standalone_families = {str(item) for item in strict}
        legacy_targets = payload.get("legacy_shared_parser_targets", [])
        if not isinstance(legacy_targets, list):
            raise ValueError("legacy_shared_parser_targets must be an array")
        self.legacy_shared_parser_targets = {str(item) for item in legacy_targets}
        forbidden_neutral_targets = payload.get("forbidden_neutral_parser_targets", [])
        if not isinstance(forbidden_neutral_targets, list):
            raise ValueError("forbidden_neutral_parser_targets must be an array")
        self.forbidden_neutral_parser_targets = {
            str(item) for item in forbidden_neutral_targets
        }
        legacy_roots = payload.get("legacy_shared_parser_roots", [])
        if not isinstance(legacy_roots, list):
            raise ValueError("legacy_shared_parser_roots must be an array")
        self.legacy_shared_parser_roots = {
            str(item).replace("\\", "/") for item in legacy_roots
        }

    def target_owner(self, token: str) -> str | None:
        matches: list[tuple[int, str]] = []
        for family, raw in self.families.items():
            assert isinstance(raw, dict)
            prefixes = raw.get(
                "target_prefixes",
                [f"sbl_{family}_parser", f"sbp_{family}", f"sbu_{family}_parser", f"sbup_{family}"],
            )
            assert isinstance(prefixes, list)
            for prefix in prefixes:
                prefix_text = str(prefix)
                if token.startswith(prefix_text):
                    matches.append((len(prefix_text), family))
        return max(matches)[1] if matches else None

    def namespace_bases(self, family: str) -> list[str]:
        raw = self.families[family]
        assert isinstance(raw, dict)
        stems = raw.get("namespace_stems", [family])
        assert isinstance(stems, list)
        bases: list[str] = []
        for stem in stems:
            bases.extend((
                f"scratchbird::parser::{stem}",
                f"scratchbird::udr::{stem}_parser_support",
            ))
        return bases

    def path_fragments(self, family: str) -> list[str]:
        raw = self.families[family]
        assert isinstance(raw, dict)
        roots = raw.get("roots", [])
        assert isinstance(roots, list)
        fragments: set[str] = set()
        for root in roots:
            normalized = str(root).replace("\\", "/")
            fragments.add(normalized)
            if normalized.startswith("project/"):
                fragments.add(normalized[len("project/"):])
        return sorted(fragments)


def scan_text(registry: Registry, family: str, channel: str, text: str) -> list[Finding]:
    findings: list[Finding] = []
    if family in registry.strict_standalone_families:
        normalized = text.replace("\\", "/")
        legacy_markers = set(registry.legacy_shared_parser_targets)
        legacy_markers.update(registry.forbidden_neutral_parser_targets)
        legacy_markers.update(registry.legacy_shared_parser_roots)
        legacy_markers.update(
            root.removeprefix("project/")
            for root in registry.legacy_shared_parser_roots
        )
        legacy_markers.update(
            {
                "libsbl_compatibility_parser_common",
                "compatibility_dialect.cpp",
                "compatibility_dialect.hpp",
                "compatibility_worker_session.cpp",
                "compatibility_worker_session.hpp",
                "scratchbird::parser::compatibility::ParseStatement",
                "scratchbird::parser::compatibility::LexTokens",
                "scratchbird::parser::compatibility::HandleWorkerCommand",
                "scratchbird::parser::compatibility::ServeTextWorkerSession",
            }
        )
        for marker in sorted(legacy_markers):
            if marker in normalized:
                findings.append(
                    Finding(family, channel, "legacy_shared_parser", marker)
                )
    for token_match in TARGET_TOKEN_RE.finditer(text):
        token = token_match.group(0)
        owner = registry.target_owner(token)
        if owner is not None and owner != family:
            findings.append(Finding(family, channel, owner, token))
    for library_match in LIBRARY_TARGET_RE.finditer(text):
        token = library_match.group("target")
        owner = registry.target_owner(token)
        if owner is not None and owner != family:
            findings.append(Finding(family, channel, owner, library_match.group(0)))
    if family != "sbsql" and "sbsql" in registry.families:
        for pattern in (
            SBSQL_BRANDED_LIBRARY_RE,
            SBSQL_BRANDED_EXECUTABLE_PATH_RE,
            SBSQL_BRANDED_EXECUTABLE_LISTING_RE,
        ):
            for branded_match in pattern.finditer(text.replace("\\", "/")):
                findings.append(
                    Finding(family, channel, "sbsql", branded_match.group("artifact"))
                )
    normalized_text = text.replace("\\", "/")
    for foreign in registry.families:
        if foreign == family:
            continue
        for namespace in registry.namespace_bases(foreign):
            pattern = re.compile(re.escape(namespace) + r"(?=\s*(?:::|\{|;))")
            if pattern.search(text):
                findings.append(Finding(family, channel, foreign, namespace))
        for fragment in registry.path_fragments(foreign):
            # A family root must end at a path boundary. Without this guard,
            # the opensearch root would incorrectly own opensearch_sql_ppl.
            if re.search(re.escape(fragment) + r"(?=/|$)", normalized_text):
                findings.append(Finding(family, channel, foreign, fragment))
        endpoint = re.compile(
            rf"(?:parser|sbp)[/_-]+{re.escape(foreign)}(?:[/.:]|$)",
            re.IGNORECASE,
        )
        match = endpoint.search(text)
        if match:
            findings.append(Finding(family, channel, foreign, match.group(0)))
    unique = {(row.family, row.channel, row.foreign_family, row.evidence): row for row in findings}
    return sorted(unique.values(), key=lambda row: (row.channel, row.foreign_family, row.evidence))


def command(
    args: list[str],
    *,
    timeout: int = 120,
    cwd: pathlib.Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout, check=False, cwd=cwd, env=env)


def sha256_text(text: str) -> str:
    return "sha256:" + hashlib.sha256(text.encode("utf-8", errors="replace")).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def executable_name(family: str) -> str:
    return "SBParser" if family == "sbsql" else f"sbp_{family}"


def archive_name(family: str) -> str:
    return "libSBParser_pipeline.a" if family == "sbsql" else f"libsbl_{family}_parser_pipeline.a"


def executable_names(family: str) -> tuple[str, ...]:
    name = executable_name(family)
    return (name, f"{name}.exe")


def archive_names(family: str) -> tuple[str, ...]:
    linux_name = archive_name(family)
    windows_name = (
        "SBParser_pipeline.lib"
        if family == "sbsql"
        else f"sbl_{family}_parser_pipeline.lib"
    )
    return (linux_name, windows_name)


def select_unique(paths: list[pathlib.Path], description: str) -> pathlib.Path:
    files = sorted({path.resolve() for path in paths if path.is_file()}, key=lambda path: (len(path.parts), str(path)))
    if not files:
        raise ValueError(f"missing {description}")
    if len(files) != 1:
        rendered = ", ".join(str(path) for path in files)
        raise ValueError(f"ambiguous {description}: {rendered}")
    return files[0]


def discover_artifacts(build_dir: pathlib.Path, registry: Registry) -> dict[str, tuple[pathlib.Path, pathlib.Path]]:
    artifacts: dict[str, tuple[pathlib.Path, pathlib.Path]] = {}
    public_output = build_dir / "output"
    platform_roots = (
        sorted(path for path in public_output.iterdir() if path.is_dir())
        if public_output.is_dir()
        else []
    )
    for family in sorted(registry.families):
        binary_matches = [
            root / "bin" / name
            for root in platform_roots
            for name in executable_names(family)
            if (root / "bin" / name).is_file() and os.access(root / "bin" / name, os.X_OK)
        ]
        archive_matches = [
            root / "lib" / name
            for root in platform_roots
            for name in archive_names(family)
            if (root / "lib" / name).is_file()
        ]
        if not binary_matches and not archive_matches:
            continue
        binary = select_unique(binary_matches, f"{family} parser executable")
        archive = select_unique(archive_matches, f"{family} parser pipeline archive")
        artifacts[family] = (binary, archive)
    return artifacts


def link_command(build_dir: pathlib.Path, family: str) -> tuple[pathlib.Path | None, str]:
    target = "sbp_sbsql" if family == "sbsql" else f"sbp_{family}"
    matches = sorted(build_dir.rglob(f"{target}.dir/link.txt"))
    if matches:
        path = matches[0]
        return path, path.read_text(encoding="utf-8", errors="replace")
    # Ninja keeps the exact transitive link command in build.ninja.
    ninja = build_dir / "build.ninja"
    if ninja.is_file():
        text = ninja.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        blocks: list[str] = []
        link_edge = re.compile(
            rf"^build .+: CXX_EXECUTABLE_LINKER__{re.escape(target)}"
            r"(?:_(?:Debug|Release|RelWithDebInfo|MinSizeRel)?)?\s"
        )
        for index, line in enumerate(lines):
            if not link_edge.search(line):
                continue
            end = index + 1
            while end < len(lines) and lines[end].strip():
                end += 1
            blocks.append("\n".join(lines[index:end]))
        relevant = "\n\n".join(blocks)
        return ninja, relevant
    return None, ""


def expected_family_uuid(family: str) -> str:
    return "parser.native.scratchbird" if family == "sbsql" else f"parser.compatibility.{family}"


def expected_same_family_targets(family: str, *, include_udr: bool = True) -> set[str]:
    if family == "sbsql":
        targets = {
            "sbp_sbsql",
            "sbl_sbsql_parser_pipeline",
            "sbl_sbsql_parser_worker_core",
        }
        if include_udr:
            targets.add("sbu_sbsql_parser_support")
        return targets
    targets = {
        f"sbp_{family}",
        f"sbl_{family}_parser_pipeline",
    }
    if family == "firebird":
        targets.add("sbl_firebird_transaction_policy")
    if include_udr:
        targets.add(f"sbu_{family}_parser_support")
    return targets


def parse_package_identity(family: str, output: str) -> tuple[dict[str, object] | None, list[str]]:
    try:
        payload = strict_json_loads(output.strip())
    except DuplicateJsonKeyError as exc:
        return None, [f"duplicate_json_key:{exc.key}", *CANONICAL_MANIFEST_FIELDS]
    except json.JSONDecodeError:
        return None, list(CANONICAL_MANIFEST_FIELDS)
    if not isinstance(payload, dict):
        return None, list(CANONICAL_MANIFEST_FIELDS)
    dialect = payload.get("dialect")
    if dialect != family:
        return payload, ["dialect_owner_mismatch"]
    missing = [field for field in CANONICAL_MANIFEST_FIELDS if field not in payload]
    family_uuid = expected_family_uuid(family)
    if payload.get("parser_family_uuid") != family_uuid:
        if "parser_family_uuid" not in missing:
            missing.append("parser_family_uuid=registered_owner")
    if payload.get("standalone_package") is not True:
        if "standalone_package" not in missing:
            missing.append("standalone_package=true")
    cross_count = payload.get("cross_parser_dependency_count")
    if isinstance(cross_count, bool) or cross_count != 0:
        if "cross_parser_dependency_count" not in missing:
            missing.append("cross_parser_dependency_count=0")
    udr_uuid = payload.get("parser_support_udr_family_uuid")
    expected_targets = expected_same_family_targets(
        family, include_udr=udr_uuid is not None
    )
    same_family = payload.get("same_family_library_set")
    if isinstance(same_family, list) and same_family:
        targets: set[str] = set()
        for index, entry in enumerate(same_family):
            if not isinstance(entry, dict):
                missing.append(f"same_family_library_set[{index}]=object")
                continue
            target = entry.get("target")
            artifact = entry.get("artifact")
            owner = entry.get("owner")
            if not isinstance(target, str) or not target:
                missing.append(f"same_family_library_set[{index}].target")
            else:
                targets.add(target)
            if not isinstance(artifact, str) or not artifact:
                missing.append(f"same_family_library_set[{index}].artifact")
            if owner != family_uuid:
                missing.append(f"same_family_library_set[{index}].owner")
        absent_targets = expected_targets - targets
        if absent_targets:
            missing.append("same_family_library_set=complete:" + ",".join(sorted(absent_targets)))
        unexpected_targets = targets - expected_targets
        if unexpected_targets:
            missing.append(
                "same_family_library_set=unexpected:" + ",".join(sorted(unexpected_targets))
            )
    elif "same_family_library_set" not in missing:
        missing.append("same_family_library_set=nonempty_array")
    neutral = payload.get("neutral_dependency_set")
    if isinstance(neutral, list):
        for index, entry in enumerate(neutral):
            if not isinstance(entry, dict):
                missing.append(f"neutral_dependency_set[{index}]=object")
                continue
            for field in ("target", "artifact", "owner", "version"):
                if not isinstance(entry.get(field), str) or not entry[field]:
                    missing.append(f"neutral_dependency_set[{index}].{field}")
            if entry.get("owner") not in {
                "family_neutral", "scratchbird_engine", "system_neutral"
            }:
                missing.append(f"neutral_dependency_set[{index}].owner=neutral")
    elif "neutral_dependency_set" not in missing:
        missing.append("neutral_dependency_set=array")
    if udr_uuid is not None and udr_uuid != family_uuid:
        if "parser_support_udr_family_uuid" not in missing:
            missing.append("parser_support_udr_family_uuid=null_or_owner")
    if payload.get("direct_sblr_lowering") is not True:
        if "direct_sblr_lowering" not in missing:
            missing.append("direct_sblr_lowering=true")
    if payload.get("foreign_parser_fallback") is not False:
        if "foreign_parser_fallback" not in missing:
            missing.append("foreign_parser_fallback=false")
    expected_profiles = {
        "isolated_build_profile": ISOLATED_BUILD_PROFILE,
        "isolated_package_profile": ISOLATED_PACKAGE_PROFILE,
    }
    for field, profile in expected_profiles.items():
        if payload.get(field) != profile and field not in missing:
            missing.append(f"{field}=declared_profile")
    evidence = payload.get("dependency_closure_evidence")
    if isinstance(evidence, dict):
        absent_channels = [
            channel for channel in DEPENDENCY_EVIDENCE_CHANNELS
            if not isinstance(evidence.get(channel), str) or not evidence[channel]
        ]
        if absent_channels:
            missing.append("dependency_closure_evidence=complete:" + ",".join(absent_channels))
        for channel, reference in DEPENDENCY_EVIDENCE_REFERENCES.items():
            if evidence.get(channel) != reference:
                missing.append(f"dependency_closure_evidence.{channel}=scoped_reference")
    elif "dependency_closure_evidence" not in missing:
        missing.append("dependency_closure_evidence=object")
    return payload, sorted(set(missing))


def project_link_targets(link_text: str) -> set[str]:
    targets = {match.group("target") for match in LIBRARY_TARGET_RE.finditer(link_text)}
    branded_sbsql_targets = {
        "libSBParser_pipeline.a": "sbl_sbsql_parser_pipeline",
        "libSBParser_core.a": "sbl_sbsql_parser_worker_core",
    }
    for artifact, target in branded_sbsql_targets.items():
        if artifact in link_text:
            targets.add(target)
    return targets


def manifest_link_dependency_gaps(
    manifest: dict[str, object] | None, link_text: str
) -> list[str]:
    if manifest is None:
        return []
    declared: set[str] = set()
    for field in ("same_family_library_set", "neutral_dependency_set"):
        entries = manifest.get(field)
        if not isinstance(entries, list):
            continue
        for entry in entries:
            if isinstance(entry, dict) and isinstance(entry.get("target"), str):
                declared.add(entry["target"])
    undeclared = project_link_targets(link_text) - declared
    if not undeclared:
        return []
    return ["dependency_sets=complete_link_closure:" + ",".join(sorted(undeclared))]


def validate_family(
    registry: Registry,
    family: str,
    binary: pathlib.Path,
    archive: pathlib.Path,
    build_dir: pathlib.Path,
    evidence_dir: pathlib.Path,
) -> tuple[dict[str, object], list[Finding]]:
    reports = evidence_dir / "reports"
    stage = evidence_dir / "stage" / family
    traces = evidence_dir / "runtime"
    reports.mkdir(parents=True, exist_ok=True)
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "bin").mkdir(parents=True, exist_ok=True)
    traces.mkdir(parents=True, exist_ok=True)
    staged_binary = stage / "bin" / binary.name
    shutil.copy2(binary, staged_binary)
    binary_hash = sha256_file(binary)
    staged_binary_hash = sha256_file(staged_binary)
    if staged_binary_hash != binary_hash:
        raise ValueError(f"staged binary hash mismatch for {family}")

    findings: list[Finding] = []
    link_path, link_text = link_command(build_dir, family)
    if not link_text.strip():
        raise ValueError(f"missing link command evidence for {family}")
    findings.extend(scan_text(registry, family, "link_command", link_text))

    nm_binary = command(["nm", "-C", str(binary)], timeout=300)
    nm_archive = command(["nm", "-C", str(archive)], timeout=300)
    if nm_binary.returncode != 0 or nm_archive.returncode != 0:
        raise ValueError(f"nm failed for {family}")
    findings.extend(scan_text(registry, family, "binary_symbols", nm_binary.stdout))
    findings.extend(scan_text(registry, family, "archive_symbols", nm_archive.stdout))

    readelf = command(["readelf", "-d", str(staged_binary)])
    ldd = command(["ldd", str(staged_binary)])
    if readelf.returncode != 0 or ldd.returncode != 0:
        raise ValueError(f"dynamic dependency inspection failed for {family}")
    findings.extend(scan_text(registry, family, "dynamic_dependencies", readelf.stdout + "\n" + ldd.stdout))

    runtime_args = [str(staged_binary), "--package-identity"]
    trace_path = traces / f"{family}.strace"
    runtime_env = {
        "HOME": str(stage),
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    runtime = command([
        "strace", "-f", "-qq", "-s", "4096", "-o", str(trace_path),
        "-e", "trace=process,file,network,ipc", *runtime_args,
    ], timeout=60, cwd=stage, env=runtime_env)
    trace_text = trace_path.read_text(encoding="utf-8", errors="replace") if trace_path.is_file() else ""
    if runtime.returncode != 0:
        raise ValueError(f"staged runtime probe failed for {family}: {runtime.stdout[:240]}")
    findings.extend(scan_text(registry, family, "runtime_trace", trace_text))

    package_paths = [path.relative_to(stage).as_posix() for path in sorted(stage.rglob("*")) if path.is_file()]
    package_hashes = {
        path.relative_to(stage).as_posix(): sha256_file(path)
        for path in sorted(stage.rglob("*"))
        if path.is_file()
    }
    package_listing = "\n".join(package_paths)
    findings.extend(scan_text(registry, family, "package_contents", package_listing))

    manifest, manifest_gaps = parse_package_identity(family, runtime.stdout)
    manifest_gaps.extend(manifest_link_dependency_gaps(manifest, link_text))
    manifest_gaps = sorted(set(manifest_gaps))
    if manifest is not None:
        findings.extend(scan_text(
            registry, family, "package_manifest", json.dumps(manifest, sort_keys=True)
        ))

    (reports / f"{family}.link.txt").write_text(link_text, encoding="utf-8")
    (reports / f"{family}.dynamic.txt").write_text(readelf.stdout + "\n" + ldd.stdout,
                                                    encoding="utf-8")
    (reports / f"{family}.runtime.stdout.txt").write_text(runtime.stdout,
                                                            encoding="utf-8")
    (reports / f"{family}.package.json").write_text(json.dumps({
        "family": family,
        "stage": str(stage),
        "files": package_paths,
        "file_sha256": package_hashes,
        "package_identity": manifest,
        "canonical_manifest_gaps": manifest_gaps,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    row: dict[str, object] = {
        "family": family,
        "binary": str(binary),
        "archive": str(archive),
        "binary_sha256": binary_hash,
        "archive_sha256": sha256_file(archive),
        "staged_binary": str(staged_binary),
        "staged_binary_sha256": staged_binary_hash,
        "link_command_source": str(link_path) if link_path else None,
        "link_command_present": bool(link_text),
        "binary_nm_hash": sha256_text(nm_binary.stdout),
        "binary_nm_line_count": len(nm_binary.stdout.splitlines()),
        "archive_nm_hash": sha256_text(nm_archive.stdout),
        "archive_nm_line_count": len(nm_archive.stdout.splitlines()),
        "dynamic_dependency_report": str(reports / f"{family}.dynamic.txt"),
        "package_manifest_report": str(reports / f"{family}.package.json"),
        "runtime_trace": str(trace_path),
        "runtime_stdout_report": str(reports / f"{family}.runtime.stdout.txt"),
        "runtime_exit_code": runtime.returncode,
        "canonical_manifest_gaps": manifest_gaps,
        "foreign_dependency_count": len(findings),
    }
    return row, findings


def negative_fixture_result(registry: Registry) -> dict[str, object]:
    families = sorted(registry.families)
    owner, foreign = families[0], families[1]
    fixtures = {
        "foreign_static_link": f"lib{sbl_name(foreign)}.a",
        "foreign_dynamic_link": f"lib{sbl_name(foreign)}.so",
        "foreign_symbol": f"scratchbird::parser::{foreign}::ParseStatement()",
        "foreign_package": f"lib/{sbl_name(foreign)}.so",
        "foreign_process": f'execve("/stage/bin/sbp_{foreign}", ["sbp_{foreign}"], 0)',
        "foreign_socket": f'connect(3, "/run/scratchbird/parser_{foreign}.sock", 42)',
        "foreign_sbsql_branded_static_link": "lib/libSBParser_pipeline.a",
        "foreign_sbsql_branded_process": (
            'execve("/stage/bin/SBParser", ["SBParser"], 0)'
        ),
    }
    detected = {
        name: bool(scan_text(registry, owner, name, text))
        for name, text in fixtures.items()
    }
    return {
        "owner": owner,
        "foreign_family": foreign,
        "fixture_count": len(fixtures),
        "detected_count": sum(detected.values()),
        "fixtures": detected,
        "passed": all(detected.values()),
    }


def sbl_name(family: str) -> str:
    return f"sbl_{family}_parser_pipeline"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--ownership-map", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-dir", required=True, type=pathlib.Path)
    parser.add_argument("--family", help="Validate one parser family only")
    parser.add_argument("--expected-family-count", type=int, default=0)
    parser.add_argument("--require-complete-registry", action="store_true")
    parser.add_argument("--allow-no-artifacts", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    build_dir = args.build_dir.resolve()
    ownership = args.ownership_map
    if not ownership.is_absolute():
        ownership = repo_root / ownership
    evidence_dir = args.evidence_dir.resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)

    try:
        ownership_payload = strict_json_loads(ownership.read_text(encoding="utf-8"))
        if not isinstance(ownership_payload, dict):
            raise ValueError("ownership map root must be an object")
        registry = Registry(ownership_payload)
        artifacts = discover_artifacts(build_dir, registry)
        if args.family:
            if args.family not in registry.families:
                raise ValueError(f"unknown parser family: {args.family}")
            if args.family not in artifacts:
                raise ValueError(f"parser artifacts not found for family: {args.family}")
            artifacts = {args.family: artifacts[args.family]}
        if not artifacts and args.allow_no_artifacts:
            print("parser_family_binary_identity_checkpoint=skipped no parser artifacts")
            return 77
        expected_family_count = args.expected_family_count
        if args.require_complete_registry:
            expected_family_count = len(registry.families)
        if expected_family_count and len(artifacts) != expected_family_count:
            raise ValueError(
                f"expected {expected_family_count} parser families, discovered {len(artifacts)}"
            )

        rows: list[dict[str, object]] = []
        findings: list[Finding] = []
        errors: list[str] = []
        for family, (binary, archive) in sorted(artifacts.items()):
            try:
                row, family_findings = validate_family(
                    registry, family, binary, archive, build_dir, evidence_dir
                )
                rows.append(row)
                findings.extend(family_findings)
            except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
                errors.append(f"{family}: {exc}")
        negative = negative_fixture_result(registry)
        manifest_gap_count = sum(len(row["canonical_manifest_gaps"]) for row in rows)
        identity_checkpoint_passed = not findings and not errors and bool(rows)
        manifest_passed = manifest_gap_count == 0
        checkpoint_passed = identity_checkpoint_passed and manifest_passed and negative["passed"]
        payload = {
            "gate": "parser_family_binary_identity_checkpoint",
            "scope": [
                "declared package identity contract",
                "project-target link command closure",
                "binary and pipeline archive symbol ownership scan",
                "synthetic staged identity-probe load trace",
            ],
            "scope_limitations": [
                "does not perform cmake --install into a fresh empty prefix",
                "does not stage or verify every declared package artifact and parser-support UDR",
                "does not execute an operational parser handshake, attach, or tool session",
                "does not collect a linker map or prove complete system-library closure",
                "negative fixtures are synthetic and do not replace injected build/package/runtime tests",
                "symbolic family IDs are convention-checked, not resolved against an external UUID registry",
                "declared evidence references are shape-checked but referenced files are not cross-resolved",
            ],
            "evidence_timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
            "build_dir": str(build_dir),
            "artifact_family_count": len(artifacts),
            "artifact_families": sorted(artifacts),
            "family_count": len(rows),
            "families": rows,
            "foreign_dependency_count": len(findings),
            "findings": [finding.to_json() for finding in findings],
            "errors": errors,
            "negative_fixtures": negative,
            "conformance": {
                "PARSER-ISO-004": (
                    "partial_identity_and_link_command_only"
                    if identity_checkpoint_passed else "failed"
                ),
                "PARSER-ISO-006": "not_evaluated_real_empty_prefix_install_required",
                "PARSER-ISO-007": "not_evaluated_operational_parser_session_required",
                "PARSER-ISO-008": "not_evaluated_operational_runtime_trace_required",
                "PARSER-ISO-014": (
                    "partial_synthetic_fixtures_only" if negative["passed"] else "failed"
                ),
            },
            "canonical_manifest_gap_count": manifest_gap_count,
            "manifest_contract_status": "passed" if manifest_passed else "failed",
            "link_symbol_identity_scan_status": (
                "passed" if identity_checkpoint_passed else "failed"
            ),
            "checkpoint_status": "passed" if checkpoint_passed else "failed",
            "canonical_isolation_status": "not_evaluated",
        }
        evidence_file = evidence_dir / "parser_family_binary_isolation_evidence.json"
        evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(
            f"parser_family_binary_identity_checkpoint={payload['checkpoint_status']} "
            f"families={len(rows)} foreign_dependencies={len(findings)} "
            f"manifest_gaps={manifest_gap_count} canonical_isolation=not_evaluated"
        )
        if payload["checkpoint_status"] != "passed":
            print(json.dumps(payload, indent=2, sort_keys=True), file=sys.stderr)
            return 1
        return 0
    except (OSError, ValueError, json.JSONDecodeError, DuplicateJsonKeyError) as exc:
        print(f"parser_family_binary_identity_checkpoint: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
