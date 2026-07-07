#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Promote or verify ScratchBird beta release package payloads under packaging/."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Any, Iterable


REPORT_NAME = "driver_packaging_promotion.json"
MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
DEFAULT_BUILD_BIN_REL = Path("build/output/linux/bin")
DEFAULT_BUILD_OUTPUT_RELS = (
    Path("build/public-release-linux/output/linux"),
    Path("build/output/linux"),
)
DEFAULT_PROOF_REL = Path("build/reports")
DEFAULT_LANGUAGE_PACK_REL = Path(
    "project/resources/seed-packs/initial-resource-pack/resources/i18n/"
    "sbsql-language-resource-pack"
)
DEFAULT_PRODUCT_RESOURCES_REL = Path("project/resources")
LEGAL_SOURCE_FILES = ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md")
ROOT_SBOM_REL = Path("SBOM.json")
ROOT_METADATA = {"RELEASE_MANIFEST.json", "SHA256SUMS"}
ALLOWED_TOP_LEVELS = {
    "adapters",
    "docs",
    "drivers",
    "installers",
    "proofs",
    "reference-parsers",
    "server",
    "source",
    "tools",
    "udr",
}
STAGED_EXECUTABLES = {
    "adbc": "sb_isql_adbc",
    "cpp": "sb_isql_cpp",
    "dart": "sb_isql_dart",
    "dotnet": "sb_isql_dotnet",
    "elixir": "sb_isql_elixir",
    "flightsql": "sb_isql_flightsql",
    "go": "sb_isql_go",
    "jdbc": "sb_isql_jdbc",
    "julia": "sb_isql_julia",
    "mojo": "sb_isql_mojo",
    "node": "sb_isql_node",
    "odbc": "sb_isql_odbc",
    "pascal": "sb_isql_pascal",
    "perl": "sb_isql_perl",
    "php": "sb_isql_php",
    "python": "sb_isql_python",
    "r": "sb_isql_r",
    "r2dbc": "sb_isql_r2dbc",
    "ruby": "sb_isql_ruby",
    "rust": "sb_isql_rust",
    "swift": "sb_isql_swift",
}
OPTIONAL_SUPPORT_FILES = (
    "package_contract.json",
    "package.json",
    "pyproject.toml",
    "Project.toml",
    "Package.swift",
    "Package.resolved",
    "Makefile.PL",
    "pom.xml",
)
EXCLUDED_COPY_PARTS = {
    ".build",
    ".dart_tool",
    "__pycache__",
    "bin",
    "build",
    "dist",
    "node_modules",
    "obj",
    "target",
}
EXCLUDED_COPY_SUFFIXES = {".pyc", ".o", ".obj", ".class"}
PACKAGE_DIR_BY_CATEGORY = {
    "driver": "drivers",
    "adaptor": "adapters",
    "tool": "tools",
}
CLI_TOOL_BINARIES = ("SBsql", "SBadm", "SBbak", "SBsec", "SBcop", "SBdoc")
SERVER_ENGINE_LIB_PREFIXES = (
    "libSBcore",
    "libpublic_",
    "libsb_core_",
    "libsb_engine_",
    "libsb_storage_",
    "libsb_transaction_",
)
IPC_SERVER_BINARIES = ("SBsrv", "SBgate", "SBmgr")
IPC_SERVER_CONFIGS = ("SBsrv.conf", "SBgate.conf", "SBmgr.conf")
IPC_SERVER_LIB_PREFIXES = (
    "libsb_local_ipc",
    "libsb_local_parser_shared_memory_transport",
    "libsb_server_",
    "libsbl_listener_",
    "libsbl_manager_",
    "libsbl_parser_server_ipc_schema",
    "libsb_wire_result_batch",
)
SBPARSER_BINARIES = ("SBParser",)
SBPARSER_CONFIGS = ("SBParser.conf",)
SBPARSER_LIB_NAMES = (
    "libSBParser_core.a",
    "libSBParser_pipeline.a",
    "libSBParser_udr.a",
)
SBPARSER_LIB_PREFIXES = ("libSBParser_",)
OPTIONAL_UDR_LIB_PREFIX = "libsbu_"
OPTIONAL_UDR_EXCLUDED_LIBS = set(SBPARSER_LIB_NAMES)
ADAPTER_RUNTIME_SUFFIXES = (".zip", ".jar", ".tdc", ".mez", ".nupkg", ".whl")
ADAPTER_RUNTIME_NAMES = ("package.json", "pyproject.toml", "Project.toml")


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_text(repo_root: Path, *args: str) -> str | None:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def latest_release_dir(repo_root: Path) -> Path:
    packaging = repo_root / "packaging"
    candidates = [path for path in packaging.iterdir() if path.is_dir() and path.name[:4].isdigit()]
    if not candidates:
        return packaging / "2026.07.03"
    return sorted(candidates, key=lambda path: path.name)[-1]


def default_build_bin_root(repo_root: Path) -> Path:
    for output_root in DEFAULT_BUILD_OUTPUT_RELS:
        candidate = repo_root / output_root / "bin"
        if candidate.is_dir():
            return candidate
    return repo_root / DEFAULT_BUILD_BIN_REL


def build_output_root_from_bin(build_bin_root: Path) -> Path:
    return build_bin_root.parent if build_bin_root.name == "bin" else build_bin_root


def build_tree_root_from_bin(build_bin_root: Path) -> Path:
    output_root = build_output_root_from_bin(build_bin_root)
    if output_root.name == "linux" and output_root.parent.name == "output":
        return output_root.parent.parent
    return output_root.parent if output_root.name == "output" else output_root


def component_rows(repo_root: Path) -> list[dict[str, str]]:
    return read_csv(repo_root / MANIFEST_REL)


def source_path(repo_root: Path, manifest: dict[str, str]) -> Path:
    return repo_root / manifest.get("source_path", "").strip()


