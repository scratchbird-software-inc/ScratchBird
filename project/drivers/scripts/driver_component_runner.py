#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run driver/adaptor/tool component build and test gates.

Every command either runs in an external build directory or in a staged copy
under build/drivers so package-manager output never lands in project/drivers.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


DRIVERS_ROOT_PARTS = ("project", "drivers")
SKIP_CODE = 77

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
    "driver:mojo": [],
    "driver:node": ["node", "npm"],
    "driver:odbc": ["cmake", "c++"],
    "driver:pascal": ["fpc"],
    "driver:perl": ["python3"],
    "driver:php": ["php", "composer"],
    "driver:python": ["python3"],
    "driver:r": ["Rscript"],
    "driver:r2dbc": ["python3"],
    "driver:ruby": ["ruby"],
    "driver:rust": ["cargo"],
    "driver:swift": ["swift"],
    "adaptor:scratchbird-airbyte": ["python3"],
    "adaptor:scratchbird-dbeaver-driver": ["mvn", "java", "zip"],
    "adaptor:scratchbird-dbt-adapter": ["python3"],
    "adaptor:scratchbird-hibernate-dialect": ["mvn", "java"],
    "adaptor:scratchbird-looker": ["python3"],
    "adaptor:scratchbird-metabase-driver": ["clojure", "java"],
    "adaptor:scratchbird-powerbi": ["python3"],
    "adaptor:scratchbird-prisma-adapter": ["node"],
    "adaptor:scratchbird-sqlalchemy-dialect": ["python3"],
    "adaptor:scratchbird-superset-driver": ["python3"],
    "adaptor:scratchbird-tableau": ["python3"],
    "adaptor:scratchbird-typeorm-adapter": ["node"],
    "tool:cli": ["cmake", "c++"],
}

CONTRACT_COMPONENTS = {
    "driver:adbc",
    "driver:flightsql",
    "driver:julia",
    "driver:perl",
    "driver:r2dbc",
    "adaptor:scratchbird-airbyte",
    "adaptor:scratchbird-dbt-adapter",
    "adaptor:scratchbird-looker",
    "adaptor:scratchbird-powerbi",
    "adaptor:scratchbird-tableau",
}

REQUIRED_ROUTE_REQUIREMENTS = {
    "sbwp_v1_1",
    "scratchbird_tls_1_3_floor",
    "engine_authentication_authority",
    "engine_authorization_authority",
    "mga_transaction_finality",
    "sys_information_metadata",
    "uuid_identity",
    "no_hidden_replay",
}

ALLOWED_CONTRACT_STATUSES = {"beta_2", "planned_not_implemented"}

REQUIRED_CONFORMANCE = {
    "connect_auth",
    "prepare_execute_fetch",
    "transactions",
    "metadata",
    "type_mapping",
    "error_mapping",
    "reconnect",
    "protocol_negotiation",
    "cancellation",
}

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
    ".beam",
    ".class",
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
    component: str
    allow_toolchain_waivers: bool
    require_all_toolchains: bool

    @property
    def category(self) -> str:
        return self.component.split(":", 1)[0]

    @property
    def name(self) -> str:
        return self.component.split(":", 1)[1]

    @property
    def source_dir(self) -> Path:
        return self.project_root / "drivers" / self.category / self.name

    @property
    def component_build_root(self) -> Path:
        return self.build_root / self.category / self.name

    @property
    def deps_root(self) -> Path:
        return self.build_root / "_deps"

    @property
    def logs_root(self) -> Path:
        return self.component_build_root / "logs"


