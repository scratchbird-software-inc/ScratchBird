#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Driver/adaptor/tool execution_plan gates.

The gates are deliberately source-tree focused for the first integration pass:
they prove inventory, path hygiene, artifact isolation, CTest matrix coverage,
and current-repo benchmark-driver routing before native language wrappers are
allowed to run.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DRIVERS = [
    "adbc",
    "cpp",
    "dart",
    "dotnet",
    "elixir",
    "flightsql",
    "go",
    "jdbc",
    "julia",
    "mojo",
    "node",
    "odbc",
    "pascal",
    "perl",
    "php",
    "python",
    "r",
    "r2dbc",
    "ruby",
    "rust",
    "swift",
]

ADAPTORS = [
    "scratchbird-airbyte",
    "scratchbird-dbeaver-driver",
    "scratchbird-dbt-adapter",
    "scratchbird-hibernate-dialect",
    "scratchbird-looker",
    "scratchbird-metabase-driver",
    "scratchbird-powerbi",
    "scratchbird-prisma-adapter",
    "scratchbird-sqlalchemy-dialect",
    "scratchbird-superset-driver",
    "scratchbird-tableau",
    "scratchbird-typeorm-adapter",
]

TOOLS = ["cli"]

ALLOWED_DRIVER_STATUSES = {"beta_2", "planned_not_implemented"}

DRIVER_FAMILIES = {
    "native_cli",
    "c_cpp",
    "odbc",
    "jdbc",
    "dotnet",
    "go",
    "node",
    "python",
    "rust",
    "ruby",
    "swift",
    "dart",
    "elixir",
    "php",
    "pascal",
    "perl",
    "julia",
    "mojo",
    "adbc",
    "flight_sql",
    "r2dbc",
    "r",
    "dbeaver",
    "hibernate",
    "metabase",
    "prisma",
    "sqlalchemy",
    "superset",
    "typeorm",
    "airbyte",
    "dbt",
    "looker",
    "powerbi",
    "tableau",
}

INGRESS_MODES = {"direct_listener", "manager_proxy", "local_ipc", "embedded_standalone", "driver_embedded_jdbc", "driver_embedded_node", "driver_embedded_python"}

THREAD_SAFETY_CLASSES = {
    "single_threaded",
    "connection_thread_confined",
    "statement_thread_confined",
    "thread_safe_with_external_lock",
    "thread_safe",
}

COMPONENT_TOOLCHAINS = {
    "driver:adbc": ["python3"],
    "driver:cpp": ["cmake", "c++"],
    "driver:dart": ["dart"],
    "driver:dotnet": ["dotnet"],
    "driver:elixir": ["mix", "elixir"],
    "driver:flightsql": ["python3"],
    "driver:go": ["go"],
    "driver:jdbc": ["java", "javac"],
    "driver:julia": ["python3"],
    "driver:mojo": ["mojo"],
    "driver:node": ["node", "npm"],
    "driver:odbc": ["cmake", "c++"],
    "driver:pascal": ["fpc"],
    "driver:perl": ["python3"],
    "driver:php": ["php", "composer"],
    "driver:python": ["python3"],
    "driver:r": ["R"],
    "driver:r2dbc": ["python3"],
    "driver:ruby": ["ruby", "gem"],
    "driver:rust": ["cargo"],
    "driver:swift": ["swift"],
    "adaptor:scratchbird-airbyte": ["python3"],
    "adaptor:scratchbird-dbeaver-driver": ["mvn", "java", "zip"],
    "adaptor:scratchbird-dbt-adapter": ["python3"],
    "adaptor:scratchbird-hibernate-dialect": ["mvn", "java"],
    "adaptor:scratchbird-looker": ["python3"],
    "adaptor:scratchbird-metabase-driver": ["java"],
    "adaptor:scratchbird-powerbi": ["python3"],
    "adaptor:scratchbird-prisma-adapter": ["node", "npm"],
    "adaptor:scratchbird-sqlalchemy-dialect": ["python3"],
    "adaptor:scratchbird-superset-driver": ["python3"],
    "adaptor:scratchbird-tableau": ["python3"],
    "adaptor:scratchbird-typeorm-adapter": ["node", "npm"],
    "tool:cli": ["cmake", "c++"],
}

