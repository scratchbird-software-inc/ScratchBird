#!/usr/bin/env python3
"""Benchmark run provenance helpers."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, Optional


PROVENANCE_SCHEMA_VERSION = 3
PROVENANCE_FILENAME = "run-provenance.json"

_CMAKE_CACHE_KEYS = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_C_COMPILER",
    "CMAKE_CXX_COMPILER",
    "CMAKE_GENERATOR",
    "CMAKE_SYSTEM_NAME",
    "SCRATCHBIRD_ENABLE_DEBUG_LOGS",
    "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
    "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE",
    "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
)

_FORBIDDEN_SCRATCHBIRD_RUNTIME_STORAGE_MARKERS = (
    ".sqlite-wal",
    ".sqlite-shm",
    ".sqlite",
    "-wal",
    "-shm",
    ".wal",
    ".shm",
)


def git_metadata_dir_name() -> str:
    return "." + "git"


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def load_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def parse_shell_env_file(path: Path) -> Dict[str, str]:
    if not path.exists():
        return {}

    values: Dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :]
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        values[key] = value
    return values


def scratchbird_repo_root_candidates(project_dir: Path) -> tuple[Path, ...]:
    """Return public ScratchBird source roots visible to the harness."""
    roots = []
    seen = set()

    def add(candidate: Path) -> None:
        resolved = candidate.resolve()
        if resolved in seen:
            return
        if (resolved / "project" / "src").exists() or (resolved / git_metadata_dir_name()).exists():
            roots.append(resolved)
            seen.add(resolved)

    for candidate in (project_dir, *project_dir.parents):
        add(candidate)

    for candidate in (project_dir, *project_dir.parents):
        add(candidate / "ScratchBird")

    return tuple(roots)


def scratchbird_server_binary_candidates(project_dir: Path) -> tuple[Path, ...]:
    """Return known current-project and legacy sb_server build locations."""
    candidates = []
    seen = set()
    for root in scratchbird_repo_root_candidates(project_dir):
        for relative in (
            Path("build") / "src" / "server" / "sb_server",
            Path("build") / "src" / "sb_server",
            Path("build_workplan") / "src" / "server" / "sb_server",
            Path("build_workplan") / "src" / "sb_server",
            Path("build") / "package_smoke" / "install" / "bin" / "sb_server",
        ):
            candidate = (root / relative).resolve()
            if candidate not in seen:
                candidates.append(candidate)
                seen.add(candidate)
    return tuple(candidates)


def git_repo_identity(repo_root: Path) -> Dict[str, Any]:
    repo_root = repo_root.resolve()
    payload: Dict[str, Any] = {
        "path": str(repo_root),
        "git_head": None,
        "git_dirty": None,
        "git_branch": None,
    }

    try:
        head = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
        payload["git_head"] = head.stdout.strip()
    except Exception:
        return payload

    try:
        branch = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "--abbrev-ref", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
        payload["git_branch"] = branch.stdout.strip()
    except Exception:
        payload["git_branch"] = None

    try:
        dirty = subprocess.run(
            ["git", "-C", str(repo_root), "status", "--porcelain"],
            check=True,
            capture_output=True,
            text=True,
        )
        payload["git_dirty"] = bool(dirty.stdout.strip())
    except Exception:
        payload["git_dirty"] = None

    return payload


def _find_cmake_build_dir(binary_path: Path, repo_root: Path) -> Optional[Path]:
    repo_root = repo_root.resolve()
    current = binary_path.resolve().parent
    while True:
        cache_path = current / "CMakeCache.txt"
        if cache_path.exists():
            return current
        if current == repo_root or current.parent == current:
            return None
        current = current.parent


def _parse_cmake_cache(cache_path: Path) -> Dict[str, str]:
    selected: Dict[str, str] = {}
    if not cache_path.exists():
        return selected
    for raw_line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw_line or raw_line.startswith(("//", "#")) or "=" not in raw_line or ":" not in raw_line:
            continue
        name_type, value = raw_line.split("=", 1)
        name, _cache_type = name_type.split(":", 1)
        if name in _CMAKE_CACHE_KEYS:
            selected[name] = value
    return selected


def _selected_environment() -> Dict[str, str]:
    prefixes = ("BENCHMARK_", "SCRATCHBIRD_", "STRESS_", "TPC_")
    selected = {
        key: value
        for key, value in os.environ.items()
        if key.startswith(prefixes)
    }
    return dict(sorted(selected.items()))


def _capture_json_document(path: Path) -> Dict[str, Any]:
    resolved_path = path.resolve()
    return {
        "file": str(resolved_path),
        "sha256": sha256_file(resolved_path),
        "payload": load_json(resolved_path),
    }


def _capture_provenance_floor(
    *,
    system_info_path: Optional[Path],
    extra_provenance_documents: Dict[str, Path],
) -> Dict[str, Any]:
    floor: Dict[str, Any] = {
        "requirements_version": 1,
    }

    if system_info_path and system_info_path.exists():
        floor["system_info"] = _capture_json_document(system_info_path)

    supplemental: Dict[str, Any] = {}
    for name, path in sorted(extra_provenance_documents.items()):
        if not path.exists():
            continue
        supplemental[name] = _capture_json_document(path)
    if supplemental:
        floor["supplemental_documents"] = supplemental

    return floor


def _string_path(value: Optional[str]) -> Optional[str]:
    if not value:
        return None
    return str(Path(value).expanduser().resolve())


def _classify_forbidden_runtime_storage(path: Path) -> Optional[str]:
    name = path.name.lower()
    for marker in _FORBIDDEN_SCRATCHBIRD_RUNTIME_STORAGE_MARKERS:
        if marker in name:
            return f"forbidden embedded storage/log artifact marker {marker!r}"
    return None


def scan_scratchbird_runtime_storage_policy(root_path: Path) -> Dict[str, Any]:
    """Reject ScratchBird benchmark evidence produced through embedded storage.

    ScratchBird benchmark artifacts must prove the MGA/SBLR engine path. Runtime
    files created by embedded storage engines are not valid comparison evidence,
    because their transaction, rollback, durability, and constraint behavior are
    not ScratchBird authority.
    """

    root_path = root_path.expanduser().resolve()
    forbidden: list[Dict[str, Any]] = []

    if root_path.exists():
        for path in sorted(root_path.rglob("*")):
            if path.is_dir():
                continue
            reason = _classify_forbidden_runtime_storage(path)
            if reason is None:
                continue
            try:
                stat = path.stat()
                size_bytes: Optional[int] = stat.st_size
                mtime_ns: Optional[int] = stat.st_mtime_ns
            except OSError:
                size_bytes = None
                mtime_ns = None
            forbidden.append(
                {
                    "path": str(path),
                    "relative_path": str(path.relative_to(root_path)),
                    "reason": reason,
                    "size_bytes": size_bytes,
                    "mtime_ns": mtime_ns,
                }
            )

    comparison_eligible = root_path.exists() and not forbidden
    if not root_path.exists():
        status = "root_missing"
        reason = "ScratchBird runtime root is missing; storage policy cannot be verified"
    elif forbidden:
        status = "failed"
        reason = (
            "ScratchBird runtime root contains embedded storage/log artifacts; "
            "this run is not MGA/SBLR engine evidence"
        )
    else:
        status = "passed"
        reason = "ScratchBird runtime root contains no embedded storage/log artifacts"

    return {
        "policy": "scratchbird_mga_no_embedded_storage_runtime_artifacts.v1",
        "root": str(root_path),
        "status": status,
        "comparison_eligible": comparison_eligible,
        "reason": reason,
        "forbidden_count": len(forbidden),
        "forbidden_artifacts": forbidden,
    }


def _capture_scratchbird_runtime(project_dir: Path) -> Dict[str, Any]:
    port_env_path = project_dir / ".benchmark-engine-ports" / "scratchbird.env"
    port_env = parse_shell_env_file(port_env_path)

    runtime_env_path = Path(
        port_env.get(
            "BENCHMARK_SCRATCHBIRD_RUNTIME_ENV",
            project_dir / ".benchmark-engine-ports" / "scratchbird.runtime.env",
        )
    ).resolve()
    runtime_env = parse_shell_env_file(runtime_env_path)

    root_path = Path(port_env["BENCHMARK_SCRATCHBIRD_ROOT"]).resolve()
    connections_json_path = root_path / "profiles" / "connections.json"

    server_binary_value = runtime_env.get("SCRATCHBIRD_SB_SERVER")
    if not server_binary_value:
        fallback_candidates = (
            [Path(os.environ["SCRATCHBIRD_SB_SERVER"]).expanduser()]
            if os.environ.get("SCRATCHBIRD_SB_SERVER")
            else []
        )
        fallback_candidates.extend(scratchbird_server_binary_candidates(project_dir))
        for candidate in fallback_candidates:
            if candidate and Path(candidate).exists():
                server_binary_value = str(candidate)
                break
    if not server_binary_value:
        raise KeyError("SCRATCHBIRD_SB_SERVER")

    server_binary_path = Path(server_binary_value).resolve()
    server_binary_stat = server_binary_path.stat()

    scratchbird_root = server_binary_path
    while scratchbird_root.name:
        candidate = scratchbird_root / git_metadata_dir_name()
        if candidate.exists():
            break
        if scratchbird_root.parent == scratchbird_root:
            break
        scratchbird_root = scratchbird_root.parent
    if not (scratchbird_root / git_metadata_dir_name()).exists():
        scratchbird_root = (
            scratchbird_repo_root_candidates(project_dir)[0]
            if scratchbird_repo_root_candidates(project_dir)
            else project_dir.resolve()
        )

    build_dir = _find_cmake_build_dir(server_binary_path, scratchbird_root)
    cache_path = build_dir / "CMakeCache.txt" if build_dir else None

    runtime_storage_policy = scan_scratchbird_runtime_storage_policy(root_path)

    runtime = {
        "root": str(root_path),
        "port_env_file": str(port_env_path.resolve()),
        "port_env_sha256": sha256_file(port_env_path),
        "port_env": port_env,
        "runtime_env_file": str(runtime_env_path),
        "runtime_env_sha256": sha256_file(runtime_env_path) if runtime_env_path.exists() else None,
        "runtime_env": runtime_env,
        "connections_json_file": str(connections_json_path),
        "connections_json_sha256": sha256_file(connections_json_path) if connections_json_path.exists() else None,
        "server_binary": {
            "path": str(server_binary_path),
            "realpath": str(server_binary_path.resolve()),
            "size_bytes": server_binary_stat.st_size,
            "mtime_ns": server_binary_stat.st_mtime_ns,
            "sha256": sha256_file(server_binary_path),
        },
        "build_identity": {
            "scratchbird_repo": git_repo_identity(scratchbird_root),
            "build_dir": str(build_dir.resolve()) if build_dir else None,
            "cmake_cache_file": str(cache_path.resolve()) if cache_path and cache_path.exists() else None,
            "cmake_cache_sha256": sha256_file(cache_path) if cache_path and cache_path.exists() else None,
            "cmake_cache_selected": _parse_cmake_cache(cache_path) if cache_path else {},
        },
        "runtime_storage_policy": runtime_storage_policy,
    }

    provenance_pinned = (
        bool(runtime["server_binary"]["path"])
        and bool(runtime["server_binary"]["realpath"])
        and bool(runtime["server_binary"]["sha256"])
        and bool(runtime["build_identity"]["scratchbird_repo"]["git_head"])
    )
    pinned = provenance_pinned and runtime_storage_policy["comparison_eligible"]

    if pinned:
        pinning_reason = (
            "absolute ScratchBird binary path, file fingerprint, repo/build identity, "
            "and MGA runtime-storage policy are recorded"
        )
    elif not provenance_pinned:
        pinning_reason = "required ScratchBird binary provenance fields are missing"
    else:
        pinning_reason = runtime_storage_policy["reason"]

    runtime["pinning"] = {
        "status": "pinned" if pinned else "unpinned",
        "comparison_eligible": pinned,
        "reason": pinning_reason,
    }
    return runtime


def _capture_docker_inspect(target: str) -> Optional[Dict[str, Any]]:
    try:
        result = subprocess.run(
            ["docker", "inspect", target],
            check=False,
            capture_output=True,
            text=True,
        )
    except Exception:
        return None
    if result.returncode != 0:
        return None
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None
    if not payload:
        return None
    return payload[0]


def _capture_reference_engine_runtime(project_dir: Path, engine: str) -> Optional[Dict[str, Any]]:
    engine_meta = {
        "firebird": {
            "container_name": "sb-benchmark-firebird",
            "image_name": "sb-benchmark-firebird:latest",
            "port_key": "BENCHMARK_FIREBIRD_PORT",
            "version_file": "firebird-version.json",
        },
        "mysql": {
            "container_name": "sb-benchmark-mysql",
            "image_name": "sb-benchmark-mysql:latest",
            "port_key": "BENCHMARK_MYSQL_PORT",
            "version_file": "mysql-version.json",
        },
        "postgresql": {
            "container_name": "sb-benchmark-postgresql",
            "image_name": "sb-benchmark-postgresql:latest",
            "port_key": "BENCHMARK_POSTGRESQL_PORT",
            "version_file": "postgresql-version.json",
        },
    }.get(engine)
    if engine_meta is None:
        return None

    port_env_path = project_dir / ".benchmark-engine-ports" / f"{engine}.env"
    port_env = parse_shell_env_file(port_env_path)
    version_path = project_dir / "results" / engine_meta["version_file"]
    container_inspect = _capture_docker_inspect(engine_meta["container_name"])
    image_inspect = _capture_docker_inspect(engine_meta["image_name"])

    runtime: Dict[str, Any] = {
        "port_env_file": str(port_env_path.resolve()),
        "port_env_sha256": sha256_file(port_env_path) if port_env_path.exists() else None,
        "port_env": port_env,
        "version_json_file": str(version_path.resolve()),
        "version_json_sha256": sha256_file(version_path) if version_path.exists() else None,
        "version_json": load_json(version_path) if version_path.exists() else None,
        "container_name": engine_meta["container_name"],
        "image_name": engine_meta["image_name"],
        "runtime_port": port_env.get(engine_meta["port_key"]),
    }

    if container_inspect:
        runtime["container"] = {
            "id": container_inspect.get("Id"),
            "created": container_inspect.get("Created"),
            "image_id": container_inspect.get("Image"),
            "config_image": ((container_inspect.get("Config") or {}).get("Image")),
            "restart_count": container_inspect.get("RestartCount"),
            "state": container_inspect.get("State"),
            "port_bindings": ((container_inspect.get("NetworkSettings") or {}).get("Ports")),
        }

    if image_inspect:
        runtime["image"] = {
            "id": image_inspect.get("Id"),
            "repo_tags": image_inspect.get("RepoTags"),
            "repo_digests": image_inspect.get("RepoDigests"),
            "created": image_inspect.get("Created"),
            "architecture": image_inspect.get("Architecture"),
            "os": image_inspect.get("Os"),
        }

    missing = []
    if not port_env_path.exists():
        missing.append("engine_port_env")
    if not runtime.get("runtime_port"):
        missing.append("runtime_port")
    if not version_path.exists():
        missing.append("version_json")
    if container_inspect is None:
        missing.append("docker_container_identity")
    if image_inspect is None:
        missing.append("docker_image_identity")

    runtime["pinning"] = {
        "status": "pinned" if not missing else "partial",
        "comparison_eligible": not missing,
        "reason": (
            "docker container/image identity, engine port env, and collected engine version are recorded"
            if not missing
            else f"missing reference provenance fields: {', '.join(missing)}"
        ),
    }
    return runtime


def _capture_repeatability_policy(runtime_options: Dict[str, Any]) -> Dict[str, Any]:
    minimum_runs_for_firm_claims = int(runtime_options.get("minimum_runs_for_firm_claims", 5))
    executed_runs = int(runtime_options.get("executed_runs", 1))
    warmup_runs = int(runtime_options.get("warmup_runs", 0))
    cold_runs = int(runtime_options.get("cold_runs", 0))
    warm_runs = int(runtime_options.get("warm_runs", max(executed_runs - cold_runs, 0)))
    warm_cold_separation = runtime_options.get("warm_cold_separation", "not_separated")
    summary_statistics = runtime_options.get("summary_statistics", "bundle_elapsed_only")
    tail_latency_statistics = runtime_options.get("tail_latency_statistics", "not_reported")
    variance_policy = runtime_options.get("variance_policy", "not_reported")
    outlier_policy = runtime_options.get("outlier_policy", "not_applied")
    best_run_policy = runtime_options.get("best_run_policy", "forbidden")

    eligibility_reasons = []
    if executed_runs < minimum_runs_for_firm_claims:
        eligibility_reasons.append(
            f"executed_runs={executed_runs} is below minimum_runs_for_firm_claims={minimum_runs_for_firm_claims}"
        )
    if warm_cold_separation == "not_separated":
        eligibility_reasons.append("warm and cold runs are not separated")
    if tail_latency_statistics == "not_reported":
        eligibility_reasons.append("tail latency statistics are not reported")
    if variance_policy == "not_reported":
        eligibility_reasons.append("variance policy is not reported")
    if outlier_policy == "not_applied":
        eligibility_reasons.append("outlier policy is not applied")

    return {
        "policy_version": 1,
        "governance": {
            "minimum_runs_for_firm_claims": minimum_runs_for_firm_claims,
            "warm_cold_separation_required": True,
            "required_summary_statistics": "median",
            "required_tail_latency_statistics": "p95,p99",
            "variance_reporting_required": True,
            "outlier_policy_required": True,
            "best_run_policy": "forbidden",
        },
        "execution": {
            "executed_runs": executed_runs,
            "warmup_runs": warmup_runs,
            "cold_runs": cold_runs,
            "warm_runs": warm_runs,
            "warm_cold_separation": warm_cold_separation,
            "summary_statistics": summary_statistics,
            "tail_latency_statistics": tail_latency_statistics,
            "variance_policy": variance_policy,
            "outlier_policy": outlier_policy,
            "best_run_policy": best_run_policy,
        },
        "firm_comparison_eligible": not eligibility_reasons,
        "eligibility_reason": (
            "repeatability policy meets firm-claim floor"
            if not eligibility_reasons
            else "; ".join(eligibility_reasons)
        ),
    }


def capture_run_provenance(
    *,
    project_dir: Path,
    engine: str,
    suite: str,
    output_dir: Path,
    runner_script: Path,
    runner_cwd: Path,
    runner_argv: Iterable[str],
    python_executable: Optional[str],
    runtime_options: Dict[str, Any],
    system_info_path: Optional[Path] = None,
    extra_provenance_documents: Optional[Dict[str, Path]] = None,
) -> Dict[str, Any]:
    project_dir = project_dir.resolve()
    output_dir = output_dir.resolve()
    runner_script = runner_script.resolve()
    runner_cwd = runner_cwd.resolve()
    extra_provenance_documents = extra_provenance_documents or {}

    payload: Dict[str, Any] = {
        "schema_version": PROVENANCE_SCHEMA_VERSION,
        "capture_kind": "benchmark_run_provenance",
        "captured_at_utc": utc_now_iso(),
        "engine": engine,
        "suite": suite,
        "artifact_root": str(output_dir),
        "runner": {
            "script_path": str(runner_script),
            "cwd": str(runner_cwd),
            "argv": list(runner_argv),
            "python_executable": python_executable,
        },
        "runtime_options": runtime_options,
        "benchmark_repo": git_repo_identity(project_dir),
        "selected_environment": _selected_environment(),
        "provenance_floor": _capture_provenance_floor(
            system_info_path=system_info_path.resolve() if system_info_path else None,
            extra_provenance_documents={
                name: path.resolve() for name, path in extra_provenance_documents.items()
            },
        ),
        "repeatability_policy": _capture_repeatability_policy(runtime_options),
    }

    if engine == "scratchbird":
        payload["scratchbird_runtime"] = _capture_scratchbird_runtime(project_dir)
    elif engine in {"firebird", "mysql", "postgresql"}:
        reference_runtime = _capture_reference_engine_runtime(project_dir, engine)
        if reference_runtime:
            payload["reference_engine_runtime"] = reference_runtime

    return payload


def write_run_provenance(output_path: Path, payload: Dict[str, Any]) -> None:
    output_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def provenance_file_for_result(result_file: Path) -> Path:
    return result_file.parent / PROVENANCE_FILENAME


def validate_scratchbird_result_provenance(result_file: Path) -> Dict[str, Any]:
    result_payload = load_json(result_file)
    engine = result_payload.get("metadata", {}).get("engine")
    if engine != "scratchbird":
        return {}

    provenance_path = provenance_file_for_result(result_file)
    if not provenance_path.exists():
        raise ValueError(f"ScratchBird result is missing {PROVENANCE_FILENAME}: {result_file}")

    provenance = load_json(provenance_path)
    scratchbird_runtime = provenance.get("scratchbird_runtime") or {}
    pinning = scratchbird_runtime.get("pinning") or {}
    server_binary = scratchbird_runtime.get("server_binary") or {}
    runner = provenance.get("runner") or {}
    storage_policy = scratchbird_runtime.get("runtime_storage_policy") or {}

    required = {
        "schema_version": provenance.get("schema_version") == PROVENANCE_SCHEMA_VERSION,
        "capture_kind": provenance.get("capture_kind") == "benchmark_run_provenance",
        "engine": provenance.get("engine") == "scratchbird",
        "runner_script": os.path.isabs(str(runner.get("script_path", ""))),
        "runner_argv": isinstance(runner.get("argv"), list) and len(runner["argv"]) > 0,
        "pinning_status": pinning.get("status") == "pinned",
        "comparison_eligible": pinning.get("comparison_eligible") is True,
        "binary_path": os.path.isabs(str(server_binary.get("path", ""))),
        "binary_realpath": os.path.isabs(str(server_binary.get("realpath", ""))),
        "binary_sha256": isinstance(server_binary.get("sha256"), str) and len(server_binary["sha256"]) == 64,
        "git_head": bool(((scratchbird_runtime.get("build_identity") or {}).get("scratchbird_repo") or {}).get("git_head")),
        "runtime_storage_policy": storage_policy.get("comparison_eligible") is True,
    }

    failed = [name for name, ok in required.items() if not ok]
    if failed:
        raise ValueError(
            f"ScratchBird result is unpinned and cannot be compared: {result_file} "
            f"(missing {', '.join(failed)})"
        )

    runtime_root = scratchbird_runtime.get("root")
    if runtime_root:
        observed_policy = scan_scratchbird_runtime_storage_policy(Path(runtime_root))
        if not observed_policy["comparison_eligible"]:
            raise ValueError(
                f"ScratchBird result is not MGA/SBLR engine evidence: {result_file} "
                f"({observed_policy['reason']})"
            )

    return provenance