def print_skip(message: str) -> int:
    print(f"skipped: {message}")
    return SKIP_CODE


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def display_path(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


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


def resolve_argv(argv: list[str], env: dict[str, str]) -> list[str]:
    if not argv:
        return argv
    executable = argv[0]
    if any(sep in executable for sep in ("/", "\\")) or Path(executable).is_absolute():
        return argv
    resolved = shutil.which(executable, path=env.get("PATH"))
    if resolved is None:
        return argv
    return [resolved, *argv[1:]]


def base_env(ctx: Context) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("LC_ALL", "C.UTF-8")
    env.setdefault("LANG", "C.UTF-8")
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PIP_CACHE_DIR"] = str(ctx.deps_root / "python" / "pip")
    env["PYTHONPYCACHEPREFIX"] = str(ctx.component_build_root / "pycache")
    env["npm_config_cache"] = str(ctx.deps_root / "npm")
    env["CARGO_HOME"] = str(ctx.deps_root / "cargo" / "home")
    env["GOCACHE"] = str(ctx.deps_root / "go" / "cache")
    env["GOMODCACHE"] = str(ctx.deps_root / "go" / "mod")
    env["GRADLE_USER_HOME"] = str(ctx.deps_root / "jvm" / "gradle-home")
    env["COMPOSER_CACHE_DIR"] = str(ctx.deps_root / "composer")
    env["PUB_CACHE"] = str(ctx.deps_root / "dart" / "pub-cache")
    env["MIX_HOME"] = str(ctx.deps_root / "elixir" / "mix-home")
    env["HEX_HOME"] = str(ctx.deps_root / "elixir" / "hex-home")
    env["R_LIBS_USER"] = str(ctx.deps_root / "r" / "library")
    java = shutil.which("java", path=env.get("PATH"))
    if java is not None:
        java_home = Path(java).resolve().parent.parent
        if (java_home / "bin" / ("java.exe" if os.name == "nt" else "java")).exists():
            env["JAVA_HOME"] = str(java_home)
    return env


def run_command(
    ctx: Context,
    name: str,
    argv: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
) -> int:
    ctx.logs_root.mkdir(parents=True, exist_ok=True)
    log_path = ctx.logs_root / f"{name}.log"
    merged_env = base_env(ctx)
    if env:
        merged_env.update(env)
    for key in (
        "PIP_CACHE_DIR",
        "PYTHONPYCACHEPREFIX",
        "npm_config_cache",
        "CARGO_HOME",
        "GOCACHE",
        "GOMODCACHE",
        "GRADLE_USER_HOME",
        "COMPOSER_CACHE_DIR",
        "PUB_CACHE",
        "MIX_HOME",
        "HEX_HOME",
        "R_LIBS_USER",
    ):
        Path(merged_env[key]).mkdir(parents=True, exist_ok=True)
    (ctx.component_build_root / "tmp").mkdir(parents=True, exist_ok=True)
    argv = resolve_argv(argv, merged_env)
    print(f"running {ctx.component} {name}: {' '.join(argv)}")
    with log_path.open("w", encoding="utf-8") as handle:
        handle.write(f"$ {' '.join(argv)}\n")
        handle.write(f"cwd={cwd}\n\n")
        try:
            proc = subprocess.run(
                argv,
                cwd=cwd,
                env=merged_env,
                stdout=handle,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
        except FileNotFoundError as exc:
            handle.write(f"command launch failed: {exc}\n")
            return fail(f"{ctx.component} {name} command not found: {argv[0]}; log={log_path}")
    if proc.returncode != 0:
        try:
            tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-80:])
        except OSError:
            tail = ""
        if proc.returncode == SKIP_CODE:
            print(tail)
            return SKIP_CODE
        print(tail, file=sys.stderr)
        return fail(f"{ctx.component} {name} failed with exit code {proc.returncode}; log={log_path}")
    print(f"{ctx.component} {name} ok; log={display_path(log_path, ctx.repo_root)}")
    return 0


def stage_source(ctx: Context) -> Path:
    stage = ctx.component_build_root / "stage"
    if stage.exists():
        shutil.rmtree(stage)

    def ignore(_: str, names: list[str]) -> set[str]:
        ignored = set()
        for name in names:
            if name in GENERATED_DIR_NAMES or name.endswith(".egg-info"):
                ignored.add(name)
        return ignored

    shutil.copytree(ctx.source_dir, stage, ignore=ignore)
    return stage


