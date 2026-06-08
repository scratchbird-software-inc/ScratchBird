#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path


PUBLIC_TOP_LEVEL = {
    "project",
    "docs",
    "LICENSE",
    "NOTICE",
    "LICENSES",
    "data",
    "release",
}

REQUIRED_TOP_LEVEL = {
    "project",
    "docs",
    "LICENSE",
    "NOTICE",
    "LICENSES",
    "data",
    "release",
}

DOCS_CHILDREN = {"build_requirements", "legal", "contracts"}
DOCS_CONTRACT_FILES = {
    Path("implementation_inputs")
    / "sbsql-canonicalization"
    / "SBSQL_SURFACE_REGISTRY.csv",
    Path("implementation_inputs")
    / "sbsql-canonicalization"
    / "SBSQL_SURFACE_STATUS_MATRIX.csv",
}

PRIVATE_LEGAL_WORKING_FILES = {
    "ScratchBird_legacy_poc_vs_private_cluster_boundary_audit.md",
}

SKIP_DIRS = {
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
    "CMakeFiles",
    "Testing",
    "build",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
    ".dart_tool",
    "target",
}

SKIP_SUFFIXES = {
    ".a",
    ".dll",
    ".dylib",
    ".exe",
    ".jar",
    ".o",
    ".obj",
    ".pdf",
    ".png",
    ".pyc",
    ".so",
    ".sbdb",
    ".zip",
}

ALLOWED_ENGINE_BINARY_NAMES = {
    "scratchbird-engine",
    "scratchbird-engine.exe",
    "scratchbird_engine",
    "scratchbird_engine.exe",
    "libscratchbird_engine.so",
    "scratchbird_engine.dll",
    "libscratchbird_engine.a",
}

GIT_REFERENCE_ALLOWLIST = {
    Path("docs/build_requirements/README.md"),
    Path("project/drivers/driver/cpp/include/nlohmann/json.hpp"),
    Path("project/drivers/tool/cli/include/nlohmann/json.hpp"),
    Path("project/drivers/driver/php/composer.lock"),
    Path("project/drivers/driver/mojo/README.md"),
    Path("project/drivers/driver/mojo/BASELINE_REQUIREMENT_MAPPING.md"),
    Path("project/drivers/driver/swift/Package.swift"),
    Path("project/drivers/driver/swift/Package.resolved"),
    Path("project/drivers/adaptor/scratchbird-metabase-driver/deps.edn"),
    Path(
        "project/tests/firebird_parser_worker/fixtures/"
        "full_firebirdsql_parser_udr_emulation_closure/artifacts/"
        "FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv"
    ),
    Path("project/resources/seed-packs/initial-resource-pack/resources/timezones/CONTRIBUTING"),
    Path("project/resources/seed-packs/initial-resource-pack/resources/timezones/NEWS"),
    Path("project/resources/seed-packs/initial-resource-pack/resources/timezones/theory.html"),
    Path("project/resources/seed-packs/initial-resource-pack/resources/timezones/tz-link.html"),
}

RELEASE_METADATA_DIR = Path("release") / "metadata"
PACKAGE_FILE_LIST_REL = RELEASE_METADATA_DIR / "public-export-file-list.txt"
CLEANUP_MANIFEST_REL = RELEASE_METADATA_DIR / "public-export-cleanup-manifest.json"


def io_path(path: Path) -> str:
    text = str(path.resolve())
    if os.name != "nt":
        return text
    if text.startswith("\\\\?\\"):
        return text
    if text.startswith("\\\\"):
        return "\\\\?\\UNC\\" + text.lstrip("\\")
    return "\\\\?\\" + text


def normal_path(path: Path) -> Path:
    text = str(path)
    if os.name == "nt":
        if text.startswith("\\\\?\\UNC\\"):
            return Path("\\\\" + text.removeprefix("\\\\?\\UNC\\"))
        if text.startswith("\\\\?\\"):
            return Path(text.removeprefix("\\\\?\\"))
    return path