FORBIDDEN_PATH_PATTERNS = [
    re.compile(r"/" + r"home/dcalford" + r"/local workspace" + r"/ScratchBird-driver"),
    re.compile(r"tracks/p3"),
    re.compile(r"tracks/alpha"),
    re.compile(r"build_cli/tracks"),
    re.compile(r"build/tracks"),
    re.compile(r"\.\./ScratchBird(?:/|$)"),
    re.compile(r"ScratchBird/build"),
]

GENERATED_DIR_NAMES = {
    ".build",
    ".dart_tool",
    ".elixir_ls",
    ".eggs",
    ".gradle",
    ".pytest_cache",
    "_build",
    "__pycache__",
    "bin",
    "build",
    "deps",
    "dist",
    "node_modules",
    "obj",
    "target",
    "vendor",
    "Testing",
}

GENERATED_SUFFIXES = {
    ".a",
    ".dll",
    ".dylib",
    ".exe",
    ".gem",
    ".o",
    ".pyc",
    ".ppu",
    ".rlib",
    ".so",
}


@dataclass(frozen=True)
class Context:
    repo_root: Path
    project_root: Path
    build_root: Path
    execution_plan_root: Path
    allow_toolchain_waivers: bool
    require_all_toolchains: bool

    @property
    def drivers_root(self) -> Path:
        return self.project_root / "drivers"


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def iter_files(root: Path) -> Iterable[Path]:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name not in GENERATED_DIR_NAMES]
        for filename in filenames:
            yield Path(dirpath) / filename


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def skip(message: str) -> int:
    print(f"skipped: {message}")
    return 77


def component_path(ctx: Context, component: str) -> Path:
    category, name = component.split(":", 1)
    return ctx.drivers_root / category / name


def git_tracked_paths(ctx: Context, root: Path) -> set[str]:
    try:
        rel_root = str(root.relative_to(ctx.repo_root))
    except ValueError:
        return set()
    result = subprocess.run(
        ["git", "-C", str(ctx.repo_root), "ls-files", "--", rel_root],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return set()
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}


def is_tracked_source_path(path: Path, ctx: Context, tracked_paths: set[str]) -> bool:
    try:
        rel = str(path.relative_to(ctx.repo_root))
    except ValueError:
        return False
    if rel in tracked_paths:
        return True
    if path.is_dir():
        prefix = rel.rstrip("/") + "/"
        return any(tracked.startswith(prefix) for tracked in tracked_paths)
    return False


def expected_components() -> list[str]:
    return [f"driver:{name}" for name in DRIVERS] + [
        f"adaptor:{name}" for name in ADAPTORS
    ] + [f"tool:{name}" for name in TOOLS]


def check_inventory(ctx: Context) -> int:
    inventory = ctx.execution_plan_root / "artifacts" / "DRIVER_SOURCE_INVENTORY.csv"
    rows = read_csv(inventory)
    by_component = {f"{row['category']}:{row['name']}": row for row in rows}
    missing = [component for component in expected_components() if component not in by_component]
    if missing:
        return fail(f"inventory missing components: {', '.join(missing)}")
    for component in expected_components():
        path = component_path(ctx, component)
        if not path.is_dir():
            return fail(f"component path missing: {component} -> {path}")
        row = by_component[component]
        if not (ctx.project_root / row["relative_path"]).exists():
            return fail(f"inventory relative_path does not exist for {component}: {row['relative_path']}")
    print(f"inventory ok: {len(rows)} rows")
    return 0


def check_old_paths(ctx: Context) -> int:
    roots = [
        ctx.drivers_root,
        ctx.repo_root / "docs/reference/legacy_execution_plan_10_performance_parity/benchmark_harness/scripts",
        ctx.repo_root / "docs/reference/legacy_execution_plan_10_performance_parity/benchmark_harness/stress-tests/runners",
    ]
    hits: list[str] = []
    fixture_root = ctx.execution_plan_root.resolve()
    for root in roots:
        if not root.exists():
            continue
        for path in iter_files(root):
            resolved_path = path.resolve()
            if resolved_path == Path(__file__).resolve():
                continue
            if resolved_path == fixture_root or fixture_root in resolved_path.parents:
                continue
            try:
                text = path.read_text(errors="ignore")
            except OSError:
                continue
            for pattern in FORBIDDEN_PATH_PATTERNS:
                if pattern.search(text):
                    rel = path.relative_to(ctx.repo_root)
                    hits.append(f"{rel}: {pattern.pattern}")
                    break
    if hits:
        return fail("old path hits:\n" + "\n".join(hits[:50]))
    print("old path gate ok")
    return 0