def check_toolchains(ctx: Context) -> int:
    if ctx.component == "driver:mojo":
        if mojo_launcher() is not None:
            return 0
        message = "driver:mojo missing toolchain(s): mojo or pixi+Mojo manifest"
        if ctx.require_all_toolchains:
            return fail(message)
        return print_skip(message)
    missing = [tool for tool in COMPONENT_TOOLCHAINS.get(ctx.component, []) if shutil.which(tool) is None]
    if not missing:
        return 0
    message = f"{ctx.component} missing toolchain(s): {', '.join(missing)}"
    if ctx.require_all_toolchains:
        return fail(message)
    if ctx.allow_toolchain_waivers:
        return print_skip(message)
    return print_skip(message)


def mojo_launcher() -> list[str] | None:
    mojo_bin = os.environ.get("MOJO_BIN", "").strip()
    if mojo_bin:
        return [mojo_bin]
    mojo_path = shutil.which("mojo")
    if mojo_path:
        return [mojo_path]
    pixi_path = shutil.which("pixi")
    if not pixi_path:
        return None
    manifest = os.environ.get("MOJO_PIXI_MANIFEST", "").strip()
    if not manifest:
        manifest = str(Path.home() / "mojo-work" / "sb-mojo")
    manifest_path = Path(manifest).expanduser()
    if not manifest_path.exists():
        return None
    return [pixi_path, "run", "-m", str(manifest_path), "--executable", "mojo"]


def check_source_artifacts(ctx: Context) -> int:
    offenders: list[str] = []
    drivers_root = ctx.project_root / "drivers"
    tracked_paths = git_tracked_paths(ctx, drivers_root)
    for path in drivers_root.rglob("*"):
        rel = path.relative_to(ctx.repo_root)
        if is_tracked_source_path(path, ctx, tracked_paths):
            continue
        if path.is_dir() and (path.name in GENERATED_DIR_NAMES or path.name.endswith(".egg-info")):
            offenders.append(str(rel))
        elif path.is_file() and path.suffix in GENERATED_SUFFIXES:
            offenders.append(str(rel))
    if offenders:
        return fail("generated artifacts under project/drivers after component run:\n" + "\n".join(offenders[:80]))
    return 0


def run_cmake_component(ctx: Context, source_dir: Path, extra_configure_args: list[str] | None = None) -> int:
    build_dir = ctx.component_build_root / "cmake"
    build_dir.mkdir(parents=True, exist_ok=True)
    configure = [
        "cmake",
        "-S",
        str(source_dir),
        "-B",
        str(build_dir),
        "-DBUILD_TESTING=ON",
        f"-DCMAKE_INSTALL_PREFIX={ctx.component_build_root / 'install'}",
    ] + (extra_configure_args or [])
    for name, argv in (
        ("configure", configure),
        ("build", ["cmake", "--build", str(build_dir), "--parallel"]),
        ("ctest", ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]),
    ):
        result = run_command(ctx, name, argv, cwd=ctx.repo_root)
        if result != 0:
            return result
    return 0


def run_cpp(ctx: Context) -> int:
    return run_cmake_component(ctx, ctx.source_dir)


def run_odbc(ctx: Context) -> int:
    return run_cmake_component(ctx, ctx.source_dir, ["-DODBC_FETCH_GTEST=OFF"])


def run_cli(ctx: Context) -> int:
    return run_cmake_component(ctx, ctx.source_dir, ["-DSB_BUILD_CLI_FDW=OFF"])


def run_go(ctx: Context) -> int:
    env = {"GOTMPDIR": str(ctx.component_build_root / "tmp")}
    return run_command(ctx, "go-test", ["go", "test", "./..."], cwd=ctx.source_dir, env=env)


def run_rust(ctx: Context) -> int:
    env = {"CARGO_TARGET_DIR": str(ctx.component_build_root / "target")}
    return run_command(ctx, "cargo-test", ["cargo", "test", "--locked"], cwd=ctx.source_dir, env=env)


def run_python_driver(ctx: Context) -> int:
    env = {
        "PYTHONPATH": str(ctx.source_dir / "src"),
        "PYTEST_ADDOPTS": f"-o cache_dir={ctx.component_build_root / 'pytest-cache'}",
    }
    return run_command(ctx, "pytest", ["python3", "-m", "pytest", "-q", "tests"], cwd=ctx.source_dir, env=env)