def rel_to_repo(repo_root: Path, path: Path) -> str:
    try:
        return path.relative_to(repo_root).as_posix()
    except ValueError:
        return path.as_posix()


def build_component_root(build_bin_root: Path, manifest: dict[str, str]) -> Path:
    source_rel = Path(manifest.get("source_path", "").strip())
    parts = source_rel.parts
    if parts and parts[0] == "project":
        source_rel = Path(*parts[1:])
    return build_tree_root_from_bin(build_bin_root) / source_rel


def copy_file(src: Path, dst: Path, verify_only: bool) -> bool:
    if not src.is_file():
        return False
    if verify_only:
        return dst.is_file() and dst.stat().st_size == src.stat().st_size
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return True


def write_text(path: Path, content: str, verify_only: bool) -> bool:
    if verify_only:
        return path.is_file()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def write_executable_text(path: Path, content: str, verify_only: bool) -> bool:
    if not write_text(path, content, verify_only):
        return False
    if not verify_only:
        path.chmod(path.stat().st_mode | 0o111)
    return True


def sanitized_text(text: str, repo_root: Path) -> str:
    private_repo_name = "ScratchBird" + "-Private"
    private_repo_path = "/home/" + "dcalford" + "/CliWork/" + private_repo_name
    home_path = "/home/" + "dcalford"
    replacements = (
        (str(repo_root), "${SCRATCHBIRD_REPO}"),
        (private_repo_path, "${SCRATCHBIRD_PRIVATE_REPO}"),
        (private_repo_name, "SCRATCHBIRD_PRIVATE_REPO"),
        (home_path, "${HOME}"),
        ("local" + "_work", "LOCAL_WORKSPACE"),
    )
    result = text
    for old, new in replacements:
        result = result.replace(old, new)
    return result


def copy_sanitized_artifact(src: Path, dst: Path, repo_root: Path, verify_only: bool) -> bool:
    if not src.is_file():
        return False
    if verify_only:
        return dst.is_file()
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        text = src.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        shutil.copy2(src, dst)
        return True
    dst.write_text(sanitized_text(text, repo_root), encoding="utf-8")
    return True


def should_copy(path: Path) -> bool:
    if any(part in EXCLUDED_COPY_PARTS for part in path.parts):
        return False
    if path.suffix in EXCLUDED_COPY_SUFFIXES:
        return False
    return True


def copy_tree(src: Path, dst: Path, verify_only: bool) -> bool:
    if not src.is_dir():
        return False
    files = [path for path in src.rglob("*") if path.is_file() and should_copy(path.relative_to(src))]
    if not files:
        return False
    if verify_only:
        return dst.is_dir() and any(path.is_file() for path in dst.rglob("*"))
    for item in files:
        rel = item.relative_to(src)
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, target)
    return True


def copy_runtime_tree(src: Path, dst: Path, verify_only: bool) -> bool:
    if not src.is_dir():
        return False
    files = [
        path for path in src.rglob("*")
        if path.is_file() and not any(part in EXCLUDED_COPY_PARTS for part in path.relative_to(src).parts)
    ]
    if not files:
        return False
    if verify_only:
        return dst.is_dir() and any(path.is_file() for path in dst.rglob("*"))
    for item in files:
        rel = item.relative_to(src)
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, target)
    return True


def reset_package_root(path: Path, verify_only: bool) -> None:
    if not verify_only and path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def collect_files(root: Path, *, include_root_metadata: bool = False) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not root.exists():
        return rows
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        if not include_root_metadata and rel in ROOT_METADATA:
            continue
        stat = path.stat()
        rows.append({"path": rel, "bytes": stat.st_size, "sha256": sha256(path)})
    return rows