def check_artifact_isolation(ctx: Context) -> int:
    offenders: list[str] = []
    tracked_paths = git_tracked_paths(ctx, ctx.drivers_root)
    for path in ctx.drivers_root.rglob("*"):
        rel = path.relative_to(ctx.repo_root)
        if is_tracked_source_path(path, ctx, tracked_paths):
            continue
        if path.is_dir() and (path.name in GENERATED_DIR_NAMES or path.name.endswith(".egg-info")):
            offenders.append(str(rel))
        elif path.is_file() and path.suffix in GENERATED_SUFFIXES:
            offenders.append(str(rel))
    if offenders:
        return fail("generated artifacts under project/drivers:\n" + "\n".join(offenders[:80]))
    print("artifact isolation ok")
    return 0


def check_source_tree_write(ctx: Context) -> int:
    # The first-pass write guard is intentionally identical to artifact
    # isolation. Native wrappers will extend this with before/after snapshots.
    return check_artifact_isolation(ctx)


def check_toolchains(ctx: Context) -> int:
    missing: list[str] = []
    present = 0
    for component, commands in sorted(COMPONENT_TOOLCHAINS.items()):
        component_missing = [command for command in commands if shutil.which(command) is None]
        if component_missing:
            missing.append(f"{component}: {', '.join(component_missing)}")
        else:
            present += 1
    if missing and ctx.require_all_toolchains:
        return fail("missing required toolchains:\n" + "\n".join(missing))
    if missing and ctx.allow_toolchain_waivers:
        print(f"toolchains present for {present} components")
        print("waived missing toolchains:")
        print("\n".join(missing))
        return 0
    if missing:
        return skip("missing non-required toolchains:\n" + "\n".join(missing))
    print(f"all component toolchains present: {present}")
    return 0


def check_artifact_file(ctx: Context, filename: str) -> int:
    path = ctx.execution_plan_root / "artifacts" / filename
    if not path.is_file():
        return fail(f"required artifact missing: {path}")
    print(f"artifact present: {path.relative_to(ctx.repo_root)}")
    return 0


def check_dependency_policy(ctx: Context) -> int:
    return check_artifact_file(ctx, "TOOLCHAIN_AND_DEPENDENCY_POLICY.md")


def check_conformance_matrix(ctx: Context) -> int:
    rows = read_csv(ctx.execution_plan_root / "artifacts" / "DRIVER_COMMON_CONFORMANCE_MATRIX.csv")
    drivers = {row["driver"] for row in rows}
    missing = [driver for driver in DRIVERS if driver not in drivers]
    if missing:
        return fail(f"conformance matrix missing drivers: {', '.join(missing)}")
    print(f"conformance matrix ok: {len(rows)} rows")
    return 0