def run_python_adaptor(ctx: Context) -> int:
    env = {
        "PYTHONPATH": os.pathsep.join(
            [
                str(ctx.source_dir),
                str(ctx.project_root / "drivers" / "driver" / "python" / "src"),
            ]
        ),
        "PYTEST_ADDOPTS": f"-o cache_dir={ctx.component_build_root / 'pytest-cache'}",
    }
    return run_command(ctx, "pytest", ["python3", "-m", "pytest", "-q", "tests"], cwd=ctx.source_dir, env=env)


def run_node(ctx: Context) -> int:
    stage = stage_source(ctx)
    if (stage / "package-lock.json").is_file():
        result = run_command(ctx, "npm-ci", ["npm", "ci"], cwd=stage)
        if result != 0:
            return result
    return run_command(ctx, "npm-test", ["npm", "test"], cwd=stage)


def run_node_adaptor(ctx: Context) -> int:
    stage = stage_source(ctx)
    return run_command(ctx, "node-test", ["node", "--test"], cwd=stage)


def run_jdbc(ctx: Context) -> int:
    stage = stage_source(ctx)
    init_script = ctx.component_build_root / "offline-test-filter.gradle"
    init_script.write_text(
        """
allprojects {
    tasks.withType(Test).configureEach {
        exclude '**/JDBC203PoolingAndRecoveryContractTest.class'
        exclude '**/SBIntegrationTest.class'
        exclude '**/SBJdbcClosureParityTest.class'
        exclude '**/SBNativeSQLParityTest.class'
    }
}
""".lstrip(),
        encoding="utf-8",
    )
    if os.name == "nt" and (stage / "gradlew.bat").is_file():
        argv = [
            str(stage / "gradlew.bat"),
            "--no-daemon",
            "--console=plain",
            "-g",
            str(ctx.deps_root / "jvm" / "gradle-home"),
            "-I",
            str(init_script),
            "test",
        ]
    else:
        gradlew = stage / "gradlew"
        argv = [
            "bash",
            str(gradlew),
            "--no-daemon",
            "--console=plain",
            "-g",
            str(ctx.deps_root / "jvm" / "gradle-home"),
            "-I",
            str(init_script),
            "test",
        ]
    return run_command(ctx, "gradle-test", argv, cwd=stage, env={"JAVA_HOME": ""})


def run_maven(ctx: Context) -> int:
    stage = stage_source(ctx)
    argv = [
        "mvn",
        "-q",
        f"-Dmaven.repo.local={ctx.deps_root / 'jvm' / 'maven-repo'}",
        "test",
    ]
    return run_command(ctx, "maven-test", argv, cwd=stage)


def run_dbeaver(ctx: Context) -> int:
    stage = stage_source(ctx)
    result = run_command(
        ctx,
        "current-project-design",
        ["python3", "scripts/check-current-project-design.py"],
        cwd=stage,
    )
    if result != 0:
        return result

    env = {
        "SCRATCHBIRD_DRIVER_BUILD_ROOT": str(ctx.component_build_root),
        "SCRATCHBIRD_JDBC_DIR": str(ctx.project_root / "drivers" / "driver" / "jdbc"),
        "MAVEN_REPO_LOCAL": str(ctx.deps_root / "jvm" / "maven-repo"),
    }
    result = run_command(
        ctx,
        "build-p2-update-site",
        ["bash", "scripts/build-p2-update-site.sh", str(ctx.component_build_root / "dist")],
        cwd=stage,
        env=env,
    )
    if result != 0:
        return result
    return run_dbeaver_stock_install_smoke(ctx, stage)


def run_dbeaver_stock_install_smoke(ctx: Context, stage: Path) -> int:
    launcher = shutil.which("dbeaver") or shutil.which("dbeaver-ce")
    if launcher is None:
        print("adaptor:scratchbird-dbeaver-driver stock install smoke skipped: dbeaver launcher not found")
        return 0

    resolved_launcher = Path(launcher).resolve()
    install_root = resolved_launcher.parent
    if not (install_root / resolved_launcher.name).is_file():
        print(f"adaptor:scratchbird-dbeaver-driver stock install smoke skipped: unsupported launcher {resolved_launcher}")
        return 0

    zips = sorted((ctx.component_build_root / "dist").glob("scratchbird-dbeaver-update-site-*.zip"))
    if not zips:
        return fail("DBeaver stock install smoke could not find generated update-site zip")

    clone_root = ctx.component_build_root / "stock-install-smoke" / install_root.name
    if clone_root.exists():
        shutil.rmtree(clone_root)
    clone_root.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(install_root, clone_root, symlinks=True)

    return run_command(
        ctx,
        "stock-install-smoke",
        [
            "bash",
            "scripts/install-into-stock-dbeaver.sh",
            str(clone_root),
            str(zips[-1].resolve()),
        ],
        cwd=stage,
    )


