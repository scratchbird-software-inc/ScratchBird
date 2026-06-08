#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run live-native ScratchBird AI conformance against a real HTTP bridge target."""

from __future__ import annotations

import argparse
import contextlib
import errno
import importlib
import json
import os
import shlex
import socket
import stat
import subprocess
import sys
import time
from urllib import error as url_error
from urllib import request as url_request
from urllib import parse as url_parse
import xml.etree.ElementTree as ET
from dataclasses import dataclass, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

ROOT_DIR = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from scratchbird_ai.service import build_default_service  # noqa: E402
from scratchbird_ai.scratchbird_core_surface import build_scratchbird_core_surface_packet  # noqa: E402
from scratchbird_ai.settings import RuntimeSettings, load_runtime_settings  # noqa: E402


DEFAULT_OUTPUT_DIR = "artifacts/live_native_conformance"
SUPPORTED_INTERFACE_PROFILE_IDS = {
    "service_internal_v0",
    "mcp_local_v0",
    "mcp_remote_v0",
    "streaming_async_v0",
    "retrieval_ingest_v0",
}
SUPPORTED_RUNTIME_MODE_IDS = {
    "listener_direct",
    "manager_proxy",
    "local_ipc",
    "embedded_local_only",
}
SUPPORTED_TRANSPORT_PROFILE = "http_json_request_response"
DEFAULT_BRIDGE_HOST = "127.0.0.1"
DEFAULT_BRIDGE_PORT = 3095
DEFAULT_BRIDGE_TOKEN = "live-native-bridge-token"


@dataclass(slots=True)
class LiveNativeConfig:
    repo_root: Path
    output_dir: Path
    interface_profile_id: str
    covered_interface_profiles: tuple[str, ...]
    live_enabled: bool
    adapter_mode: str
    base_url: str
    token: str | None
    dialect: str
    query_text: str
    schema: str
    table: str
    database: str | None
    timeout_sec: float
    skip_metadata: bool
    scratchbird_server_version: str
    parser_compiler_version: str
    driver_runtime_version: str | None
    transport_profile: str
    auth_mode: str
    test_dataset_version: str
    seed_or_fixture_version: str
    launch_bridge: bool = False
    bridge_host: str = DEFAULT_BRIDGE_HOST
    bridge_port: int = DEFAULT_BRIDGE_PORT
    bridge_driver_src: str | None = None
    bridge_server_setup: str = "listener-only"
    runtime_env_path: str | None = None
    native_host: str | None = None
    native_port: int | None = None
    native_database: str | None = None
    native_user: str | None = None
    native_password: str | None = None
    native_sslmode: str = "disable"
    runtime_mode_id: str = "listener_direct"


@dataclass(slots=True)
class SmokeCommandResult:
    command: list[str]
    returncode: int
    stdout: str
    stderr: str
    duration_sec: float

    @property
    def passed(self) -> bool:
        return self.returncode == 0


SmokeRunner = Callable[[LiveNativeConfig], SmokeCommandResult]


@dataclass(slots=True)
class NativePreflightResult:
    passed: bool
    duration_sec: float
    dsn_redacted: str
    row_sample: list[Any]
    schema_count: int | None
    error: str | None = None
    runtime_diagnostics: dict[str, Any] | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "passed": self.passed,
            "duration_sec": round(self.duration_sec, 6),
            "dsn_redacted": self.dsn_redacted,
            "row_sample": list(self.row_sample),
            "schema_count": self.schema_count,
            "error": self.error,
            "runtime_diagnostics": self.runtime_diagnostics,
        }


@dataclass(slots=True)
class BridgeLaunchSession:
    base_url: str
    token: str | None
    log_path: str
    pid: int | None = None
    dsn_redacted: str | None = None
    driver_src: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "base_url": self.base_url,
            "token_present": bool(self.token),
            "log_path": self.log_path,
            "pid": self.pid,
            "dsn_redacted": self.dsn_redacted,
            "driver_src": self.driver_src,
        }


NativeProbeRunner = Callable[[LiveNativeConfig], NativePreflightResult]
BridgeLauncher = Callable[[LiveNativeConfig], contextlib.AbstractContextManager[BridgeLaunchSession]]


@dataclass(slots=True)
class ProfileProbeResult:
    profile_id: str
    passed: bool
    checks: list[str]
    errors: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "profile_id": self.profile_id,
            "passed": self.passed,
            "checks": list(self.checks),
            "errors": list(self.errors),
        }


ServiceInternalProbeRunner = Callable[[LiveNativeConfig, Any], ProfileProbeResult]
RetrievalProbeRunner = Callable[[LiveNativeConfig, Any], ProfileProbeResult]


def _exception_messages(exc: BaseException) -> list[str]:
    nested = getattr(exc, "exceptions", None)
    if isinstance(nested, tuple) and nested:
        messages: list[str] = []
        for item in nested:
            messages.extend(_exception_messages(item))
        return messages
    message = str(exc).strip()
    return [message or exc.__class__.__name__]


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _parse_bool(raw: str | None, default: bool = False) -> bool:
    if raw is None:
        return default
    normalized = raw.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    return default


def _env(name: str) -> str:
    return os.getenv(name, "").strip()


def _default_runtime_env_path() -> str:
    candidate = Path.home() / ".scratchbird" / "static-example" / "profiles" / "runtime.env"
    return str(candidate) if candidate.is_file() else ""


def _default_bridge_driver_src(repo_root: Path) -> str:
    candidate = (
        repo_root.parent
        / "drivers"
        / "driver"
        / "python"
        / "src"
    )
    return str(candidate) if candidate.is_dir() else ""


def _strip_wrapping_quotes(raw: str) -> str:
    if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in {"'", '"'}:
        return raw[1:-1]
    return raw


def _parse_export_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key:
            continue
        parsed = _strip_wrapping_quotes(value.strip())
        try:
            tokens = shlex.split(parsed, posix=True)
        except ValueError:
            tokens = [parsed]
        values[key] = tokens[0] if len(tokens) == 1 else parsed
    return values


def _resolve_native_target(config: LiveNativeConfig) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    runtime_values: dict[str, str] = {}
    runtime_env_path = str(config.runtime_env_path or "").strip()
    if runtime_env_path:
        path = Path(runtime_env_path).expanduser()
        if not path.is_file():
            errors.append(f"runtime_env_path not found: {path}")
        else:
            runtime_values = _parse_export_file(path)

    def pick(explicit: str | None, env_key: str) -> str | None:
        if explicit is not None and str(explicit).strip():
            return str(explicit).strip()
        runtime_value = runtime_values.get(env_key, "").strip()
        if runtime_value:
            return runtime_value
        env_value = os.getenv(env_key, "").strip()
        return env_value or None

    port_value = config.native_port
    if port_value is None:
        port_raw = pick(None, "SCRATCHBIRD_NATIVE_PORT")
        if port_raw:
            try:
                port_value = int(port_raw)
            except ValueError:
                errors.append(f"invalid SCRATCHBIRD_NATIVE_PORT value: {port_raw}")
                port_value = None

    target = {
        "host": pick(config.native_host, "SCRATCHBIRD_NATIVE_HOST"),
        "port": port_value,
        "database": pick(config.native_database, "SCRATCHBIRD_NATIVE_DB"),
        "user": pick(config.native_user, "SCRATCHBIRD_NATIVE_USER"),
        "password": pick(config.native_password, "SCRATCHBIRD_NATIVE_PASSWORD"),
        "sslmode": str(pick(config.native_sslmode, "SCRATCHBIRD_NATIVE_SSLMODE") or "disable"),
        "runtime_env_path": runtime_env_path or None,
    }
    return target, errors