def directory_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for row in collect_files(root, include_root_metadata=True):
        digest.update(row["path"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(row["sha256"].encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def write_sha256s(root: Path, verify_only: bool) -> bool:
    target = root / "SHA256SUMS"
    if verify_only:
        return target.is_file()
    rows = collect_files(root)
    target.write_text("".join(f"{row['sha256']}  {row['path']}\n" for row in rows), encoding="utf-8")
    return True


def proof_sources(repo_root: Path) -> list[Path]:
    reports = repo_root / DEFAULT_PROOF_REL
    names = (
        "driver_complete_coverage_tests.json",
        "driver_complete_coverage_checklist.json",
        "driver_complete_delta_implementation.json",
        "driver_wiki_documentation.json",
        "driver_complete_coverage_tests_preflight_after_profile.json",
    )
    return [reports / name for name in names if (reports / name).is_file()]


def example_roots(driver_source: Path) -> list[tuple[str, Path]]:
    candidates = (
        ("tools", driver_source / "tools"),
        ("cmd", driver_source / "cmd"),
        ("src-tools", driver_source / "src" / "tools"),
        ("swift-sb-isql", driver_source / "Sources" / "SBIsqlSwift"),
        ("tests", driver_source / "tests"),
        ("test", driver_source / "test"),
        ("perl-tests", driver_source / "t"),
    )
    return [(name, path) for name, path in candidates if path.is_dir()]


def write_package_sbom(package_root: Path, name: str, component_id: str, verify_only: bool) -> bool:
    target = package_root / "SBOM.json"
    if verify_only:
        return target.is_file()
    components = [
        {"path": row["path"], "sha256": row["sha256"], "bytes": row["bytes"]}
        for row in collect_files(package_root)
        if row["path"] not in {"SBOM.json", "SHA256SUMS"}
    ]
    payload = {
        "schema_id": "scratchbird.release_package_sbom.v1",
        "component_id": component_id,
        "name": name,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "components": components,
    }
    target.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return True


def write_release_manifest(repo_root: Path, release_root: Path, promoted_paths: Iterable[str], verify_only: bool) -> bool:
    manifest = release_root / "RELEASE_MANIFEST.json"
    sums = release_root / "SHA256SUMS"
    if verify_only:
        return manifest.is_file() and sums.is_file()
    files = collect_files(release_root)
    payload = {
        "schema_id": "scratchbird.prerelease_bundle_manifest.v1",
        "channel": "prerelease",
        "release_date": release_root.name,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "pre_release_not_final": True,
        "source": {
            "commit": git_text(repo_root, "rev-parse", "HEAD"),
            "dirty_before_promotion": bool(git_text(repo_root, "status", "--porcelain")),
        },
        "policy": {
            "distribution": "private_pre_release",
            "official_release": False,
            "history_cleanup_required_before_public_release": True,
            "build_directory_is_disposable": True,
            "promotion_requires_explicit_command": True,
        },
        "categories": sorted(ALLOWED_TOP_LEVELS),
        "promoted_paths": sorted(set(promoted_paths)),
        "artifacts": [
            {
                "path": row["path"],
                "category": row["path"].split("/", 1)[0] if "/" in row["path"] else "metadata",
                "bytes": row["bytes"],
                "sha256": row["sha256"],
            }
            for row in files
        ],
    }
    manifest.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    sums.write_text("".join(f"{row['sha256']}  {row['path']}\n" for row in files), encoding="utf-8")
    return True


def file_role(rel: str) -> str:
    name = Path(rel).name
    if rel.endswith("/README.md") or name.endswith(".md"):
        return "documentation"
    if "/bin/" in rel:
        return "executable"
    if "/lib/" in rel:
        return "library"
    if "/config/" in rel or name.endswith(".conf"):
        return "configuration"
    if "/legal/" in rel or name in {"LICENSE.txt", "NOTICE", "THIRD_PARTY_NOTICES.md"}:
        return "legal"
    if "/resources/" in rel:
        return "resource"
    if "/proofs/" in rel:
        return "proof"
    if "/examples/" in rel:
        return "example"
    if "/support/source" in rel or rel.endswith(".tar.gz"):
        return "source"
    if name in {"RELEASE_MANIFEST.json", "SHA256SUMS", "SBOM.json", "package_manifest.json"}:
        return "manifest"
    return "payload"


def installer_destination_hint(rel: str) -> str:
    parts = rel.split("/")
    if len(parts) >= 3 and parts[0] in {"drivers", "adapters", "tools", "server", "udr"}:
        return f"{parts[0]}/{parts[1]} package"
    if parts[0] == "installers":
        return "installer build inputs"
    if parts[0] == "source":
        return "source distribution inputs"
    if parts[0] == "docs":
        return "documentation bundle inputs"
    if parts[0] == "proofs":
        return "release proof bundle inputs"
    if parts[0] == "reference-parsers":
        return "reference parser package inputs"
    return "release root metadata"


def write_file_location_manifest(release_root: Path, verify_only: bool) -> bool:
    target = release_root / "FILE_LOCATION_MANIFEST.json"
    if verify_only:
        return target.is_file()
    rows = []
    for item in collect_files(release_root, include_root_metadata=True):
        if item["path"] == "FILE_LOCATION_MANIFEST.json" or item["path"] in ROOT_METADATA:
            continue
        rows.append(
            {
                "path": item["path"],
                "bytes": item["bytes"],
                "sha256": item["sha256"],
                "role": file_role(item["path"]),
                "installer_destination_hint": installer_destination_hint(item["path"]),
            }
        )
    payload = {
        "schema_id": "scratchbird.prerelease_file_location_manifest.v1",
        "release_date": release_root.name,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "files": rows,
    }
    target.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return True


def package_category_dir(manifest: dict[str, str]) -> str:
    category = manifest.get("category", "").strip()
    try:
        return PACKAGE_DIR_BY_CATEGORY[category]
    except KeyError as exc:
        raise ValueError(f"unknown package category: {category}") from exc


def portable_launcher_text(driver: str) -> str | None:
    py_tools = {"adbc": "sb_isql_adbc", "flightsql": "sb_isql_flightsql", "python": "sb_isql_python.py", "r2dbc": "sb_isql_r2dbc"}
    if driver in py_tools:
        return (
            "#!/usr/bin/env bash\nset -euo pipefail\n"
            "ROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\n"
            "export PYTHONPATH=\"$ROOT/support/source/src${PYTHONPATH:+:$PYTHONPATH}\"\n"
            f"exec \"${{PYTHON_BIN:-python3}}\" \"$ROOT/support/source/tools/{py_tools[driver]}\" \"$@\"\n"
        )
    if driver == "dotnet":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexec \"${DOTNET_BIN:-dotnet}\" \"$ROOT/runtime/dotnet/SBIsqlDotNet.dll\" \"$@\"\n"
    if driver == "elixir":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexport MIX_BUILD_PATH=\"$ROOT/runtime/elixir/_build\"\nexport MIX_DEPS_PATH=\"$ROOT/runtime/elixir/deps\"\nexport MIX_HOME=\"$ROOT/runtime/elixir/mix-home\"\nexport HEX_HOME=\"$ROOT/runtime/elixir/hex-home\"\ncd \"$ROOT/runtime/elixir/stage\"\nexec \"${MIX_BIN:-mix}\" run tools/sb_isql_elixir.exs -- \"$@\"\n"
    if driver == "jdbc":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nCP=\"$ROOT/runtime/jdbc/classes\"\nif [ -d \"$ROOT/runtime/jdbc/resources\" ]; then CP=\"$CP:$ROOT/runtime/jdbc/resources\"; fi\nexec \"${JAVA_BIN:-java}\" -cp \"$CP\" com.scratchbird.jdbc.tools.SBIsqlJdbc \"$@\"\n"
    if driver == "julia":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexec \"${JULIA_BIN:-julia}\" --project=\"$ROOT/runtime/julia/stage\" \"$ROOT/runtime/julia/stage/tools/sb_isql_julia.jl\" \"$@\"\n"
    if driver == "node":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexec \"${NODE_BIN:-node}\" \"$ROOT/runtime/node/dist/tools/sb-isql-node.js\" \"$@\"\n"
    if driver == "perl":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexport PERL5LIB=\"$ROOT/support/source/lib${PERL5LIB:+:$PERL5LIB}\"\nexec \"${PERL_BIN:-perl}\" \"$ROOT/support/source/tools/sb_isql_perl.pl\" \"$@\"\n"
    if driver == "php":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexec \"${PHP_BIN:-php}\" \"$ROOT/support/source/tools/sb_isql_php.php\" \"$@\"\n"
    if driver == "r":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexec \"${RSCRIPT_BIN:-Rscript}\" \"$ROOT/support/source/tools/sb_isql_r.R\" \"$@\"\n"
    if driver == "ruby":
        return "#!/usr/bin/env bash\nset -euo pipefail\nROOT=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")/..\" && pwd)\"\nexec \"${RUBY_BIN:-ruby}\" \"$ROOT/support/source/tools/sb_isql_ruby.rb\" \"$@\"\n"
    return None


def stage_driver_runtime(repo_root: Path, build_bin_root: Path, manifest: dict[str, str], driver_root: Path, verify_only: bool, issues: list[str]) -> list[str]:
    driver = manifest.get("name", "").strip()
    build_root = build_component_root(build_bin_root, manifest)
    runtime_paths: list[str] = []

    def stage(src: Path, rel_dst: str, label: str) -> None:
        if copy_runtime_tree(src, driver_root / rel_dst, verify_only):
            runtime_paths.append(rel_dst)
        else:
            issues.append(f"missing_{label}:{rel_to_repo(repo_root, src)}")

    if driver == "dotnet":
        stage(build_root / "publish", "runtime/dotnet", "dotnet_publish_runtime")
    elif driver == "elixir":
        stage(build_root / "stage", "runtime/elixir/stage", "elixir_stage_runtime")
        copy_runtime_tree(build_root / "deps", driver_root / "runtime" / "elixir" / "deps", verify_only)
        copy_runtime_tree(build_root / "_build", driver_root / "runtime" / "elixir" / "_build", verify_only)
    elif driver == "jdbc":
        stage(build_root / "stage" / "build" / "classes" / "java" / "main", "runtime/jdbc/classes", "jdbc_classes_runtime")
        copy_runtime_tree(build_root / "stage" / "build" / "resources" / "main", driver_root / "runtime" / "jdbc" / "resources", verify_only)
    elif driver == "julia":
        stage(build_root / "stage", "runtime/julia/stage", "julia_stage_runtime")
    elif driver == "node":
        stage(build_root / "stage" / "dist", "runtime/node/dist", "node_dist_runtime")
    return runtime_paths


def stage_adapter_runtime_artifacts(build_bin_root: Path, manifest: dict[str, str], package_root: Path, verify_only: bool) -> list[str]:
    build_root = build_component_root(build_bin_root, manifest)
    artifacts: list[str] = []
    if not build_root.is_dir():
        return artifacts
    candidates: list[Path] = []
    for rel_dir in ("dist", "stage/target", "stage/dist"):
        artifact_dir = build_root / rel_dir
        if artifact_dir.is_dir():
            candidates.extend(path for path in artifact_dir.iterdir() if path.is_file() and path.suffix.lower() in ADAPTER_RUNTIME_SUFFIXES)
    for name in ADAPTER_RUNTIME_NAMES:
        descriptor = build_root / "stage" / name
        if descriptor.is_file():
            candidates.append(descriptor)
    if manifest.get("name", "").strip() == "scratchbird-dbeaver-driver":
        update_sites = [path for path in candidates if path.name.startswith("scratchbird-dbeaver-update-site-") and path.suffix == ".zip"]
        if update_sites:
            latest = sorted(update_sites, key=lambda path: path.name)[-1]
            candidates = [path for path in candidates if path not in update_sites]
            candidates.append(latest)
    for src in sorted(set(candidates)):
        rel = src.relative_to(build_root)
        dst = package_root / "runtime" / "artifacts" / rel
        if copy_file(src, dst, verify_only):
            artifacts.append(f"runtime/artifacts/{rel.as_posix()}")
    return artifacts


def copy_build_file(src: Path, dst: Path, repo_root: Path, verify_only: bool, issues: list[str]) -> bool:
    if copy_file(src, dst, verify_only):
        return True
    issues.append(f"missing_build_file:{rel_to_repo(repo_root, src)}")
    return False


def copy_build_files_by_prefix(build_lib_root: Path, package_root: Path, prefixes: Iterable[str], repo_root: Path, verify_only: bool, issues: list[str], *, exclude_names: set[str] | None = None) -> list[str]:
    exclude_names = exclude_names or set()
    copied: list[str] = []
    if not build_lib_root.is_dir():
        issues.append(f"missing_build_lib_root:{rel_to_repo(repo_root, build_lib_root)}")
        return copied
    for src in sorted(path for path in build_lib_root.iterdir() if path.is_file()):
        if src.name in exclude_names:
            continue
        if not any(src.name.startswith(prefix) for prefix in prefixes):
            continue
        if copy_build_file(src, package_root / "lib" / src.name, repo_root, verify_only, issues):
            copied.append(f"lib/{src.name}")
    return copied


def make_source_archive(repo_root: Path, package_root: Path, archive_name: str, source_rels: Iterable[str], verify_only: bool, issues: list[str]) -> str:
    rel_archive = f"support/{archive_name}"
    target = package_root / rel_archive
    if verify_only:
        if not target.is_file():
            issues.append(f"missing_source_archive:{rel_archive}")
        return rel_archive
    target.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(target, "w:gz") as archive:
        for rel in source_rels:
            source = repo_root / rel
            if not source.exists():
                issues.append(f"missing_source_path:{rel}")
                continue
            archive.add(source, arcname=rel)
    return rel_archive


def write_install_readme(package_root: Path, title: str, lines: Iterable[str], verify_only: bool) -> None:
    body = "\n".join(lines)
    write_text(package_root / "README.md", f"# {title}\n\n{body.rstrip()}\n", verify_only)


def add_common_release_materials(repo_root: Path, package_root: Path, verify_only: bool, issues: list[str]) -> None:
    for filename in LEGAL_SOURCE_FILES:
        src = repo_root / filename
        dst_name = "LICENSE.txt" if filename == "LICENSE" else filename
        if not copy_file(src, package_root / "legal" / dst_name, verify_only):
            issues.append(f"missing_legal_material:{filename}")
    if not copy_file(repo_root / ROOT_SBOM_REL, package_root / "support" / "root-SBOM.json", verify_only):
        issues.append("missing_root_sbom_support_material")


def finish_package(package_root: Path, name: str, component_id: str, verify_only: bool) -> None:
    write_package_sbom(package_root, name, component_id, verify_only)
    write_sha256s(package_root, verify_only)


def write_core_package_manifest(package_root: Path, package_id: str, component_type: str, payloads: Iterable[str], source_archive: str, verify_only: bool) -> None:
    manifest = {
        "schema_id": "scratchbird.core_release_package_manifest.v1",
        "package_id": package_id,
        "component_type": component_type,
        "payloads": sorted(set(payloads)),
        "source_archive": source_archive,
        "source_commit": git_text(repo_root_from_script(), "rev-parse", "HEAD"),
        "license": "legal/LICENSE.txt",
        "notice": "legal/NOTICE",
        "third_party_notices": "legal/THIRD_PARTY_NOTICES.md",
        "sbom": "SBOM.json",
    }
    write_text(package_root / "package_manifest.json", json.dumps(manifest, indent=2, sort_keys=True) + "\n", verify_only)


def build_driver_package(repo_root: Path, release_root: Path, build_bin_root: Path, manifest: dict[str, str], verify_only: bool) -> tuple[dict[str, Any], list[str]]:
    component = manifest.get("component_id", "").strip()
    name = manifest.get("name", "").strip()
    issues: list[str] = []
    driver_root = release_root / "drivers" / name
    src_root = source_path(repo_root, manifest)
    binary_name = STAGED_EXECUTABLES.get(name, f"sb_isql_{name}")
    binary_src = build_bin_root / binary_name
    binary_dst = driver_root / "bin" / binary_name
    reset_package_root(driver_root, verify_only)

    runtime_paths = stage_driver_runtime(repo_root, build_bin_root, manifest, driver_root, verify_only, issues)
    launcher = portable_launcher_text(name)
    if launcher is not None:
        if not write_executable_text(binary_dst, launcher, verify_only):
            issues.append(f"missing_packaged_driver_launcher:bin/{binary_name}")
    elif not copy_file(binary_src, binary_dst, verify_only):
        issues.append(f"missing_compiled_driver_binary:{rel_to_repo(repo_root, binary_src)}")

    if not copy_file(src_root / "README.md", driver_root / "support" / "README.md", verify_only):
        issues.append("missing_driver_readme_support_material")
    if not copy_file(src_root / "BASELINE_REQUIREMENT_MAPPING.md", driver_root / "support" / "BASELINE_REQUIREMENT_MAPPING.md", verify_only):
        issues.append("missing_baseline_mapping_support_material")
    for filename in OPTIONAL_SUPPORT_FILES:
        copy_file(src_root / filename, driver_root / "support" / filename, verify_only)
    for label, path in example_roots(src_root):
        copy_tree(path, driver_root / "examples" / label, verify_only)
    write_text(
        driver_root / "examples" / "README.md",
        f"# ScratchBird {name} driver example\n\nThe canonical executable example for this package is `bin/{binary_name}`.\n",
        verify_only,
    )
    language_pack_src = repo_root / DEFAULT_LANGUAGE_PACK_REL
    if not copy_tree(language_pack_src, driver_root / "resources" / "sbsql-language-resource-pack", verify_only):
        issues.append(f"missing_language_resource_pack:{DEFAULT_LANGUAGE_PACK_REL.as_posix()}")
    add_common_release_materials(repo_root, driver_root, verify_only, issues)
    proof_files = proof_sources(repo_root)
    for proof in proof_files:
        copy_sanitized_artifact(proof, driver_root / "proofs" / proof.name, repo_root, verify_only)
    proof_summary = {"schema_id": "scratchbird.driver_package_proof_summary.v1", "component_id": component, "driver": name, "proof_files": sorted(path.name for path in proof_files)}
    write_text(driver_root / "proofs" / "proof_summary.json", json.dumps(proof_summary, indent=2, sort_keys=True) + "\n", verify_only)
    binary_hash = sha256(binary_dst) if binary_dst.is_file() else (sha256(binary_src) if binary_src.is_file() else "")
    language_digest = directory_digest(language_pack_src) if language_pack_src.is_dir() else ""
    package_manifest = {
        "schema_id": "scratchbird.driver_release_package_manifest.v1",
        "component_id": component,
        "driver": name,
        "source_path": manifest.get("source_path", ""),
        "version": manifest.get("driver_package_uuid", ""),
        "source_commit": git_text(repo_root, "rev-parse", "HEAD"),
        "binary": f"bin/{binary_name}",
        "binary_sha256": binary_hash,
        "runtime_payloads": sorted(runtime_paths),
        "language_resource_pack": "resources/sbsql-language-resource-pack",
        "language_resource_pack_sha256": language_digest,
        "support_materials": ["support/README.md", "support/BASELINE_REQUIREMENT_MAPPING.md", "support/root-SBOM.json"],
        "examples": ["examples/README.md"],
        "proofs": ["proofs/proof_summary.json"],
        "license": "legal/LICENSE.txt",
        "notice": "legal/NOTICE",
        "third_party_notices": "legal/THIRD_PARTY_NOTICES.md",
        "sbom": "SBOM.json",
    }
    write_text(driver_root / "package_manifest.json", json.dumps(package_manifest, indent=2, sort_keys=True) + "\n", verify_only)
    support_bundle_manifest = {
        "schema_id": "scratchbird.driver_support_bundle_manifest.v1",
        "component_id": component,
        "driver": name,
        "hash": binary_hash,
        "resource_pack_digest": language_digest,
        "proof_summary": "../proofs/proof_summary.json",
    }
    write_text(driver_root / "support" / "support_bundle_manifest.json", json.dumps(support_bundle_manifest, indent=2, sort_keys=True) + "\n", verify_only)
    finish_package(driver_root, name, component, verify_only)

    for required_rel in ("SBOM.json", "SHA256SUMS", "package_manifest.json", f"bin/{binary_name}", "examples/README.md", "proofs/proof_summary.json", "resources/sbsql-language-resource-pack/manifest.sblrp.json", "legal/LICENSE.txt"):
        if verify_only and not (driver_root / required_rel).is_file():
            issues.append(f"missing_packaged_file:{required_rel}")
    return ({"component_id": component, "category": "driver", "driver": name, "release_path": str(driver_root.relative_to(repo_root)), "binary_sha256": binary_hash, "language_resource_pack_sha256": language_digest, "issues": issues}, issues)


def build_component_package(repo_root: Path, release_root: Path, build_bin_root: Path, manifest: dict[str, str], verify_only: bool) -> tuple[dict[str, Any], list[str]]:
    component = manifest.get("component_id", "").strip()
    name = manifest.get("name", "").strip()
    category = manifest.get("category", "").strip()
    package_root = release_root / package_category_dir(manifest) / name
    src_root = source_path(repo_root, manifest)
    issues: list[str] = []
    reset_package_root(package_root, verify_only)
    copy_tree(src_root, package_root / "support" / "source", verify_only)
    copy_file(src_root / "README.md", package_root / "support" / "README.md", verify_only)
    copy_file(src_root / "BASELINE_REQUIREMENT_MAPPING.md", package_root / "support" / "BASELINE_REQUIREMENT_MAPPING.md", verify_only)
    for filename in OPTIONAL_SUPPORT_FILES:
        copy_file(src_root / filename, package_root / "support" / filename, verify_only)
    binaries: list[str] = []
    native_artifacts: list[str] = []
    if category == "tool":
        for binary in CLI_TOOL_BINARIES:
            src = build_bin_root / binary
            dst = package_root / "bin" / binary
            if copy_file(src, dst, verify_only):
                binaries.append(f"bin/{binary}")
            else:
                issues.append(f"missing_tool_binary:{rel_to_repo(repo_root, src)}")
    elif category == "adaptor":
        native_artifacts = stage_adapter_runtime_artifacts(build_bin_root, manifest, package_root, verify_only)
        write_text(package_root / "bin" / "README.md", f"# ScratchBird {name} adapter\n\nGenerated adapter payloads, when available, are staged under runtime/artifacts/.\n", verify_only)
    for label, path in example_roots(src_root):
        copy_tree(path, package_root / "examples" / label, verify_only)
    write_text(package_root / "examples" / "README.md", f"# ScratchBird {name} {category} examples\n", verify_only)
    language_pack_src = repo_root / DEFAULT_LANGUAGE_PACK_REL
    if not copy_tree(language_pack_src, package_root / "resources" / "sbsql-language-resource-pack", verify_only):
        issues.append(f"missing_language_resource_pack:{DEFAULT_LANGUAGE_PACK_REL.as_posix()}")
    add_common_release_materials(repo_root, package_root, verify_only, issues)
    proof_files = proof_sources(repo_root)
    for proof in proof_files:
        copy_sanitized_artifact(proof, package_root / "proofs" / proof.name, repo_root, verify_only)
    source_digest_value = directory_digest(src_root) if src_root.is_dir() else ""
    language_digest = directory_digest(language_pack_src) if language_pack_src.is_dir() else ""
    package_manifest = {
        "schema_id": "scratchbird.component_release_package_manifest.v1",
        "component_id": component,
        "category": category,
        "name": name,
        "source_path": manifest.get("source_path", ""),
        "source_commit": git_text(repo_root, "rev-parse", "HEAD"),
        "source_sha256": source_digest_value,
        "binaries": binaries,
        "native_artifacts": sorted(native_artifacts),
        "language_resource_pack": "resources/sbsql-language-resource-pack",
        "language_resource_pack_sha256": language_digest,
        "support_materials": ["support/source", "support/root-SBOM.json"],
        "examples": ["examples/README.md"],
        "proofs": sorted(f"proofs/{path.name}" for path in proof_files),
        "license": "legal/LICENSE.txt",
        "notice": "legal/NOTICE",
        "third_party_notices": "legal/THIRD_PARTY_NOTICES.md",
        "sbom": "SBOM.json",
        "version": manifest.get("driver_package_uuid", ""),
    }
    write_text(package_root / "package_manifest.json", json.dumps(package_manifest, indent=2, sort_keys=True) + "\n", verify_only)
    support_bundle_manifest = {"schema_id": "scratchbird.component_support_bundle_manifest.v1", "component_id": component, "category": category, "name": name, "hash": source_digest_value, "resource_pack_digest": language_digest}
    write_text(package_root / "support" / "support_bundle_manifest.json", json.dumps(support_bundle_manifest, indent=2, sort_keys=True) + "\n", verify_only)
    proof_summary = {"schema_id": "scratchbird.component_package_proof_summary.v1", "component_id": component, "category": category, "name": name, "proof_files": sorted(path.name for path in proof_files)}
    write_text(package_root / "proofs" / "proof_summary.json", json.dumps(proof_summary, indent=2, sort_keys=True) + "\n", verify_only)
    finish_package(package_root, name, component, verify_only)
    for required_rel in ("SBOM.json", "SHA256SUMS", "package_manifest.json", "examples/README.md", "proofs/proof_summary.json", "resources/sbsql-language-resource-pack/manifest.sblrp.json", "legal/LICENSE.txt", "support/support_bundle_manifest.json"):
        if verify_only and not (package_root / required_rel).is_file():
            issues.append(f"missing_packaged_file:{required_rel}")
    return ({"component_id": component, "category": category, "name": name, "release_path": str(package_root.relative_to(repo_root)), "source_sha256": source_digest_value, "language_resource_pack_sha256": language_digest, "issues": issues}, issues)


def build_core_runtime_packages(repo_root: Path, release_root: Path, build_bin_root: Path, verify_only: bool) -> tuple[list[dict[str, Any]], list[str]]:
    issues: list[str] = []
    packages: list[dict[str, Any]] = []
    build_output_root = build_output_root_from_bin(build_bin_root)
    build_lib_root = build_output_root / "lib"
    config_root = repo_root / "project" / "config" / "templates"

    def add_package(package_root: Path, component_id: str, category: str, name: str, payloads: list[str]) -> None:
        finish_package(package_root, name, component_id, verify_only)
        packages.append({"component_id": component_id, "category": category, "name": name, "release_path": str(package_root.relative_to(repo_root)), "payloads": sorted(set(payloads)), "issues": []})

    engine_root = release_root / "server" / "engine"
    reset_package_root(engine_root, verify_only)
    engine_payloads = copy_build_files_by_prefix(build_lib_root, engine_root, SERVER_ENGINE_LIB_PREFIXES, repo_root, verify_only, issues)
    engine_source = make_source_archive(repo_root, engine_root, "scratchbird-engine-source.tar.gz", ("project/src/catalog", "project/src/core", "project/src/engine", "project/src/storage", "project/src/transaction", "project/src/server_engine_bridge"), verify_only, issues)
    write_install_readme(engine_root, "ScratchBird Engine Package", ("This package stages the ScratchBird engine libraries and core source material for installer construction.",), verify_only)
    add_common_release_materials(repo_root, engine_root, verify_only, issues)
    write_core_package_manifest(engine_root, "server:engine", "engine", engine_payloads, engine_source, verify_only)
    add_package(engine_root, "server:engine", "server", "engine", engine_payloads)

    ipc_root = release_root / "server" / "ipc-server"
    reset_package_root(ipc_root, verify_only)
    ipc_payloads: list[str] = []
    for binary in IPC_SERVER_BINARIES:
        if copy_build_file(build_bin_root / binary, ipc_root / "bin" / binary, repo_root, verify_only, issues):
            ipc_payloads.append(f"bin/{binary}")
    for config in IPC_SERVER_CONFIGS:
        if copy_build_file(config_root / config, ipc_root / "config" / config, repo_root, verify_only, issues):
            ipc_payloads.append(f"config/{config}")
    ipc_payloads.extend(copy_build_files_by_prefix(build_lib_root, ipc_root, IPC_SERVER_LIB_PREFIXES, repo_root, verify_only, issues))
    ipc_source = make_source_archive(repo_root, ipc_root, "scratchbird-ipc-server-source.tar.gz", ("project/src/server", "project/src/listener", "project/src/manager", "project/src/ipc", "project/src/wire", "project/config/templates/SBsrv.conf", "project/config/templates/SBgate.conf", "project/config/templates/SBmgr.conf"), verify_only, issues)
    write_install_readme(ipc_root, "ScratchBird IPC Server Package", ("This package stages the server, listener, and manager binaries used by the open-core IPC/listener route.",), verify_only)
    add_common_release_materials(repo_root, ipc_root, verify_only, issues)
    write_core_package_manifest(ipc_root, "server:ipc-server", "ipc_server", ipc_payloads, ipc_source, verify_only)
    add_package(ipc_root, "server:ipc-server", "server", "ipc-server", ipc_payloads)

    parser_root = release_root / "server" / "sbparser"
    reset_package_root(parser_root, verify_only)
    parser_payloads: list[str] = []
    for binary in SBPARSER_BINARIES:
        if copy_build_file(build_bin_root / binary, parser_root / "bin" / binary, repo_root, verify_only, issues):
            parser_payloads.append(f"bin/{binary}")
    for config in SBPARSER_CONFIGS:
        if copy_build_file(config_root / config, parser_root / "config" / config, repo_root, verify_only, issues):
            parser_payloads.append(f"config/{config}")
    for lib in SBPARSER_LIB_NAMES:
        if copy_build_file(build_lib_root / lib, parser_root / "lib" / lib, repo_root, verify_only, issues):
            parser_payloads.append(f"lib/{lib}")
    parser_payloads.extend(copy_build_files_by_prefix(build_lib_root, parser_root, SBPARSER_LIB_PREFIXES, repo_root, verify_only, issues))
    parser_source = make_source_archive(repo_root, parser_root, "scratchbird-sbparser-source.tar.gz", ("project/src/parsers/native", "project/src/parsers/sbsql_worker", "project/src/parsers/shared", "project/src/udr/runtime", "project/src/udr/sbu_sbsql_parser_support", "project/config/templates/SBParser.conf"), verify_only, issues)
    write_install_readme(parser_root, "ScratchBird SBParser Package", ("This package stages SBParser and the SBsql parser UDR used by servers to lower dynamic SBsql outside the engine.", "SBParser belongs to the server runtime surface, not the CLI tools package."), verify_only)
    add_common_release_materials(repo_root, parser_root, verify_only, issues)
    write_core_package_manifest(parser_root, "server:sbparser", "server_parser", parser_payloads, parser_source, verify_only)
    add_package(parser_root, "server:sbparser", "server", "sbparser", parser_payloads)

    resources_root = release_root / "server" / "resources"
    reset_package_root(resources_root, verify_only)
    resources_payloads: list[str] = []
    resource_src = repo_root / DEFAULT_PRODUCT_RESOURCES_REL
    if copy_tree(resource_src, resources_root / "resources", verify_only):
        resources_payloads.extend(
            f"resources/{row['path']}"
            for row in collect_files(resources_root / "resources", include_root_metadata=True)
        )
    else:
        issues.append(f"missing_product_resources:{DEFAULT_PRODUCT_RESOURCES_REL.as_posix()}")
    resources_source = make_source_archive(
        repo_root,
        resources_root,
        "scratchbird-product-resources.tar.gz",
        (DEFAULT_PRODUCT_RESOURCES_REL.as_posix(),),
        verify_only,
        issues,
    )
    write_install_readme(
        resources_root,
        "ScratchBird Product Resources Package",
        (
            "This package stages the create-time and runtime resource packs used by the server, parser, drivers, adapters, and tools.",
            "It includes default policy packs, initial resource seed packs, character sets, collations, IANA timezone data, and SBsql language resources.",
            "Install these files under the shared ScratchBird resource root, for example /opt/ScratchBird/resources.",
        ),
        verify_only,
    )
    add_common_release_materials(repo_root, resources_root, verify_only, issues)
    write_core_package_manifest(resources_root, "server:resources", "server_resources", resources_payloads, resources_source, verify_only)
    add_package(resources_root, "server:resources", "server", "resources", resources_payloads)

    udr_root = release_root / "udr" / "optional-parser-support"
    reset_package_root(udr_root, verify_only)
    udr_payloads = copy_build_files_by_prefix(build_lib_root, udr_root, (OPTIONAL_UDR_LIB_PREFIX,), repo_root, verify_only, issues, exclude_names=OPTIONAL_UDR_EXCLUDED_LIBS)
    udr_source = make_source_archive(repo_root, udr_root, "scratchbird-optional-parser-udr-source.tar.gz", ("project/src/udr",), verify_only, issues)
    write_install_readme(udr_root, "ScratchBird Optional Parser-Support UDR Package", ("This package stages optional parser-support UDR libraries. SBsql parser UDR payloads are staged under server/sbparser.",), verify_only)
    add_common_release_materials(repo_root, udr_root, verify_only, issues)
    write_core_package_manifest(udr_root, "udr:optional-parser-support", "optional_udr", udr_payloads, udr_source, verify_only)
    add_package(udr_root, "udr:optional-parser-support", "udr", "optional-parser-support", udr_payloads)
    return packages, issues


def build_report(repo_root: Path, matrix_path: Path, release_root: Path, build_bin_root: Path, verify_only: bool) -> dict[str, Any]:
    matrix_rows: dict[str, dict[str, str]] = {}
    for row in read_csv(matrix_path):
        component_id = row.get("component_id", "").strip()
        if component_id:
            matrix_rows[component_id] = row
            continue
        driver = row.get("driver", "").strip()
        if driver:
            matrix_rows[f"driver:{driver}"] = row
    issues: list[str] = []
    promoted: list[dict[str, Any]] = []
    promoted_paths: list[str] = []
    core_packages, core_issues = build_core_runtime_packages(repo_root, release_root, build_bin_root, verify_only)
    promoted.extend(core_packages)
    promoted_paths.extend(package["release_path"] for package in core_packages)
    issues.extend(core_issues)
    for manifest in component_rows(repo_root):
        component = manifest.get("component_id", "").strip()
        category = manifest.get("category", "").strip()
        if category == "driver" and component not in matrix_rows:
            issues.append(f"{component}:missing_complete_coverage_matrix_row")
        if category == "driver":
            package, row_issues = build_driver_package(repo_root, release_root, build_bin_root, manifest, verify_only)
        else:
            package, row_issues = build_component_package(repo_root, release_root, build_bin_root, manifest, verify_only)
        issues.extend(f"{component}:{issue}" for issue in row_issues)
        promoted.append(package)
        promoted_paths.append(package["release_path"])
    if not write_file_location_manifest(release_root, verify_only):
        issues.append("file_location_manifest_missing")
    if not write_release_manifest(repo_root, release_root, promoted_paths, verify_only):
        issues.append("release_manifest_or_sha256s_missing")
    if verify_only:
        try:
            manifest = json.loads((release_root / "RELEASE_MANIFEST.json").read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            issues.append("release_manifest_invalid_or_missing")
            manifest = {}
        promoted_set = set(manifest.get("promoted_paths", [])) if isinstance(manifest, dict) else set()
        for package in promoted:
            if package["release_path"] not in promoted_set:
                issues.append(f"{package['component_id']}:release_manifest_missing_promoted_path")
    return {
        "command": "promote_driver_release_artifacts.py",
        "gate_id": "BETA-DTA-GATE-036",
        "status": "fail" if issues else "pass",
        "summary": {
            "verify_only": verify_only,
            "release_root": str(release_root.relative_to(repo_root)),
            "components": len(promoted),
            "drivers": sum(1 for item in promoted if item.get("category") == "driver"),
            "adapters": sum(1 for item in promoted if item.get("category") == "adaptor"),
            "tools": sum(1 for item in promoted if item.get("category") == "tool"),
            "server_packages": sum(1 for item in promoted if item.get("category") == "server"),
            "udr_packages": sum(1 for item in promoted if item.get("category") == "udr"),
            "issues": len(issues),
        },
        "components": promoted,
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--release-root", type=Path)
    parser.add_argument("--build-bin-root", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    release_root = args.release_root
    if release_root is None:
        release_root = latest_release_dir(repo_root)
    elif not release_root.is_absolute():
        release_root = repo_root / release_root
    build_bin_root = args.build_bin_root or default_build_bin_root(repo_root)
    if not build_bin_root.is_absolute():
        build_bin_root = repo_root / build_bin_root
    output = args.output or repo_root / "build" / "reports" / REPORT_NAME
    try:
        report = build_report(repo_root, args.matrix.expanduser().resolve(), release_root.resolve(), build_bin_root.resolve(), args.verify_only)
    except (OSError, ValueError) as exc:
        print(f"failed: {exc}", file=sys.stderr)
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_packaging_promotion={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