def run_metabase(ctx: Context) -> int:
    stage = stage_source(ctx)
    env = {"CLJ_CONFIG": str(ctx.deps_root / "clojure" / "config")}
    argv = [
        "clojure",
        "-Sdeps",
        f"{{:mvn/local-repo \"{ctx.deps_root / 'jvm' / 'maven-repo'}\"}}",
        "-T:build",
        "jar",
    ]
    return run_command(ctx, "clojure-jar", argv, cwd=stage, env=env)


def run_dart(ctx: Context) -> int:
    stage = stage_source(ctx)
    for name, argv in (
        ("dart-pub-get", ["dart", "pub", "get"]),
        ("dart-test", ["dart", "test"]),
    ):
        result = run_command(ctx, name, argv, cwd=stage)
        if result != 0:
            return result
    return 0


def run_dotnet(ctx: Context) -> int:
    stage = stage_source(ctx)
    env = {"NUGET_PACKAGES": str(ctx.deps_root / "dotnet" / "packages")}
    argv = [
        "dotnet",
        "test",
        "ScratchBird.Data.sln",
        "--artifacts-path",
        str(ctx.component_build_root / "artifacts"),
        "--results-directory",
        str(ctx.component_build_root / "test-results"),
        "--filter",
        "FullyQualifiedName!~IntegrationTests&FullyQualifiedName!~JDBC203PoolingAndRecoveryContractTests&FullyQualifiedName!~SoakAndFaultInjectionTests",
    ]
    return run_command(ctx, "dotnet-test", argv, cwd=stage, env=env)


def run_elixir(ctx: Context) -> int:
    stage = stage_source(ctx)
    env = {
        "MIX_BUILD_PATH": str(ctx.component_build_root / "_build"),
        "MIX_DEPS_PATH": str(ctx.component_build_root / "deps"),
    }
    for name, argv in (
        ("mix-deps-get", ["mix", "deps.get"]),
        ("mix-test", ["mix", "test"]),
    ):
        result = run_command(ctx, name, argv, cwd=stage, env=env)
        if result != 0:
            return result
    return 0


def run_mojo(ctx: Context) -> int:
    env = {"SCRATCHBIRD_MOJO_NATIVE_RUN_ARGS": os.environ.get("SCRATCHBIRD_MOJO_NATIVE_RUN_ARGS", "-O0 -j1")}
    launcher = mojo_launcher()
    if launcher is None:
        return fail("driver:mojo missing Mojo launcher after toolchain check")
    tests = [
        [*launcher, "run", "-O0", "-j1", "-I", "src", "-I", "src/scratchbird", "tests/scratchbird_surface.mojo"],
        [*launcher, "run", "-O0", "-j1", "-I", "src", "-I", "src/scratchbird", "tests/native_bootstrap.mojo"],
        ["python3", "tests/integration.py"],
    ]
    for index, argv in enumerate(tests, start=1):
        result = run_command(ctx, f"mojo-test-{index}", argv, cwd=ctx.source_dir, env=env)
        if result != 0:
            return result
    return 0


def run_pascal(ctx: Context) -> int:
    unit_dir = ctx.component_build_root / "units"
    bin_dir = ctx.component_build_root / "bin"
    unit_dir.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(parents=True, exist_ok=True)
    tests = sorted((ctx.source_dir / "tests").glob("*.pas"))
    if not tests:
        return fail("no Pascal tests found")
    for test in tests:
        argv = [
            "fpc",
            f"-Fu{ctx.source_dir / 'src'}",
            f"-FU{unit_dir}",
            f"-FE{bin_dir}",
            str(test),
        ]
        result = run_command(ctx, f"fpc-{test.stem}", argv, cwd=ctx.source_dir)
        if result != 0:
            return result
        exe = bin_dir / test.stem
        if exe.exists():
            result = run_command(ctx, f"run-{test.stem}", [str(exe)], cwd=ctx.source_dir)
            if result != 0:
                return result
    return 0