def _build_native_dsn(target: dict[str, Any]) -> str:
    user = url_parse.quote(str(target["user"]), safe="")
    password = url_parse.quote(str(target["password"]), safe="")
    database = url_parse.quote(str(target["database"]), safe="")
    host = str(target["host"])
    port = int(target["port"])
    sslmode = url_parse.quote(str(target.get("sslmode", "disable")), safe="")
    return f"scratchbird://{user}:{password}@{host}:{port}/{database}?sslmode={sslmode}"


def _redact_dsn(dsn: str) -> str:
    parsed = url_parse.urlsplit(dsn)
    username = parsed.username or ""
    host = parsed.hostname or ""
    port = f":{parsed.port}" if parsed.port is not None else ""
    path = parsed.path or ""
    query = f"?{parsed.query}" if parsed.query else ""
    userinfo = f"{url_parse.quote(username, safe='')}:***@" if username else ""
    return f"{parsed.scheme}://{userinfo}{host}{port}{path}{query}"


def _local_runtime_ownership_path(config: LiveNativeConfig) -> Path | None:
    runtime_env_path = str(config.runtime_env_path or "").strip()
    if not runtime_env_path:
        return None
    runtime_env = Path(runtime_env_path).expanduser()
    if not runtime_env.is_file():
        return None
    ownership_path = runtime_env.with_name("runtime_ownership.json")
    return ownership_path if ownership_path.is_file() else None


def _probe_unix_socket_status(path: str) -> dict[str, Any]:
    if not path:
        return {
            "path": path,
            "exists": False,
            "is_socket": False,
            "connect_state": "missing",
        }

    socket_path = Path(path)
    try:
        stat_result = socket_path.stat()
        exists = True
        is_socket = stat.S_ISSOCK(stat_result.st_mode)
    except FileNotFoundError:
        return {
            "path": path,
            "exists": False,
            "is_socket": False,
            "connect_state": "missing",
        }
    except OSError as exc:
        return {
            "path": path,
            "exists": False,
            "is_socket": False,
            "connect_state": "stat_error",
            "error": str(exc),
        }

    if not is_socket:
        return {
            "path": path,
            "exists": exists,
            "is_socket": False,
            "connect_state": "not_socket",
        }

    if not hasattr(socket, "AF_UNIX"):
        return {
            "path": path,
            "exists": exists,
            "is_socket": True,
            "connect_state": "af_unix_unsupported",
        }

    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        probe.settimeout(0.25)
        probe.connect(path)
    except FileNotFoundError:
        connect_state = "missing"
        error = None
    except ConnectionRefusedError as exc:
        connect_state = "stale_or_not_listening"
        error = str(exc)
    except PermissionError as exc:
        connect_state = "permission_denied"
        error = str(exc)
    except socket.timeout as exc:
        connect_state = "connect_timeout"
        error = str(exc)
    except OSError as exc:
        if exc.errno == errno.ENOENT:
            connect_state = "missing"
        elif exc.errno == errno.ECONNREFUSED:
            connect_state = "stale_or_not_listening"
        else:
            connect_state = "connect_error"
        error = str(exc)
    else:
        connect_state = "listening"
        error = None
    finally:
        probe.close()

    status_row = {
        "path": path,
        "exists": exists,
        "is_socket": True,
        "connect_state": connect_state,
    }
    if error:
        status_row["error"] = error
    return status_row


def _inspect_engine_endpoint_diagnostics(
    config: LiveNativeConfig,
    native_target: dict[str, Any],
) -> dict[str, Any] | None:
    host = str(native_target.get("host") or "").strip().lower()
    if host not in {"127.0.0.1", "localhost", "::1"}:
        return None

    ownership_path = _local_runtime_ownership_path(config)
    if ownership_path is None:
        return None

    try:
        ownership = json.loads(ownership_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {
            "runtime_ownership_path": str(ownership_path),
            "ownership_load_error": str(exc),
        }

    native_owner = (
        ownership.get("publication", {})
        .get("native", {})
        .get("owner", {})
    )
    engine_endpoint = str(native_owner.get("engine_endpoint") or "").strip()
    if not engine_endpoint:
        return {
            "runtime_ownership_path": str(ownership_path),
            "ownership_has_native_owner": bool(native_owner),
            "engine_endpoint": None,
        }

    parser_endpoint = f"{engine_endpoint}.parser_v1"
    return {
        "runtime_ownership_path": str(ownership_path),
        "engine_endpoint": engine_endpoint,
        "base_socket": _probe_unix_socket_status(engine_endpoint),
        "parser_socket": _probe_unix_socket_status(parser_endpoint),
    }


def _summarize_engine_endpoint_diagnostics(diagnostics: dict[str, Any] | None) -> str | None:
    if not diagnostics:
        return None

    base_socket = diagnostics.get("base_socket")
    parser_socket = diagnostics.get("parser_socket")
    if not isinstance(base_socket, dict) or not isinstance(parser_socket, dict):
        load_error = diagnostics.get("ownership_load_error")
        if load_error:
            return f"runtime ownership load failed: {load_error}"
        return None

    engine_endpoint = diagnostics.get("engine_endpoint")
    base_state = str(base_socket.get("connect_state") or "unknown")
    parser_state = str(parser_socket.get("connect_state") or "unknown")
    return (
        f"engine endpoint diagnostics: engine_endpoint={engine_endpoint}; "
        f"base_socket={base_state}; parser_socket={parser_state}"
    )


def _ensure_bridge_driver_src(config: LiveNativeConfig) -> str | None:
    explicit = str(config.bridge_driver_src or "").strip()
    if explicit:
        path = Path(explicit).expanduser()
        return str(path) if path.is_dir() else None

    inferred = _default_bridge_driver_src(config.repo_root)
    if inferred:
        return inferred

    try:
        importlib.import_module("scratchbird")
    except ImportError:
        return None
    return None


def _import_scratchbird_driver(driver_src: str | None):
    if driver_src:
        path = str(Path(driver_src).expanduser())
        if path not in sys.path:
            sys.path.insert(0, path)
    return importlib.import_module("scratchbird")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=str(ROOT_DIR))
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--interface-profile-id", default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_PROFILE_ID") or "service_internal_v0")
    parser.add_argument(
        "--covered-profile",
        action="append",
        default=[],
        help=(
            "Additional live-native interface profile covered by this run. "
            "May be supplied multiple times."
        ),
    )
    parser.add_argument(
        "--enable-live",
        action="store_true",
        default=_parse_bool(os.getenv("SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED"), False),
        help="Require an explicitly configured live-native run.",
    )
    parser.add_argument(
        "--launch-bridge",
        action="store_true",
        default=_parse_bool(os.getenv("SCRATCHBIRD_AI_LIVE_NATIVE_LAUNCH_BRIDGE"), False),
        help="Launch a local ScratchBird AI HTTP bridge backed by the native ScratchBird listener.",
    )
    parser.add_argument("--adapter-mode", default=_env("SCRATCHBIRD_AI_ADAPTER_MODE"))
    parser.add_argument("--base-url", default=_env("SCRATCHBIRD_AI_HTTP_BASE_URL"))
    parser.add_argument(
        "--token",
        default=(
            _env("SCRATCHBIRD_AI_HTTP_API_TOKEN")
            or _env("SCRATCHBIRD_AI_BRIDGE_API_TOKEN")
        ),
    )
    parser.add_argument("--dialect", default=_env("SCRATCHBIRD_AI_SMOKE_DIALECT") or "native")
    parser.add_argument("--query-text", default=_env("SCRATCHBIRD_AI_SMOKE_QUERY") or "SELECT 1")
    parser.add_argument("--schema", default=_env("SCRATCHBIRD_AI_SMOKE_SCHEMA"))
    parser.add_argument("--table", default=_env("SCRATCHBIRD_AI_SMOKE_TABLE"))
    parser.add_argument("--database", default=_env("SCRATCHBIRD_AI_SMOKE_DATABASE"))
    parser.add_argument(
        "--timeout-sec",
        type=float,
        default=float(_env("SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC") or "10"),
    )
    parser.add_argument("--skip-metadata", action="store_true")
    parser.add_argument(
        "--scratchbird-server-version",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION"),
    )
    parser.add_argument(
        "--parser-compiler-version",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION"),
    )
    parser.add_argument(
        "--driver-runtime-version",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_DRIVER_RUNTIME_VERSION"),
    )
    parser.add_argument(
        "--transport-profile",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_TRANSPORT_PROFILE") or SUPPORTED_TRANSPORT_PROFILE,
    )
    parser.add_argument(
        "--auth-mode",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_AUTH_MODE"),
    )
    parser.add_argument(
        "--test-dataset-version",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION"),
    )
    parser.add_argument(
        "--seed-or-fixture-version",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION"),
    )
    parser.add_argument(
        "--runtime-mode-id",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_MODE_ID") or "listener_direct",
    )
    parser.add_argument(
        "--bridge-host",
        default=_env("SCRATCHBIRD_AI_BRIDGE_HOST") or DEFAULT_BRIDGE_HOST,
    )
    parser.add_argument(
        "--bridge-port",
        type=int,
        default=int(_env("SCRATCHBIRD_AI_BRIDGE_PORT") or str(DEFAULT_BRIDGE_PORT)),
    )
    parser.add_argument(
        "--bridge-driver-src",
        default=(
            _env("SCRATCHBIRD_AI_LIVE_NATIVE_DRIVER_SRC")
            or _env("SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC")
        ),
    )
    parser.add_argument(
        "--bridge-server-setup",
        default=_env("SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP") or "listener-only",
    )
    parser.add_argument(
        "--runtime-env-path",
        default=_env("SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH") or _default_runtime_env_path(),
    )
    parser.add_argument("--native-host", default=_env("SCRATCHBIRD_NATIVE_HOST"))
    parser.add_argument("--native-port", type=int, default=None)
    parser.add_argument("--native-database", default=_env("SCRATCHBIRD_NATIVE_DB"))
    parser.add_argument("--native-user", default=_env("SCRATCHBIRD_NATIVE_USER"))
    parser.add_argument("--native-password", default=_env("SCRATCHBIRD_NATIVE_PASSWORD"))
    parser.add_argument(
        "--native-sslmode",
        default=_env("SCRATCHBIRD_NATIVE_SSLMODE") or "disable",
    )
    return parser