def check_package_manifest(ctx: Context) -> int:
    manifest = ctx.drivers_root / "DriverPackageManifest.csv"
    rows = read_csv(manifest)
    by_component = {row["component_id"]: row for row in rows}
    missing = [component for component in expected_components() if component not in by_component]
    if missing:
        return fail(f"DriverPackageManifest missing components: {', '.join(missing)}")
    required_fields = [
        "driver_package_uuid",
        "driver_family",
        "driver_status",
        "api_surface_set",
        "ingress_mode_set",
        "wire_protocol_set",
        "dsn_key_set",
        "auth_method_set",
        "tls_profile_set",
        "type_mapping_profile",
        "diagnostic_mapping_profile",
        "metadata_profile",
        "thread_safety_class",
        "pooling_capability",
        "conformance_profile_ref",
        "source_path",
    ]
    seen_uuids: set[str] = set()
    for component in expected_components():
        row = by_component[component]
        for field in required_fields:
            if not row.get(field, "").strip():
                return fail(f"DriverPackageManifest {component} missing {field}")
        uuid = row["driver_package_uuid"]
        if uuid in seen_uuids:
            return fail(f"DriverPackageManifest duplicate driver_package_uuid: {uuid}")
        seen_uuids.add(uuid)
        if row["driver_family"] not in DRIVER_FAMILIES:
            return fail(f"DriverPackageManifest {component} unknown driver_family: {row['driver_family']}")
        if row["driver_status"] not in ALLOWED_DRIVER_STATUSES:
            return fail(
                f"DriverPackageManifest {component} status is not one of "
                f"{', '.join(sorted(ALLOWED_DRIVER_STATUSES))}"
            )
        if row["driver_status"] == "planned_not_implemented" and row.get("release_bucket") != "tracked_not_released":
            return fail(
                f"DriverPackageManifest {component} planned_not_implemented "
                "status must use tracked_not_released release_bucket"
            )
        ingress_modes = {value for value in row["ingress_mode_set"].split(";") if value}
        unknown_ingress = sorted(ingress_modes - INGRESS_MODES)
        if unknown_ingress:
            return fail(
                f"DriverPackageManifest {component} unknown ingress modes: {', '.join(unknown_ingress)}"
            )
        if row["thread_safety_class"] not in THREAD_SAFETY_CLASSES:
            return fail(
                f"DriverPackageManifest {component} unknown thread_safety_class: {row['thread_safety_class']}"
            )
        if "sbwp_v1_1" not in row["wire_protocol_set"]:
            return fail(f"DriverPackageManifest {component} missing sbwp_v1_1 wire protocol")
        if "engine_local_password" not in row["auth_method_set"]:
            return fail(f"DriverPackageManifest {component} missing engine-owned local password auth")
        if "scratchbird_tls_1_3_floor" not in row["tls_profile_set"]:
            return fail(f"DriverPackageManifest {component} missing ScratchBird TLS profile")
        source = ctx.repo_root / row["source_path"]
        if not source.is_dir():
            return fail(f"DriverPackageManifest source_path does not exist for {component}: {row['source_path']}")
    print(f"DriverPackageManifest ok: {len(rows)} rows")
    return 0


def check_server_fixture(ctx: Context) -> int:
    return check_artifact_file(ctx, "SERVER_FIXTURE_LIFECYCLE_POLICY.md")


def check_packaging_policy(ctx: Context) -> int:
    return check_artifact_file(ctx, "PACKAGING_INSTALL_GATE_POLICY.md")


def check_benchmark_policy(ctx: Context) -> int:
    return check_artifact_file(ctx, "BENCHMARK_DRIVER_READINESS_POLICY.md")


def check_execution_plan10(ctx: Context) -> int:
    runner = ctx.repo_root / "docs/reference/legacy_execution_plan_10_performance_parity/benchmark_harness/scripts/run-benchmark.sh"
    stress = ctx.repo_root / "docs/reference/legacy_execution_plan_10_performance_parity/benchmark_harness/stress-tests/runners/dialect_stress_runner.py"
    for path in (runner, stress):
        text = path.read_text(errors="ignore")
        has_current_python_driver_path = (
            "project/drivers/driver/python/src" in text
            or '"project" / "drivers" / "driver" / "python" / "src"' in text
        )
        if not has_current_python_driver_path:
            return fail(f"current Python driver path missing from {path.relative_to(ctx.repo_root)}")
    return check_old_paths(ctx)


def check_release_claims(ctx: Context) -> int:
    return check_artifact_file(ctx, "RELEASE_CLAIM_POLICY.md")


def check_component(ctx: Context, component: str) -> int:
    path = component_path(ctx, component)
    if not path.is_dir():
        return fail(f"component directory missing: {component}")
    for command in COMPONENT_TOOLCHAINS.get(component, []):
        if shutil.which(command) is None:
            if ctx.require_all_toolchains:
                return fail(f"{component} missing required toolchain: {command}")
            if ctx.allow_toolchain_waivers:
                print(f"{component} toolchain waived: missing {command}")
                return 0
            return skip(f"{component} missing toolchain: {command}")
    print(f"{component} source and toolchain gate ok")
    return 0