def run_php(ctx: Context) -> int:
    stage = stage_source(ctx)
    result = run_command(ctx, "composer-install", ["composer", "install", "--no-interaction"], cwd=stage)
    if result != 0:
        return result
    return run_command(ctx, "composer-test", ["composer", "test"], cwd=stage)


def run_r(ctx: Context) -> int:
    stage = stage_source(ctx)
    env = {
        "R_LIBS_USER": str(ctx.deps_root / "r" / "library"),
        "TMPDIR": str(ctx.component_build_root / "tmp"),
    }
    expr = (
        "if (!requireNamespace('testthat', quietly=TRUE)) quit(status=77); "
        "if (!requireNamespace('pkgload', quietly=TRUE)) quit(status=77); "
        "pkgload::load_all('.'); testthat::test_dir('tests/testthat')"
    )
    return run_command(ctx, "r-testthat", ["Rscript", "-e", expr], cwd=stage, env=env)


def run_ruby(ctx: Context) -> int:
    expr = "Dir['test/test_*.rb'].sort.each { |path| require_relative path }"
    return run_command(ctx, "ruby-tests", ["ruby", f"-I{os.pathsep.join(['lib', 'test'])}", "-e", expr], cwd=ctx.source_dir)


def run_swift(ctx: Context) -> int:
    env = {"SWIFTPM_CACHE_PATH": str(ctx.deps_root / "swift" / "cache")}
    argv = [
        "swift",
        "test",
        "--scratch-path",
        str(ctx.component_build_root / ".build"),
    ]
    return run_command(ctx, "swift-test", argv, cwd=ctx.source_dir, env=env)


def read_manifest_row(ctx: Context) -> dict[str, str] | None:
    manifest = ctx.project_root / "drivers" / "DriverPackageManifest.csv"
    with manifest.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if row.get("component_id") == ctx.component:
                return row
    return None


def run_contract_package(ctx: Context) -> int:
    contract_path = ctx.source_dir / "package_contract.json"
    if not contract_path.is_file():
        return fail(f"{ctx.component} missing package_contract.json")
    try:
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return fail(f"{ctx.component} invalid package_contract.json: {exc}")

    manifest_row = read_manifest_row(ctx)
    if manifest_row is None:
        return fail(f"{ctx.component} missing DriverPackageManifest row")

    errors: list[str] = []
    if contract.get("component_id") != ctx.component:
        errors.append("component_id mismatch")
    if contract.get("category") != ctx.category:
        errors.append("category mismatch")
    if contract.get("name") != ctx.name:
        errors.append("name mismatch")
    if contract.get("driver_family") != manifest_row.get("driver_family"):
        errors.append("driver_family mismatch with DriverPackageManifest")
    contract_status = str(contract.get("status", ""))
    manifest_status = str(manifest_row.get("driver_status", ""))
    if contract_status not in ALLOWED_CONTRACT_STATUSES:
        errors.append("status must be one of " + ", ".join(sorted(ALLOWED_CONTRACT_STATUSES)))
    if contract_status != manifest_status:
        errors.append("status mismatch with DriverPackageManifest driver_status")
    if contract_status == "planned_not_implemented" and manifest_row.get("release_bucket") != "tracked_not_released":
        errors.append("planned_not_implemented contracts must use tracked_not_released release_bucket")
    if contract.get("wire_protocol") != "sbwp_v1_1":
        errors.append("wire_protocol must be sbwp_v1_1")
    if contract.get("auth_authority") != "engine":
        errors.append("auth_authority must be engine")
    if contract.get("transaction_authority") != "mga_engine":
        errors.append("transaction_authority must be mga_engine")

    route_requirements = set(contract.get("route_requirements", []))
    missing_route = sorted(REQUIRED_ROUTE_REQUIREMENTS - route_requirements)
    if missing_route:
        errors.append("route_requirements missing " + ", ".join(missing_route))

    conformance = set(contract.get("conformance", []))
    missing_conformance = sorted(REQUIRED_CONFORMANCE - conformance)
    if missing_conformance:
        errors.append("conformance missing " + ", ".join(missing_conformance))

    for rel in contract.get("package_files", []):
        if not (ctx.source_dir / rel).is_file():
            errors.append(f"package_files missing {rel}")

    if "sbwp_v1_1" not in manifest_row.get("wire_protocol_set", ""):
        errors.append("DriverPackageManifest wire_protocol_set missing sbwp_v1_1")
    if "engine_local_password" not in manifest_row.get("auth_method_set", ""):
        errors.append("DriverPackageManifest auth_method_set missing engine_local_password")
    if "scratchbird_tls_1_3_floor" not in manifest_row.get("tls_profile_set", ""):
        errors.append("DriverPackageManifest tls_profile_set missing scratchbird_tls_1_3_floor")

    if errors:
        return fail(f"{ctx.component} contract package errors:\n" + "\n".join(errors))
    print(f"{ctx.component} contract package gate ok")
    return 0