def config_from_args(args: argparse.Namespace) -> LiveNativeConfig:
    base_settings = load_runtime_settings()
    adapter_mode = (args.adapter_mode or base_settings.adapter_mode or "mock").strip().lower()
    launch_bridge = bool(args.launch_bridge)
    bridge_host = (args.bridge_host or DEFAULT_BRIDGE_HOST).strip() or DEFAULT_BRIDGE_HOST
    bridge_port = int(args.bridge_port or DEFAULT_BRIDGE_PORT)
    default_bridge_url = f"http://{bridge_host}:{bridge_port}"
    base_url = (
        default_bridge_url
        if launch_bridge
        else (args.base_url or base_settings.http_base_url).rstrip("/")
    )
    token = (args.token or base_settings.http_api_token or "").strip() or None
    if launch_bridge and not token:
        token = DEFAULT_BRIDGE_TOKEN
    auth_mode = (args.auth_mode or ("bearer_token" if token else "none")).strip().lower()
    driver_runtime_version = (args.driver_runtime_version or "").strip() or None
    covered_profiles = list(args.covered_profile or [])
    covered_profiles.extend(
        [
            item
            for item in (
                os.getenv("SCRATCHBIRD_AI_LIVE_NATIVE_COVERED_PROFILES", "").split(",")
            )
            if item.strip()
        ]
    )
    primary_profile = args.interface_profile_id.strip()
    if primary_profile:
        covered_profiles.insert(0, primary_profile)
    normalized_profiles: list[str] = []
    seen_profiles: set[str] = set()
    for profile_id in covered_profiles:
        normalized = str(profile_id).strip()
        if not normalized or normalized in seen_profiles:
            continue
        normalized_profiles.append(normalized)
        seen_profiles.add(normalized)
    return LiveNativeConfig(
        repo_root=Path(args.repo_root).resolve(),
        output_dir=Path(args.output_dir).resolve(),
        interface_profile_id=primary_profile,
        covered_interface_profiles=tuple(normalized_profiles or [primary_profile]),
        live_enabled=bool(args.enable_live),
        adapter_mode=adapter_mode,
        base_url=base_url,
        token=token,
        dialect=args.dialect.strip().lower(),
        query_text=args.query_text,
        schema=args.schema,
        table=args.table,
        database=(args.database or "").strip() or None,
        timeout_sec=float(args.timeout_sec),
        skip_metadata=bool(args.skip_metadata),
        scratchbird_server_version=args.scratchbird_server_version.strip(),
        parser_compiler_version=args.parser_compiler_version.strip(),
        driver_runtime_version=driver_runtime_version,
        transport_profile=args.transport_profile.strip(),
        auth_mode=auth_mode,
        test_dataset_version=args.test_dataset_version.strip(),
        seed_or_fixture_version=args.seed_or_fixture_version.strip(),
        launch_bridge=launch_bridge,
        bridge_host=bridge_host,
        bridge_port=bridge_port,
        bridge_driver_src=(
            (args.bridge_driver_src or "").strip()
            or _default_bridge_driver_src(Path(args.repo_root).resolve())
            or None
        ),
        bridge_server_setup=(args.bridge_server_setup or "listener-only").strip(),
        runtime_env_path=(args.runtime_env_path or "").strip() or None,
        native_host=(args.native_host or "").strip() or None,
        native_port=args.native_port,
        native_database=(args.native_database or "").strip() or None,
        native_user=(args.native_user or "").strip() or None,
        native_password=(args.native_password or "").strip() or None,
        native_sslmode=(args.native_sslmode or "disable").strip() or "disable",
        runtime_mode_id=(args.runtime_mode_id or "listener_direct").strip() or "listener_direct",
    )


def runtime_settings_from_config(config: LiveNativeConfig) -> RuntimeSettings:
    base = load_runtime_settings()
    return replace(
        base,
        adapter_mode=config.adapter_mode,
        http_base_url=config.base_url,
        http_timeout_sec=config.timeout_sec,
        http_api_token=config.token,
    )