def check_ctest_matrix(ctx: Context) -> int:
    rows = read_csv(ctx.execution_plan_root / "artifacts" / "CTEST_DRIVER_GATE_MATRIX.csv")
    labels = {row["ctest_label"] for row in rows}
    required = {
        "driver_source_inventory_gate",
        "driver_old_path_gate",
        "driver_build_artifact_isolation_gate",
        "driver_source_tree_write_guard",
        "driver_toolchain_detection_gate",
        "driver_dependency_policy_gate",
        "driver_common_conformance_gate",
        "driver_package_manifest_gate",
        "driver_server_fixture_gate",
        "driver_packaging_install_gate",
        "driver_benchmark_readiness_gate",
        "driver_execution_plan10_runner_gate",
        "driver_static_hygiene_gate",
        "driver_release_claim_gate",
        "drivers_all",
        "drivers_final_zero_drift_audit",
    }
    required.update(f"driver_{name}_gate" for name in DRIVERS)
    required.update(
        {
            "adaptor_airbyte_gate",
            "adaptor_dbeaver_gate",
            "adaptor_dbt_gate",
            "adaptor_hibernate_gate",
            "adaptor_looker_gate",
            "adaptor_metabase_gate",
            "adaptor_powerbi_gate",
            "adaptor_prisma_gate",
            "adaptor_sqlalchemy_gate",
            "adaptor_superset_gate",
            "adaptor_tableau_gate",
            "adaptor_typeorm_gate",
            "tool_cli_gate",
        }
    )
    missing = sorted(required - labels)
    if missing:
        return fail("CTest matrix missing labels:\n" + "\n".join(missing))
    print(f"CTest matrix ok: {len(rows)} rows")
    return 0


def check_static_hygiene(ctx: Context) -> int:
    for checker in (
        check_old_paths,
        check_artifact_isolation,
        check_ctest_matrix,
        check_package_manifest,
        check_release_claims,
    ):
        result = checker(ctx)
        if result != 0:
            return result
    return 0


def check_final_zero_drift(ctx: Context) -> int:
    for checker in (
        check_inventory,
        check_static_hygiene,
        check_conformance_matrix,
        check_package_manifest,
        check_dependency_policy,
        check_server_fixture,
        check_benchmark_policy,
        check_packaging_policy,
    ):
        result = checker(ctx)
        if result != 0:
            return result
    print("final zero-drift audit ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    parser.add_argument("--component", default="")
    parser.add_argument(
        "--require-all-toolchains",
        action="store_true",
        default=os.environ.get("SB_DRIVER_REQUIRE_ALL_TOOLCHAINS") == "1",
    )
    parser.add_argument(
        "--no-toolchain-waivers",
        action="store_true",
        default=os.environ.get("SB_DRIVER_ALLOW_TOOLCHAIN_WAIVERS") == "0",
    )
    parser.add_argument(
        "mode",
        choices=[
            "inventory",
            "old-paths",
            "artifact-isolation",
            "source-tree-write",
            "toolchains",
            "dependency-policy",
            "conformance-matrix",
            "package-manifest",
            "server-fixture",
            "packaging-policy",
            "benchmark-policy",
            "execution_plan10",
            "static-hygiene",
            "release-claims",
            "component",
            "drivers-all",
            "final-zero-drift",
        ],
    )
    args = parser.parse_args()
    ctx = Context(
        repo_root=args.repo_root.resolve(),
        project_root=args.project_root.resolve(),
        build_root=args.build_root.absolute(),
        execution_plan_root=args.execution_plan_root.resolve(),
        allow_toolchain_waivers=not args.no_toolchain_waivers,
        require_all_toolchains=args.require_all_toolchains,
    )

    ctx.build_root.mkdir(parents=True, exist_ok=True)

    dispatch = {
        "inventory": check_inventory,
        "old-paths": check_old_paths,
        "artifact-isolation": check_artifact_isolation,
        "source-tree-write": check_source_tree_write,
        "toolchains": check_toolchains,
        "dependency-policy": check_dependency_policy,
        "conformance-matrix": check_conformance_matrix,
        "package-manifest": check_package_manifest,
        "server-fixture": check_server_fixture,
        "packaging-policy": check_packaging_policy,
        "benchmark-policy": check_benchmark_policy,
        "execution_plan10": check_execution_plan10,
        "static-hygiene": check_static_hygiene,
        "release-claims": check_release_claims,
        "drivers-all": check_static_hygiene,
        "final-zero-drift": check_final_zero_drift,
    }
    if args.mode == "component":
        if not args.component:
            return fail("--component is required for component mode")
        return check_component(ctx, args.component)
    return dispatch[args.mode](ctx)


if __name__ == "__main__":
    raise SystemExit(main())