RUNNERS: dict[str, Callable[[Context], int]] = {
    "driver:adbc": run_contract_package,
    "driver:cpp": run_cpp,
    "driver:dart": run_dart,
    "driver:dotnet": run_dotnet,
    "driver:elixir": run_elixir,
    "driver:flightsql": run_contract_package,
    "driver:go": run_go,
    "driver:jdbc": run_jdbc,
    "driver:julia": run_contract_package,
    "driver:mojo": run_mojo,
    "driver:node": run_node,
    "driver:odbc": run_odbc,
    "driver:pascal": run_pascal,
    "driver:perl": run_contract_package,
    "driver:php": run_php,
    "driver:python": run_python_driver,
    "driver:r": run_r,
    "driver:r2dbc": run_contract_package,
    "driver:ruby": run_ruby,
    "driver:rust": run_rust,
    "driver:swift": run_swift,
    "adaptor:scratchbird-airbyte": run_contract_package,
    "adaptor:scratchbird-dbeaver-driver": run_dbeaver,
    "adaptor:scratchbird-dbt-adapter": run_contract_package,
    "adaptor:scratchbird-hibernate-dialect": run_maven,
    "adaptor:scratchbird-looker": run_contract_package,
    "adaptor:scratchbird-metabase-driver": run_metabase,
    "adaptor:scratchbird-powerbi": run_contract_package,
    "adaptor:scratchbird-prisma-adapter": run_node_adaptor,
    "adaptor:scratchbird-sqlalchemy-dialect": run_python_adaptor,
    "adaptor:scratchbird-superset-driver": run_python_adaptor,
    "adaptor:scratchbird-tableau": run_contract_package,
    "adaptor:scratchbird-typeorm-adapter": run_node_adaptor,
    "tool:cli": run_cli,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--component", required=True)
    parser.add_argument("--require-all-toolchains", action="store_true")
    parser.add_argument("--allow-toolchain-waivers", action="store_true")
    args = parser.parse_args()

    ctx = Context(
        repo_root=args.repo_root.resolve(),
        project_root=args.project_root.resolve(),
        build_root=args.build_root.absolute(),
        component=args.component,
        allow_toolchain_waivers=args.allow_toolchain_waivers,
        require_all_toolchains=args.require_all_toolchains,
    )
    ctx.component_build_root.mkdir(parents=True, exist_ok=True)
    if not ctx.source_dir.is_dir():
        return fail(f"component source missing: {ctx.source_dir}")
    if ctx.component not in RUNNERS:
        return fail(f"no component runner registered for {ctx.component}")

    toolchain_result = check_toolchains(ctx)
    if toolchain_result != 0:
        return toolchain_result

    result = RUNNERS[ctx.component](ctx)
    if result != 0:
        return result
    artifact_result = check_source_artifacts(ctx)
    if artifact_result != 0:
        return artifact_result
    print(f"{ctx.component} native component gate ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