def mkdir_public(path: Path) -> None:
    os.makedirs(io_path(path), exist_ok=True)


def copy2_public(src: Path, dst: Path) -> None:
    mkdir_public(dst.parent)
    shutil.copy2(io_path(src), io_path(dst))


def copytree_public(src: Path, dst: Path) -> None:
    shutil.copytree(io_path(src), io_path(dst), ignore=ignore_public_copy)


def rmtree_public(path: Path) -> None:
    if path.exists():
        shutil.rmtree(io_path(path))


def dot_git() -> str:
    return "." + "git"


def private_doc_path(name: str) -> str:
    return "docs" + "/" + name


def banned_needles() -> list[tuple[str, str]]:
    return [
        ("private_execution_plan_reference", private_doc_path("execution-plans")),
        ("private_completed_execution_plan_reference", private_doc_path("completed-execution-plans")),
        ("private_findings_reference", private_doc_path("findings")),
        ("git_metadata_reference", dot_git()),
        ("local_home_path_reference", "/" + "home" + "/" + "dcalford"),
        ("private_repo_reference", "ScratchBird" + "-Private"),
        ("legacy_repo_runtime_reference", "local workspace" + "/" + "ScratchBird"),
    ]


def copy_public_tree(repo_root: Path, stage_root: Path) -> None:
    rmtree_public(stage_root)
    mkdir_public(stage_root)

    for entry in sorted(PUBLIC_TOP_LEVEL):
        src = repo_root / entry
        dst = stage_root / entry
        if not src.exists():
            continue
        if entry == "docs":
            mkdir_public(dst)
            for child in sorted(DOCS_CHILDREN):
                if child == "contracts":
                    for spec_file in sorted(DOCS_CONTRACT_FILES):
                        child_src = src / child / spec_file
                        child_dst = dst / child / spec_file
                        if child_src.exists():
                            copy2_public(child_src, child_dst)
                    continue
                child_src = src / child
                child_dst = dst / child
                if child_src.exists():
                    copytree_public(child_src, child_dst)
            continue
        if entry == "release":
            mkdir_public(dst)
            readme = src / "README.md"
            if readme.exists():
                copy2_public(readme, dst / "README.md")
            for platform in ("linux", "windows", "freebsd"):
                layout_src = src / platform / "ENGINE_BINARY_LAYOUT.json"
                if layout_src.exists():
                    copy2_public(layout_src, dst / platform / "ENGINE_BINARY_LAYOUT.json")
            metadata_src = src / "metadata"
            if metadata_src.exists():
                copytree_public(metadata_src, dst / "metadata")
            continue
        if src.is_dir():
            copytree_public(src, dst)
        else:
            copy2_public(src, dst)


def ignore_public_copy(dirpath: str, names: list[str]) -> set[str]:
    ignored: set[str] = set()
    for name in names:
        if Path(dirpath).name == "legal" and name in PRIVATE_LEGAL_WORKING_FILES:
            ignored.add(name)
            continue
        if name == dot_git() or name.startswith(dot_git()) or name in SKIP_DIRS:
            ignored.add(name)
            continue
        if name.endswith((".tmp", ".log", ".pyc")):
            ignored.add(name)
    return ignored


def iter_files(root: Path):
    walk_root = io_path(root) if os.name == "nt" else str(root)
    for dirpath, dirnames, filenames in os.walk(walk_root):
        dirnames[:] = [
            name
            for name in dirnames
            if name not in SKIP_DIRS and name != dot_git()
        ]
        for filename in filenames:
            yield normal_path(Path(dirpath) / filename)


def relative_files(root: Path) -> list[str]:
    return sorted(str(path.relative_to(root)).replace(os.sep, "/") for path in iter_files(root))


def iter_text_files(root: Path):
    for path in iter_files(root):
        if path.suffix in SKIP_SUFFIXES:
            continue
        yield path