def validate_live_native_config(
    config: LiveNativeConfig,
    *,
    runtime_settings: RuntimeSettings,
) -> list[str]:
    errors: list[str] = []
    if not config.live_enabled:
        errors.append(
            "SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED must be set or --enable-live must be passed"
        )
    if not config.covered_interface_profiles:
        errors.append("at least one covered interface profile must be declared")
    for profile_id in config.covered_interface_profiles:
        if profile_id not in SUPPORTED_INTERFACE_PROFILE_IDS:
            errors.append(
                f"unsupported interface_profile_id={profile_id}; supported={sorted(SUPPORTED_INTERFACE_PROFILE_IDS)}"
            )
    if config.interface_profile_id not in set(config.covered_interface_profiles):
        errors.append("primary interface_profile_id must be included in covered_interface_profiles")
    if runtime_settings.normalized_mode() not in {"http", "hybrid"}:
        errors.append(
            f"adapter_mode must be http or hybrid for live-native runs, got {runtime_settings.normalized_mode()}"
        )
    if not config.base_url and not config.launch_bridge:
        errors.append("http_base_url must be configured")
    if not config.runtime_mode_id:
        errors.append("runtime_mode_id must be declared")
    elif config.runtime_mode_id not in SUPPORTED_RUNTIME_MODE_IDS:
        errors.append(
            f"runtime_mode_id must be one of {sorted(SUPPORTED_RUNTIME_MODE_IDS)}, got {config.runtime_mode_id}"
        )
    if config.transport_profile != SUPPORTED_TRANSPORT_PROFILE:
        errors.append(
            f"transport_profile must be {SUPPORTED_TRANSPORT_PROFILE}, got {config.transport_profile}"
        )
    if not runtime_settings.should_use_http_for_dialect(config.dialect):
        errors.append(
            f"dialect {config.dialect} is not configured for HTTP transport under adapter_mode={runtime_settings.normalized_mode()}"
        )
    required_metadata = {
        "scratchbird_server_version": config.scratchbird_server_version,
        "parser_compiler_version": config.parser_compiler_version,
        "test_dataset_version": config.test_dataset_version,
        "seed_or_fixture_version": config.seed_or_fixture_version,
    }
    for field, value in required_metadata.items():
        if not value or value.lower() == "unknown":
            errors.append(f"{field} must be provided for live-native certification")
    if {"mcp_remote_v0", "streaming_async_v0"} & set(config.covered_interface_profiles):
        if not config.token:
            errors.append("remote MCP live-native coverage requires --token or SCRATCHBIRD_AI_HTTP_API_TOKEN")
    if config.launch_bridge:
        native_target, native_errors = _resolve_native_target(config)
        errors.extend(native_errors)
        for field in ("host", "port", "database", "user", "password"):
            if not native_target.get(field):
                errors.append(f"launch-bridge requires native {field} to be configured")
    return errors


def _smoke_command(config: LiveNativeConfig) -> list[str]:
    command = [
        sys.executable,
        "tools/smoke_http_contract.py",
        "--mode",
        "live",
        "--base-url",
        config.base_url,
        "--dialect",
        config.dialect,
        "--query-text",
        config.query_text,
        "--schema",
        config.schema,
        "--table",
        config.table,
        "--timeout-sec",
        str(config.timeout_sec),
    ]
    if config.token:
        command.extend(["--token", config.token])
    if config.database:
        command.extend(["--database", config.database])
    if config.skip_metadata:
        command.append("--skip-metadata")
    return command


def _runtime_mode_bridge_setup(runtime_mode_id: str) -> str:
    normalized = str(runtime_mode_id).strip().lower()
    if normalized == "manager_proxy":
        return "managed"
    if normalized == "local_ipc":
        return "ipc-only"
    if normalized == "embedded_local_only":
        return "embedded"
    return "listener-only"


def _runtime_mode_report(config: LiveNativeConfig) -> dict[str, Any]:
    packet = build_scratchbird_core_surface_packet()
    admitted = packet["runtime_mode_truth_packet"]["admitted_modes"]
    mode_rows = {
        str(row.get("mode_id", "")).strip(): dict(row)
        for row in admitted
        if str(row.get("mode_id", "")).strip()
    }
    row = mode_rows.get(
        config.runtime_mode_id,
        {
            "mode_id": config.runtime_mode_id,
            "transport_family": "unknown",
            "support_state": "unknown",
            "required_conditions": [],
        },
    )
    return {
        "generated_at_utc": _utc_now(),
        "runtime_mode_id": config.runtime_mode_id,
        "bridge_server_setup": _runtime_mode_bridge_setup(config.runtime_mode_id),
        "runtime_mode": row,
        "supported_runtime_modes": sorted(SUPPORTED_RUNTIME_MODE_IDS),
    }


def run_smoke_command(config: LiveNativeConfig) -> SmokeCommandResult:
    command = _smoke_command(config)
    env = os.environ.copy()
    existing = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = str(SRC_DIR) if not existing else str(SRC_DIR) + os.pathsep + existing
    started = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=config.repo_root,
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    return SmokeCommandResult(
        command=command,
        returncode=completed.returncode,
        stdout=completed.stdout,
        stderr=completed.stderr,
        duration_sec=time.perf_counter() - started,
    )


def run_native_preflight(config: LiveNativeConfig) -> NativePreflightResult:
    native_target, errors = _resolve_native_target(config)
    dsn = (
        _build_native_dsn(native_target)
        if not errors and all(native_target.get(field) for field in ("host", "port", "database", "user", "password"))
        else "scratchbird://<invalid>"
    )
    dsn_redacted = _redact_dsn(dsn) if dsn != "scratchbird://<invalid>" else dsn
    if errors:
        return NativePreflightResult(
            passed=False,
            duration_sec=0.0,
            dsn_redacted=dsn_redacted,
            row_sample=[],
            schema_count=None,
            error="; ".join(errors),
        )

    started = time.perf_counter()
    try:
        scratchbird = _import_scratchbird_driver(_ensure_bridge_driver_src(config))
        conn = scratchbird.connect(dsn=dsn, protocol="native")
        try:
            cursor = conn.cursor()
            cursor.execute(config.query_text)
            row = cursor.fetchone()
            row_sample = list(row) if row is not None else []
            schema_count: int | None = None
            try:
                schemas = conn.schemas(catalog=native_target["database"])
                schema_count = len(schemas) if isinstance(schemas, list) else None
            except Exception:
                schema_count = None
        finally:
            conn.close()
    except Exception as exc:
        runtime_diagnostics = _inspect_engine_endpoint_diagnostics(config, native_target)
        diagnostic_summary = _summarize_engine_endpoint_diagnostics(runtime_diagnostics)
        error_message = str(exc)
        if diagnostic_summary:
            error_message = f"{error_message} | {diagnostic_summary}"
        return NativePreflightResult(
            passed=False,
            duration_sec=time.perf_counter() - started,
            dsn_redacted=dsn_redacted,
            row_sample=[],
            schema_count=None,
            error=error_message,
            runtime_diagnostics=runtime_diagnostics,
        )

    return NativePreflightResult(
        passed=True,
        duration_sec=time.perf_counter() - started,
        dsn_redacted=dsn_redacted,
        row_sample=row_sample,
        schema_count=schema_count,
    )