def private_reference_scan_includes(relative_path: Path) -> bool:
    parts = relative_path.parts
    if not parts:
        return False
    if parts[0] in {"LICENSE", "NOTICE", "LICENSES", "docs", "data", "release"}:
        return True
    if parts[0] != "project":
        return False
    if len(parts) < 2:
        return False
    project_child = parts[1]
    if project_child in {
        "CMakeLists.txt",
        "cmake",
        "include",
        "docs",
        "libraries",
        "resources",
        "src",
        "drivers",
    }:
        return True
    if project_child == "tools":
        return len(parts) >= 3 and parts[2] in {"release", "release_provenance"}
    if project_child == "tests":
        return len(parts) >= 3 and parts[2] == "release"
    return False


def fail(message: str) -> None:
    raise RuntimeError(message)


def check_package_shape(stage_root: Path) -> None:
    present = {path.name for path in stage_root.iterdir()}
    missing = sorted(REQUIRED_TOP_LEVEL - present)
    if missing:
        fail("public export is missing required top-level entries: " + ", ".join(missing))

    extra = sorted(present - PUBLIC_TOP_LEVEL)
    if extra:
        fail("public export contains disallowed top-level entries: " + ", ".join(extra))

    docs_root = stage_root / "docs"
    docs_children = {path.name for path in docs_root.iterdir() if path.is_dir()}
    extra_docs = sorted(docs_children - DOCS_CHILDREN)
    if extra_docs:
        fail("public export contains disallowed docs children: " + ", ".join(extra_docs))
    missing_docs = sorted(DOCS_CHILDREN - docs_children)
    if missing_docs:
        fail("public export is missing required docs children: " + ", ".join(missing_docs))

    if (stage_root / dot_git()).exists():
        fail("public export contains repository metadata")

    for path in iter_files(stage_root):
        rel = path.relative_to(stage_root)
        if any(part == dot_git() or part.startswith(dot_git()) for part in rel.parts):
            fail("public export contains repository metadata path: " + str(rel))

    forbidden_dirs = [
        ("docs", "execution-plans"),
        ("docs", "completed-execution-plans"),
        ("docs", "findings"),
    ]
    for parts in forbidden_dirs:
        if (stage_root / Path(*parts)).exists():
            fail("public export contains forbidden private documentation directory: " + "/".join(parts))

    release_root = stage_root / "release"
    for platform in ("linux", "windows", "freebsd"):
        layout = release_root / platform / "ENGINE_BINARY_LAYOUT.json"
        if not layout.exists():
            fail(f"public export missing {platform} engine binary layout")
    if (release_root / "macos").exists() or (release_root / "darwin").exists():
        fail("public export must not contain a first-release macOS binary layout")


def scan_private_references(stage_root: Path) -> None:
    findings: list[str] = []
    for path in iter_text_files(stage_root):
        try:
            with open(io_path(path), "r", encoding="utf-8") as handle:
                text = handle.read()
        except UnicodeDecodeError:
            continue
        except FileNotFoundError:
            continue
        rel = path.relative_to(stage_root)
        if rel.name == dot_git() + "ignore":
            continue
        if not private_reference_scan_includes(rel):
            continue
        for label, needle in banned_needles():
            if label == "git_metadata_reference" and rel in GIT_REFERENCE_ALLOWLIST:
                continue
            if needle in text:
                findings.append(f"{rel}: {label}: {needle}")
    if findings:
        print("public export private-reference scan failed")
        for finding in findings[:200]:
            print(finding)
        if len(findings) > 200:
            print(f"... {len(findings) - 200} additional findings omitted")
        fail("public export contains forbidden private references")


def check_release_binaries(stage_root: Path) -> None:
    release_root = stage_root / "release"
    executable_suffixes = {".exe", ".dll", ".so", ".a"}
    findings: list[str] = []
    for path in iter_files(release_root):
        executable_candidate = path.suffix in executable_suffixes
        if os.name == "nt":
            executable_candidate = executable_candidate or path.name in ALLOWED_ENGINE_BINARY_NAMES
        else:
            executable_candidate = executable_candidate or os.access(path, os.X_OK)
        if not executable_candidate:
            continue
        if path.name not in ALLOWED_ENGINE_BINARY_NAMES and path.suffix != ".json":
            findings.append(str(path.relative_to(stage_root)))
    if findings:
        fail("public release layout contains non-engine binary payloads: " + ", ".join(findings[:20]))


def run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def configure_staged_project(args, stage_root: Path, build_root: Path) -> None:
    rmtree_public(build_root)
    build_root.mkdir(parents=True)

    command = [
        args.cmake,
        "-S",
        str(stage_root / "project"),
        "-B",
        str(build_root),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON",
        "-DSB_PUBLIC_RELEASE_EXPORT_NESTED=ON",
        "-DSB_BUILD_DATABASE_LIFECYCLE_TESTS=OFF",
        "-DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF",
        "-DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF",
        "-DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF",
        "-DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF",
    ]
    if args.c_compiler:
        command.append(f"-DCMAKE_C_COMPILER={args.c_compiler}")
    if args.cxx_compiler:
        command.append(f"-DCMAKE_CXX_COMPILER={args.cxx_compiler}")
    for name in (
        "SB_LLVM_PROJECT_ROOT",
        "SB_LLVM_TOOLS_ROOT",
        "SB_LLVM_LIBRARY",
        "SB_LLVM_LINK_MODE",
    ):
        value = os.environ.get(name, "")
        if value:
            command.append(f"-D{name}={value}")

    env = os.environ.copy()
    env.setdefault("TMPDIR", str(build_root.parent / "tmp"))
    Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    run(command, stage_root, env)
    run(
        [
            args.cmake,
            "--build",
            str(build_root),
            "--target",
            "public_release_artifact_trust_build_prereqs",
            "--parallel",
            str(args.parallel),
        ],
        stage_root,
        env,
    )
    run(
        [
            args.ctest,
            "--test-dir",
            str(build_root),
            "-L",
            "ELER-108",
            "-E",
            "public_project_export_gate",
            "--output-on-failure",
        ],
        stage_root,
        env,
    )


def find_seeder(build_root: Path) -> Path:
    names = {"public_example_database_seed", "public_example_database_seed.exe"}
    for path in build_root.rglob("*"):
        if path.name in names and path.is_file():
            return path
    fail("nested public build did not produce public_example_database_seed")
    raise AssertionError("unreachable")