@contextlib.contextmanager
def launch_local_bridge(config: LiveNativeConfig):
    native_target, errors = _resolve_native_target(config)
    if errors:
        raise RuntimeError("; ".join(errors))

    dsn = _build_native_dsn(native_target)
    driver_src = _ensure_bridge_driver_src(config)
    log_path = config.output_dir / "bridge.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    token = config.token or DEFAULT_BRIDGE_TOKEN
    base_url = f"http://{config.bridge_host}:{config.bridge_port}"

    env = os.environ.copy()
    py_paths = [str(SRC_DIR)]
    if driver_src:
        py_paths.append(driver_src)
    existing = env.get("PYTHONPATH", "")
    if existing:
        py_paths.append(existing)
    env["PYTHONPATH"] = os.pathsep.join(py_paths)
    env["SCRATCHBIRD_AI_BRIDGE_HOST"] = config.bridge_host
    env["SCRATCHBIRD_AI_BRIDGE_PORT"] = str(config.bridge_port)
    env["SCRATCHBIRD_AI_BRIDGE_API_TOKEN"] = token
    env["SCRATCHBIRD_AI_BRIDGE_DIALECTS"] = config.dialect
    env["SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN"] = dsn
    env["SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP"] = _runtime_mode_bridge_setup(config.runtime_mode_id)
    if driver_src:
        env["SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC"] = driver_src

    with log_path.open("w", encoding="utf-8") as handle:
        process = subprocess.Popen(
            [sys.executable, "-m", "scratchbird_ai.http_bridge"],
            cwd=config.repo_root,
            env=env,
            stdout=handle,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            deadline = time.monotonic() + max(5.0, config.timeout_sec)
            last_error: Exception | None = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    status, body = _request_json(
                        method="GET",
                        url=f"{base_url}/healthz",
                        token=token,
                        payload=None,
                        timeout_sec=1.0,
                    )
                    if status == 200 and body.get("status") == "ok":
                        break
                except Exception as exc:  # noqa: BLE001
                    last_error = exc
                time.sleep(0.2)
            else:
                process.terminate()
                process.wait(timeout=5)
                raise RuntimeError(
                    f"bridge failed to become healthy at {base_url}: {last_error or 'timeout'}"
                )

            if process.poll() is not None:
                raise RuntimeError(
                    f"bridge exited before becoming healthy (log: {log_path})"
                )

            yield BridgeLaunchSession(
                base_url=base_url,
                token=token,
                log_path=str(log_path),
                pid=process.pid,
                dsn_redacted=_redact_dsn(dsn),
                driver_src=driver_src,
            )
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _request_json(
    *,
    method: str,
    url: str,
    token: str | None,
    payload: dict[str, Any] | None,
    timeout_sec: float,
) -> tuple[int, dict[str, Any]]:
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = None
    if payload is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(payload).encode("utf-8")
    req = url_request.Request(url=url, method=method.upper(), data=data, headers=headers)
    try:
        with url_request.urlopen(req, timeout=timeout_sec) as resp:
            status = resp.status
            body = resp.read().decode("utf-8")
    except url_error.HTTPError as exc:
        status = exc.code
        body = exc.read().decode("utf-8")
    decoded = json.loads(body) if body else {}
    if not isinstance(decoded, dict):
        raise RuntimeError(f"Expected JSON object response from {method} {url}")
    return status, decoded


def _request_text(
    *,
    method: str,
    url: str,
    token: str | None,
    timeout_sec: float,
) -> tuple[int, str, str]:
    headers = {"Accept": "text/event-stream"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = url_request.Request(url=url, method=method.upper(), headers=headers)
    with url_request.urlopen(req, timeout=timeout_sec) as resp:
        return resp.status, resp.read().decode("utf-8"), resp.headers.get_content_type()


def _security_context() -> dict[str, Any]:
    return {
        "tenant_id": "live_native_tenant",
        "actor_id": "live_native_actor",
        "roles": ["analyst"],
        "session_id": "live_native_session",
        "context_version": 1,
    }


def _probe_service_internal(
    config: LiveNativeConfig,
    *,
    service: Any,
) -> ProfileProbeResult:
    checks: list[str] = []
    errors: list[str] = []
    security_context = _security_context()
    try:
        readonly = service.execute_readonly_query(
            request_id="req_live_service_internal_read",
            dialect=config.dialect,
            query_text=config.query_text,
            security_context=security_context,
            options={"max_rows": 1},
        )
        if int(readonly.get("row_count", 0) or 0) < 1:
            raise RuntimeError(f"execute_readonly_query returned no rows: {readonly}")
        checks.append("service_internal_execute_readonly_query")

        explain = service.explain_query(
            dialect=config.dialect,
            query_text=config.query_text,
            context={"security_context": security_context},
        )
        if not str(explain.get("plan_hash", "")).strip():
            raise RuntimeError(f"explain_query missing plan_hash: {explain}")
        operator_tree = explain.get("operator_tree")
        if not isinstance(operator_tree, dict) or operator_tree.get("operator_id") != "root":
            raise RuntimeError(f"explain_query missing operator tree root: {explain}")
        checks.append("service_internal_explain_query")

        trace_ids: list[str] = []
        for idx in range(3):
            response = service.run_query(
                request_id=f"req_live_workload_{idx}",
                dialect=config.dialect,
                query_text=config.query_text,
                mode="ai_analysis",
                options={"max_rows": 1},
                context={"security_context": security_context},
            )
            if int(response.row_count) < 1:
                raise RuntimeError(f"run_query returned no rows on iteration {idx}")
            trace_ids.append(str(response.trace_id))
        if len(trace_ids) != 3:
            raise RuntimeError("workload probe did not produce the expected trace count")
        checks.append("service_internal_workload_batch")

        bundle = service.latest_audit_bundle()
        if not isinstance(bundle, dict):
            raise RuntimeError("latest audit bundle missing after workload probe")
        replay = service.replay_audit_bundle(
            bundle=bundle,
            security_context=security_context,
            expected_policy_decision="allow",
            expected_plan_hash=str(bundle.get("plan_hash", "")).strip() or None,
        )
        if not replay.get("matches"):
            raise RuntimeError(f"audit replay failed: {replay}")
        checks.append("service_internal_audit_replay")
    except Exception as exc:
        errors.append(str(exc))
    return ProfileProbeResult(
        profile_id="service_internal_v0",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _probe_remote_request_response(config: LiveNativeConfig) -> ProfileProbeResult:
    checks: list[str] = []
    errors: list[str] = []
    security_context = _security_context()
    try:
        status, opened = _request_json(
            method="POST",
            url=f"{config.base_url}/v1/mcp/session/open",
            token=config.token,
            payload={
                "request_id": "req_live_remote_open",
                "interface_profile_id": "mcp_remote_v0",
                "protocol_version": "v0",
                "requested_transport": "https_json_request_response",
                "client_id": "live-native-conformance",
                "client_version": "0.1.0",
                "client_capabilities": {"streaming": False},
                "auth_envelope": {
                    "auth_type": "bearer",
                    "token": config.token,
                    "security_context": security_context,
                },
            },
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or not opened.get("session_id"):
            raise RuntimeError(f"session open failed status={status} body={opened}")
        checks.append("remote_session_open")
        session_id = str(opened["session_id"])

        status, invoked = _request_json(
            method="POST",
            url=f"{config.base_url}/v1/mcp/sessions/{session_id}/invoke",
            token=config.token,
            payload={
                "request_id": "req_live_remote_invoke",
                "method": "execute_readonly_query",
                "params": {
                    "dialect": config.dialect,
                    "query_text": config.query_text,
                    "options": {"max_rows": 1},
                },
            },
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or invoked.get("status") != "success":
            raise RuntimeError(f"remote invoke failed status={status} body={invoked}")
        checks.append("remote_execute_readonly_query")

        status, closed = _request_json(
            method="POST",
            url=f"{config.base_url}/v1/mcp/sessions/{session_id}/close",
            token=config.token,
            payload={"request_id": "req_live_remote_close"},
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or closed.get("status") not in {"closed", "already_closed"}:
            raise RuntimeError(f"remote close failed status={status} body={closed}")
        checks.append("remote_session_close")
    except Exception as exc:
        errors.append(str(exc))
    return ProfileProbeResult(
        profile_id="mcp_remote_v0",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _probe_remote_streaming(config: LiveNativeConfig) -> ProfileProbeResult:
    checks: list[str] = []
    errors: list[str] = []
    security_context = _security_context()
    try:
        status, opened = _request_json(
            method="POST",
            url=f"{config.base_url}/v1/mcp/session/open",
            token=config.token,
            payload={
                "request_id": "req_live_stream_open",
                "interface_profile_id": "mcp_remote_v0",
                "protocol_version": "v0",
                "requested_transport": "https_sse_server_stream",
                "client_id": "live-native-conformance",
                "client_version": "0.1.0",
                "client_capabilities": {"streaming": True},
                "auth_envelope": {
                    "auth_type": "bearer",
                    "token": config.token,
                    "security_context": security_context,
                },
            },
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or not opened.get("session_id"):
            raise RuntimeError(f"stream session open failed status={status} body={opened}")
        checks.append("stream_session_open")
        session_id = str(opened["session_id"])

        status, invoked = _request_json(
            method="POST",
            url=f"{config.base_url}/v1/mcp/sessions/{session_id}/invoke",
            token=config.token,
            payload={
                "request_id": "req_live_stream_invoke",
                "method": "execute_readonly_query",
                "stream_requested": True,
                "params": {
                    "dialect": config.dialect,
                    "query_text": config.query_text,
                    "options": {"max_rows": 1},
                },
            },
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or invoked.get("operation_id") is None:
            raise RuntimeError(f"stream invoke failed status={status} body={invoked}")
        checks.append("stream_invocation_created")
        operation_id = str(invoked["operation_id"])

        status, polled = _request_json(
            method="GET",
            url=f"{config.base_url}/v1/mcp/sessions/{session_id}/operations/{operation_id}",
            token=config.token,
            payload=None,
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or not isinstance(polled.get("events"), list):
            raise RuntimeError(f"stream poll failed status={status} body={polled}")
        if not polled["events"]:
            raise RuntimeError("stream poll returned no events")
        checks.append("stream_poll_events")

        status, event_stream, content_type = _request_text(
            method="GET",
            url=f"{config.base_url}/v1/mcp/sessions/{session_id}/operations/{operation_id}/events",
            token=config.token,
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or content_type != "text/event-stream":
            raise RuntimeError(
                f"stream events endpoint failed status={status} content_type={content_type}"
            )
        if "event: accepted" not in event_stream or "event: stream_end" not in event_stream:
            raise RuntimeError("stream events payload missing accepted/stream_end markers")
        checks.append("stream_sse_events")

        status, cancelled = _request_json(
            method="POST",
            url=f"{config.base_url}/v1/mcp/sessions/{session_id}/operations/{operation_id}/cancel",
            token=config.token,
            payload={
                "request_id": "req_live_stream_cancel",
                "reason": "post_completion_cleanup",
            },
            timeout_sec=config.timeout_sec,
        )
        if status != 200 or cancelled.get("status") not in {
            "already_completed",
            "accepted",
            "already_terminal",
        }:
            raise RuntimeError(f"stream cancel failed status={status} body={cancelled}")
        checks.append("stream_cancel_contract")

        _request_json(
            method="POST",
            url=f"{config.base_url}/v1/mcp/sessions/{session_id}/close",
            token=config.token,
            payload={"request_id": "req_live_stream_close"},
            timeout_sec=config.timeout_sec,
        )
    except Exception as exc:
        errors.append(str(exc))
    return ProfileProbeResult(
        profile_id="streaming_async_v0",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _probe_retrieval_ingest(
    config: LiveNativeConfig,
    *,
    service: Any,
) -> ProfileProbeResult:
    checks: list[str] = []
    errors: list[str] = []
    security_context = _security_context()
    index_id = f"idx_live_native_{int(time.time())}"
    try:
        created = service.create_vector_index(
            index_id=index_id,
            dimension=3,
            security_context=security_context,
        )
        index_doc = created.get("index") if isinstance(created, dict) else None
        if not isinstance(index_doc, dict) or index_doc.get("index_id") != index_id:
            raise RuntimeError(f"unexpected create_vector_index status={created}")
        checks.append("retrieval_create_index")

        added = service.add_embeddings(
            index_id=index_id,
            dimension=3,
            records=[
                {
                    "vector_id": "doc-1#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {"document_id": "doc-1", "text": "live native probe"},
                }
            ],
            security_context=security_context,
        )
        if int(added.get("accepted", 0)) != 1:
            raise RuntimeError(f"unexpected add_embeddings result={added}")
        checks.append("retrieval_add_embeddings")

        searched = service.vector_search(
            index_id=index_id,
            query_embedding=[0.1, 0.2, 0.3],
            top_k=1,
            security_context=security_context,
        )
        if not searched.get("results"):
            raise RuntimeError(f"vector_search returned no results: {searched}")
        checks.append("retrieval_vector_search")

        described = service.describe_vector_index(
            index_id=index_id,
            security_context=security_context,
        )
        described_index = described.get("index") if isinstance(described, dict) else None
        if not isinstance(described_index, dict) or described_index.get("index_id") != index_id:
            raise RuntimeError(f"describe_vector_index mismatch: {described}")
        checks.append("retrieval_describe_index")

        hybrid = service.hybrid_search(
            dialect=config.dialect,
            query_text="live native probe",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id=index_id,
            top_k=1,
            security_context=security_context,
            sql_filter={"metadata": {"document_id": "doc-1"}},
            weights={"vector": 0.7, "lexical": 0.2, "structured": 0.1},
        )
        if not hybrid.get("results"):
            raise RuntimeError(f"hybrid_search returned no results: {hybrid}")
        checks.append("retrieval_hybrid_search")

        managed_index_id = f"{index_id}_managed"
        managed_created = service.create_vector_index(
            index_id=managed_index_id,
            dimension=3,
            security_context=security_context,
            profile_id="engine_managed_retrieval_v0",
        )
        managed_doc = managed_created.get("index") if isinstance(managed_created, dict) else None
        if not isinstance(managed_doc, dict) or managed_doc.get("index_id") != managed_index_id:
            raise RuntimeError(f"managed create_vector_index failed: {managed_created}")
        checks.append("managed_retrieval_contract_create")

        managed_added = service.add_embeddings(
            index_id=managed_index_id,
            dimension=3,
            records=[
                {
                    "vector_id": "doc-managed#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {
                        "document_id": "doc-managed",
                        "status": "OVERDUE",
                        "text": "north overdue invoice",
                    },
                },
                {
                    "vector_id": "doc-managed#2",
                    "embedding": [0.05, 0.1, 0.15],
                    "metadata": {
                        "document_id": "doc-managed-2",
                        "status": "PAID",
                        "text": "south paid invoice",
                    },
                },
            ],
            security_context=security_context,
        )
        if int(managed_added.get("accepted", 0) or 0) != 2:
            raise RuntimeError(f"managed add_embeddings failed: {managed_added}")
        checks.append("managed_retrieval_contract_add")

        managed_hybrid = service.hybrid_search(
            dialect=config.dialect,
            query_text="overdue invoice north",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id=managed_index_id,
            top_k=1,
            security_context=security_context,
            sql_filter={"where": "status = 'OVERDUE'"},
            weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
        )
        if not managed_hybrid.get("results"):
            raise RuntimeError(f"managed hybrid_search returned no results: {managed_hybrid}")
        checks.append("managed_retrieval_contract_where_pushdown")
    except Exception as exc:
        errors.append(str(exc))
    return ProfileProbeResult(
        profile_id="retrieval_ingest_v0",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _probe_mcp_local(config: LiveNativeConfig) -> ProfileProbeResult:
    checks: list[str] = []
    errors: list[str] = []
    try:
        import anyio
        from mcp import ClientSession
        from mcp.client.stdio import StdioServerParameters, stdio_client
    except ImportError as exc:
        return ProfileProbeResult(
            profile_id="mcp_local_v0",
            passed=False,
            checks=[],
            errors=[f"MCP runtime not installed: {exc}"],
        )

    async def exercise() -> None:
        env = os.environ.copy()
        existing = env.get("PYTHONPATH", "")
        env["PYTHONPATH"] = str(SRC_DIR) if not existing else str(SRC_DIR) + os.pathsep + existing
        env["SCRATCHBIRD_AI_ADAPTER_MODE"] = config.adapter_mode
        env["SCRATCHBIRD_AI_HTTP_BASE_URL"] = config.base_url
        if config.token:
            env["SCRATCHBIRD_AI_HTTP_API_TOKEN"] = config.token
        server = StdioServerParameters(
            command=sys.executable,
            args=["-m", "scratchbird_ai.mcp_server"],
            env=env,
            cwd=str(config.repo_root),
        )
        async with stdio_client(server) as (read_stream, write_stream):
            async with ClientSession(read_stream, write_stream) as session:
                initialized = await session.initialize()
                if initialized.serverInfo.name != "scratchbird-ai":
                    raise RuntimeError(f"unexpected server name {initialized.serverInfo.name}")
                checks.append("mcp_local_initialize")

                tools = await session.list_tools()
                tool_names = {tool.name for tool in tools.tools}
                if "execute_readonly_query" not in tool_names:
                    raise RuntimeError("execute_readonly_query missing from local MCP tools")
                checks.append("mcp_local_tool_catalog")

                response = await session.call_tool(
                    "execute_readonly_query",
                    {
                        "dialect": config.dialect,
                        "query_text": config.query_text,
                        "security_context": _security_context(),
                        "options": {"max_rows": 1},
                    },
                )
                if response.isError:
                    raise RuntimeError(f"local MCP query failed: {response.content[0].text}")
                payload = json.loads(response.content[0].text)
                if payload.get("error_code"):
                    raise RuntimeError(f"local MCP query returned non-success payload: {payload}")
                checks.append("mcp_local_execute_readonly_query")

    try:
        anyio.run(exercise)
    except Exception as exc:
        errors.extend(_exception_messages(exc))

    return ProfileProbeResult(
        profile_id="mcp_local_v0",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _write_junit(
    *,
    path: Path,
    config_errors: list[str],
    native_preflight: NativePreflightResult | None,
    smoke_result: SmokeCommandResult | None,
    profile_results: list[ProfileProbeResult],
) -> None:
    testsuite = ET.Element(
        "testsuite",
        {
            "name": "live_native_conformance",
            "tests": str(3 + len(profile_results)),
            "failures": str(
                int(bool(config_errors))
                + int(bool(native_preflight and not native_preflight.passed))
                + int(bool(smoke_result and not smoke_result.passed))
                + sum(1 for result in profile_results if not result.passed)
            ),
            "errors": "0",
            "skipped": str(int(native_preflight is None) + int(smoke_result is None)),
            "time": f"{(smoke_result.duration_sec if smoke_result is not None else 0.0):.6f}",
        },
    )

    config_case = ET.SubElement(
        testsuite,
        "testcase",
        {"classname": "live_native", "name": "configuration_validation", "time": "0.000000"},
    )
    if config_errors:
        failure = ET.SubElement(config_case, "failure", {"message": config_errors[0]})
        failure.text = "\n".join(config_errors)

    native_case = ET.SubElement(
        testsuite,
        "testcase",
        {
            "classname": "live_native",
            "name": "native_direct_preflight",
            "time": f"{(native_preflight.duration_sec if native_preflight is not None else 0.0):.6f}",
        },
    )
    if native_preflight is None:
        skipped = ET.SubElement(native_case, "skipped", {"message": "native preflight not requested"})
        skipped.text = "native preflight not requested"
    elif not native_preflight.passed:
        message = native_preflight.error or "native preflight failed"
        failure = ET.SubElement(native_case, "failure", {"message": message})
        failure.text = message

    smoke_case = ET.SubElement(
        testsuite,
        "testcase",
        {
            "classname": "live_native",
            "name": "http_bridge_smoke",
            "time": f"{(smoke_result.duration_sec if smoke_result is not None else 0.0):.6f}",
        },
    )
    if smoke_result is None:
        skipped = ET.SubElement(smoke_case, "skipped", {"message": "configuration validation failed"})
        skipped.text = "configuration validation failed"
    elif not smoke_result.passed:
        message = smoke_result.stderr.strip() or smoke_result.stdout.strip() or "smoke failed"
        failure = ET.SubElement(smoke_case, "failure", {"message": message.splitlines()[0]})
        failure.text = message

    for result in profile_results:
        case = ET.SubElement(
            testsuite,
            "testcase",
            {
                "classname": "live_native",
                "name": f"profile_{result.profile_id}",
                "time": "0.000000",
            },
        )
        if not result.passed:
            message = result.errors[0] if result.errors else f"{result.profile_id} failed"
            failure = ET.SubElement(case, "failure", {"message": message})
            failure.text = "\n".join(result.errors or [message])

    root = ET.Element("testsuites")
    root.append(testsuite)
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def _safe_git_commit(repo_root: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repo_root,
            check=True,
            capture_output=True,
            text=True,
        )
    except Exception:
        return "unknown"
    return result.stdout.strip() or "unknown"


def run_live_native_conformance(
    config: LiveNativeConfig,
    *,
    smoke_runner: SmokeRunner = run_smoke_command,
    native_probe_runner: NativeProbeRunner = run_native_preflight,
    bridge_launcher: BridgeLauncher = launch_local_bridge,
    service_internal_probe_runner: ServiceInternalProbeRunner = _probe_service_internal,
    retrieval_probe_runner: RetrievalProbeRunner = _probe_retrieval_ingest,
) -> int:
    config.output_dir.mkdir(parents=True, exist_ok=True)
    runtime_settings = runtime_settings_from_config(config)
    config_errors = validate_live_native_config(config, runtime_settings=runtime_settings)
    effective_config = config
    native_preflight: NativePreflightResult | None = None
    bridge_launch: BridgeLaunchSession | None = None
    smoke_result: SmokeCommandResult | None = None
    profile_results: list[ProfileProbeResult] = []
    service = None
    environment_manifest: dict[str, Any] = {}
    orchestration_error: str | None = None
    try:
        with contextlib.ExitStack() as stack:
            if not config_errors and config.launch_bridge:
                native_preflight = native_probe_runner(config)
                if native_preflight.passed:
                    bridge_launch = stack.enter_context(bridge_launcher(config))
                    effective_config = replace(
                        config,
                        base_url=bridge_launch.base_url,
                        token=bridge_launch.token or config.token,
                    )
                else:
                    bridge_launch = None

            effective_runtime_settings = runtime_settings_from_config(effective_config)
            service = build_default_service(settings=effective_runtime_settings)
            environment_manifest = service.export_certification_manifest()
            live_run_metadata = {
                "certification_level": "live_native",
                "interface_profile_id": effective_config.interface_profile_id,
                "covered_interface_profiles": list(effective_config.covered_interface_profiles),
                "scratchbird_server_version": effective_config.scratchbird_server_version or "unknown",
                "parser_compiler_version": effective_config.parser_compiler_version or "unknown",
                "driver_runtime_version": effective_config.driver_runtime_version,
                "scratchbird_ai_version": environment_manifest.get("release_version", "unknown"),
                "transport_profile": effective_config.transport_profile,
                "auth_mode": effective_config.auth_mode,
                "runtime_mode_id": effective_config.runtime_mode_id,
                "test_dataset_version": effective_config.test_dataset_version or "unknown",
                "seed_or_fixture_version": effective_config.seed_or_fixture_version or "unknown",
                "target": {
                    "base_url": effective_config.base_url,
                    "dialect": effective_config.dialect,
                    "skip_metadata": effective_config.skip_metadata,
                    "launch_bridge": effective_config.launch_bridge,
                    "runtime_env_path": effective_config.runtime_env_path,
                    "runtime_mode_id": effective_config.runtime_mode_id,
                    "bridge_server_setup": _runtime_mode_bridge_setup(
                        effective_config.runtime_mode_id
                    ),
                },
            }
            environment_manifest = dict(environment_manifest)
            environment_manifest["live_run_metadata"] = live_run_metadata
            environment_manifest["git_commit"] = _safe_git_commit(effective_config.repo_root)

            failed_checks = list(config_errors)
            if native_preflight is not None and not native_preflight.passed:
                failed_checks.append(native_preflight.error or "native preflight failed")

            if not failed_checks:
                smoke_result = smoke_runner(effective_config)

            if not failed_checks:
                for profile_id in effective_config.covered_interface_profiles:
                    if profile_id == "service_internal_v0":
                        if smoke_result is None:
                            profile_results.append(
                                ProfileProbeResult(
                                    profile_id=profile_id,
                                    passed=False,
                                    checks=[],
                                    errors=["service_internal_v0 requires the baseline smoke runner"],
                                )
                            )
                        elif smoke_result.passed:
                            probe_result = service_internal_probe_runner(
                                effective_config,
                                service=service,
                            )
                            profile_results.append(
                                ProfileProbeResult(
                                    profile_id=profile_id,
                                    passed=probe_result.passed,
                                    checks=["http_bridge_smoke"] + probe_result.checks,
                                    errors=probe_result.errors,
                                )
                            )
                        else:
                            message = (
                                smoke_result.stderr.strip()
                                or smoke_result.stdout.strip()
                                or "smoke failed"
                            )
                            profile_results.append(
                                ProfileProbeResult(
                                    profile_id=profile_id,
                                    passed=False,
                                    checks=[],
                                    errors=[message],
                                )
                            )
                    elif profile_id == "mcp_local_v0":
                        profile_results.append(_probe_mcp_local(effective_config))
                    elif profile_id == "mcp_remote_v0":
                        profile_results.append(_probe_remote_request_response(effective_config))
                    elif profile_id == "streaming_async_v0":
                        profile_results.append(_probe_remote_streaming(effective_config))
                    elif profile_id == "retrieval_ingest_v0":
                        profile_results.append(
                            retrieval_probe_runner(effective_config, service=service)
                        )
    except Exception as exc:  # noqa: BLE001
        orchestration_error = str(exc)
        if not environment_manifest:
            fallback_settings = runtime_settings_from_config(effective_config)
            fallback_service = build_default_service(settings=fallback_settings)
            environment_manifest = fallback_service.export_certification_manifest()
            environment_manifest = dict(environment_manifest)
            environment_manifest["live_run_metadata"] = {
                "certification_level": "live_native",
                "interface_profile_id": effective_config.interface_profile_id,
                "covered_interface_profiles": list(effective_config.covered_interface_profiles),
                "scratchbird_server_version": effective_config.scratchbird_server_version or "unknown",
                "parser_compiler_version": effective_config.parser_compiler_version or "unknown",
                "driver_runtime_version": effective_config.driver_runtime_version,
                "scratchbird_ai_version": environment_manifest.get("release_version", "unknown"),
                "transport_profile": effective_config.transport_profile,
                "auth_mode": effective_config.auth_mode,
                "runtime_mode_id": effective_config.runtime_mode_id,
                "test_dataset_version": effective_config.test_dataset_version or "unknown",
                "seed_or_fixture_version": effective_config.seed_or_fixture_version or "unknown",
                "target": {
                    "base_url": effective_config.base_url,
                    "dialect": effective_config.dialect,
                    "skip_metadata": effective_config.skip_metadata,
                    "launch_bridge": effective_config.launch_bridge,
                    "runtime_env_path": effective_config.runtime_env_path,
                    "runtime_mode_id": effective_config.runtime_mode_id,
                    "bridge_server_setup": _runtime_mode_bridge_setup(
                        effective_config.runtime_mode_id
                    ),
                },
            }
            environment_manifest["git_commit"] = _safe_git_commit(effective_config.repo_root)

    failed_checks = list(config_errors)
    if native_preflight is not None and not native_preflight.passed:
        failed_checks.append(native_preflight.error or "native preflight failed")
    if orchestration_error:
        failed_checks.append(orchestration_error)
    if smoke_result is not None and not smoke_result.passed:
        failed_checks.append(smoke_result.stderr.strip() or smoke_result.stdout.strip() or "smoke failed")
    for result in profile_results:
        failed_checks.extend(result.errors)

    summary = {
        "generated_at_utc": _utc_now(),
        "git_commit": environment_manifest["git_commit"],
        "status": "PASS"
        if not failed_checks and profile_results and all(result.passed for result in profile_results)
        else "FAIL",
        "check_count": 2 + len(profile_results) + int(native_preflight is not None),
        "passed_checks": (
            int(not config_errors)
            + int(native_preflight is not None and native_preflight.passed)
            + int(bool(smoke_result and smoke_result.passed))
            + sum(1 for result in profile_results if result.passed)
        ),
        "failed_checks": failed_checks,
        "interface_profile_id": effective_config.interface_profile_id,
        "covered_interface_profiles": list(effective_config.covered_interface_profiles),
        "certification_level": "live_native",
        "runtime_mode_id": effective_config.runtime_mode_id,
        "base_url": effective_config.base_url,
        "transport_profile": effective_config.transport_profile,
        "config_errors": config_errors,
        "native_preflight_passed": bool(native_preflight and native_preflight.passed),
        "smoke_passed": bool(smoke_result and smoke_result.passed),
        "profile_results": [result.to_dict() for result in profile_results],
        "artifacts": {
            "environment_manifest": "environment_manifest.json",
            "junit_report": "test_report.junit.xml",
            "run_log": "run_log.json",
            "runtime_mode_report": "runtime_mode_report.json",
        },
    }

    run_log = {
        "generated_at_utc": summary["generated_at_utc"],
        "git_commit": summary["git_commit"],
        "live_run_metadata": environment_manifest["live_run_metadata"],
        "config_errors": config_errors,
        "orchestration_error": orchestration_error,
        "native_preflight": native_preflight.to_dict() if native_preflight is not None else None,
        "bridge_launch": bridge_launch.to_dict() if bridge_launch is not None else None,
        "smoke": None,
        "profile_results": [result.to_dict() for result in profile_results],
    }
    if smoke_result is not None:
        run_log["smoke"] = {
            "command": smoke_result.command,
            "returncode": smoke_result.returncode,
            "duration_sec": round(smoke_result.duration_sec, 6),
            "stdout": smoke_result.stdout,
            "stderr": smoke_result.stderr,
        }

    environment_artifact = {
        "generated_at_utc": summary["generated_at_utc"],
        "git_commit": summary["git_commit"],
        "status": summary["status"],
        "check_count": summary["check_count"],
        "passed_checks": summary["passed_checks"],
        "failed_checks": list(summary["failed_checks"]),
        "certification_manifest": environment_manifest,
    }
    runtime_mode_report = _runtime_mode_report(effective_config)

    _write_json(config.output_dir / "summary.json", summary)
    _write_json(config.output_dir / "environment_manifest.json", environment_artifact)
    _write_json(config.output_dir / "run_log.json", run_log)
    _write_json(config.output_dir / "runtime_mode_report.json", runtime_mode_report)
    _write_junit(
        path=config.output_dir / "test_report.junit.xml",
        config_errors=config_errors,
        native_preflight=native_preflight,
        smoke_result=smoke_result,
        profile_results=profile_results,
    )

    return 0 if summary["status"] == "PASS" else 1


def main() -> int:
    args = _build_parser().parse_args()
    config = config_from_args(args)
    return run_live_native_conformance(config)


if __name__ == "__main__":
    raise SystemExit(main())