def generate_example_database(args, stage_root: Path, build_root: Path) -> None:
    seeder = find_seeder(build_root)
    example_root = stage_root / "data" / "example"
    example_root.mkdir(parents=True, exist_ok=True)
    database_path = example_root / "scratchbird-example.sbdb"
    manifest_path = example_root / "scratchbird-example.manifest.json"
    resource_root = stage_root / "project" / "resources" / "seed-packs" / "initial-resource-pack"
    env = os.environ.copy()
    env.setdefault("TMPDIR", str(build_root.parent / "tmp"))
    Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    run(
        [
            str(seeder),
            "--output",
            str(database_path),
            "--manifest",
            str(manifest_path),
            "--resource-seed-pack-root",
            str(resource_root),
            "--overwrite",
        ],
        stage_root,
        env,
    )
    if not database_path.exists() or database_path.stat().st_size == 0:
        fail("example database was not generated")
    if not manifest_path.exists() or manifest_path.stat().st_size == 0:
        fail("example database manifest was not generated")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write_text_file(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def build_cleanup_manifest(stage_root: Path, payload_files: list[str]) -> dict[str, object]:
    example_database = stage_root / "data" / "example" / "scratchbird-example.sbdb"
    example_manifest = stage_root / "data" / "example" / "scratchbird-example.manifest.json"
    package_file_list_text = "\n".join(payload_files) + "\n"
    metadata_outputs = [
        CLEANUP_MANIFEST_REL.as_posix(),
        PACKAGE_FILE_LIST_REL.as_posix(),
    ]

    if not example_database.exists() or not example_manifest.exists():
        fail("cleanup manifest requires generated example database artifacts")

    # PUBLIC_RELEASE_CLEANUP_MANIFEST
    return {
        "status": "pass",
        "profile": "public_core_engine_source",
        "package_roots": sorted(PUBLIC_TOP_LEVEL),
        "docs_children": sorted(DOCS_CHILDREN),
        "payload_file_count": len(payload_files),
        "metadata_outputs": metadata_outputs,
        "package_file_count_after_metadata": len(payload_files) + len(metadata_outputs),
        "payload_file_list_sha256": sha256_text(package_file_list_text),
        "staged_text_scan": "passed",
        "nested_public_build_target": "public_release_artifact_trust_build_prereqs",
        "nested_public_ctest": "passed",
        "nested_public_ctest_label": "ELER-108",
        "nested_public_ctest_scope": "release_artifact_trust",
        "release_binary_policy": "engine_only",
        "supported_platform_layouts": ["linux", "windows", "freebsd"],
        "unsupported_platform_layouts_absent": ["macos", "darwin"],
        "example_database": {
            "path": "data/example/scratchbird-example.sbdb",
            "bytes": example_database.stat().st_size,
            "sha256": sha256_file(example_database),
            "manifest_path": "data/example/scratchbird-example.manifest.json",
            "manifest_sha256": sha256_file(example_manifest),
        },
        "cleanup_exclusions": [
            "repository_metadata",
            "private_coordination_docs",
            "private_evidence_docs",
            "private_audit_outputs",
            "build_outputs",
            "cache_outputs",
            "temporary_outputs",
            "stale_generated_outputs",
            "absolute_developer_paths",
            "legacy_checkout_paths",
            "unsupported_platform_layouts",
            "non_engine_binary_payloads",
        ],
        "authority_note": (
            "Release metadata is proof evidence only; engine durable UUID, "
            "SBLR/internal-envelope, and MGA transaction authority are unchanged."
        ),
        "cluster_boundary": "local cluster production execution remains external-provider-only",
    }


def write_cleanup_outputs(
    stage_root: Path,
    external_file_list_path: Path,
    external_cleanup_manifest_path: Path,
) -> None:
    payload_files = relative_files(stage_root)
    package_file_list_text = "\n".join(payload_files) + "\n"
    cleanup_manifest = build_cleanup_manifest(stage_root, payload_files)
    cleanup_manifest_text = json.dumps(cleanup_manifest, indent=2, sort_keys=True) + "\n"

    write_text_file(stage_root / PACKAGE_FILE_LIST_REL, package_file_list_text)
    write_text_file(stage_root / CLEANUP_MANIFEST_REL, cleanup_manifest_text)
    write_text_file(external_cleanup_manifest_path, cleanup_manifest_text)
    write_text_file(external_file_list_path, "\n".join(relative_files(stage_root)) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--staging-root", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--c-compiler", default="")
    parser.add_argument("--cxx-compiler", default="")
    parser.add_argument("--parallel", type=int, default=1)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    stage_root = args.staging_root.resolve() / "public-export"
    build_root = args.staging_root.resolve() / "public-export-build"
    file_list_path = args.staging_root.resolve() / "public-export-file-list.txt"
    cleanup_manifest_path = args.staging_root.resolve() / "public-export-cleanup-manifest.json"

    try:
        copy_public_tree(repo_root, stage_root)
        check_package_shape(stage_root)
        scan_private_references(stage_root)
        configure_staged_project(args, stage_root, build_root)
        generate_example_database(args, stage_root, build_root)
        check_package_shape(stage_root)
        check_release_binaries(stage_root)
        write_cleanup_outputs(stage_root, file_list_path, cleanup_manifest_path)
        check_package_shape(stage_root)
        check_release_binaries(stage_root)
        scan_private_references(stage_root)
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"public_project_export_gate=failed reason={exc}")
        return 1

    print(f"public_project_export_gate=passed staged_root={stage_root}")
    print(f"public_project_export_manifest={file_list_path}")
    print(f"public_project_export_cleanup_manifest={cleanup_manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
