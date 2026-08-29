#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Operational standalone parser-family runtime isolation gate.

The initial operational adapter is intentionally bounded to Firebird.  It
copies one already-qualified empty-prefix parser package into a fresh runtime
root, launches a ScratchBird server and listener with the exact copied worker,
and drives a real Firebird ``isql`` attach/query through that listener.

The listener, parser worker, server, and client are traced independently with
``strace -ff``.  Live ``/proc`` maps and file-descriptor snapshots supplement
the syscall evidence so PARSER-ISO-007 and PARSER-ISO-008 are not inferred from
package identity alone.

Search key: PARSER-STANDALONE-RUNTIME-TRACE-GATE
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
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any


HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import parser_family_binary_isolation_gate as identity  # noqa: E402
import parser_family_package_isolation_gate as package_gate  # noqa: E402


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
PRINCIPAL_UUID = "019f0a11-ce00-7000-8000-000000000001"
TRACE_EXPRESSION = "process,file,network,ipc,memory"
EXECVE_RE = re.compile(r'execve\("(?P<path>[^"\\]*(?:\\.[^"\\]*)*)".*\)\s+=\s+0')
FILE_SYSCALL_RE = re.compile(
    r"\b(?:open|openat|openat2|stat|lstat|newfstatat|access|faccessat|readlink|"
    r"readlinkat|getdents|getdents64|unlink|unlinkat|rename|renameat|mkdir|"
    r"mkdirat|chdir|fchdir)\("
)
SOCKET_SYSCALL_RE = re.compile(
    r"\b(?:socket|socketpair|bind|listen|accept|accept4|connect|sendto|recvfrom|"
    r"sendmsg|recvmsg|getsockname|getpeername|shutdown)\("
)
SHARED_MEMORY_RE = re.compile(
    r"(?:\b(?:shmget|shmat|shmdt|shmctl|memfd_create)\(|/dev/shm/)"
)
LIBRARY_RE = re.compile(r"(?:^|[ /])[^\s\"]+\.(?:so(?:\.[0-9]+)*|dylib)(?:$|[\s\"])")


class GateError(RuntimeError):
    pass


@dataclass
class TracedProcess:
    role: str
    command: list[str]
    process: subprocess.Popen[bytes]
    stdout_path: pathlib.Path
    stderr_path: pathlib.Path
    trace_prefix: pathlib.Path


def utc_timestamp() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def sha256_text(text: str) -> str:
    return "sha256:" + hashlib.sha256(
        text.encode("utf-8", errors="replace")
    ).hexdigest()


def read_text(path: pathlib.Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def read_tail(path: pathlib.Path, limit: int = 12000) -> str:
    return read_text(path)[-limit:]


def executable_identity(path: pathlib.Path, role: str) -> dict[str, object]:
    resolved = path.resolve()
    is_file = resolved.is_file()
    return {
        "role": role,
        "resolved_path": str(resolved),
        "is_file": is_file,
        "executable": is_file and os.access(resolved, os.X_OK),
        "size_bytes": resolved.stat().st_size if is_file else 0,
        "sha256": sha256_file(resolved) if is_file else "",
    }


def relative_file_hashes(root: pathlib.Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): sha256_file(path)
        for path in sorted(root.rglob("*"))
        if path.is_file() and not path.is_symlink()
    }


def path_is_within(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def parser_artifact_family(name: str) -> str | None:
    if name in {"SBParser", "SBParser.exe"}:
        return "sbsql"
    match = re.fullmatch(r"sbp_([A-Za-z0-9_]+)(?:\.exe)?", name)
    return match.group(1) if match else None


def runtime_prefix_gaps(prefix: pathlib.Path, family: str) -> list[str]:
    gaps: list[str] = []
    if not prefix.is_dir():
        return ["runtime_prefix=directory"]
    symlinks = sorted(
        path.relative_to(prefix).as_posix()
        for path in prefix.rglob("*")
        if path.is_symlink()
    )
    if symlinks:
        gaps.append("runtime_prefix_symlinks=" + ",".join(symlinks))
    parser_executables: list[tuple[str, str]] = []
    bin_dir = prefix / "bin"
    if bin_dir.is_dir():
        for path in sorted(bin_dir.iterdir()):
            owner = parser_artifact_family(path.name)
            if owner is not None:
                parser_executables.append((path.name, owner))
    foreign = [name for name, owner in parser_executables if owner != family]
    owned = [name for name, owner in parser_executables if owner == family]
    if foreign:
        gaps.append("foreign_parser_executables=" + ",".join(foreign))
    if owned != [f"sbp_{family}"] and owned != [f"sbp_{family}.exe"]:
        gaps.append("selected_parser_executable=exactly_one")
    return gaps


def adjacent_parser_gaps(paths: list[pathlib.Path], family: str) -> list[str]:
    """Reject fallback-discoverable sibling workers next to server/listener."""
    gaps: list[str] = []
    checked: set[pathlib.Path] = set()
    for executable in paths:
        directory = executable.resolve().parent
        if directory in checked or not directory.is_dir():
            continue
        checked.add(directory)
        for candidate in sorted(directory.iterdir()):
            owner = parser_artifact_family(candidate.name)
            if owner is not None and owner != family:
                gaps.append(
                    "foreign_adjacent_parser_executable:"
                    + candidate.resolve().as_posix()
                )
    return gaps


def search_path_parser_gaps(path_value: str, family: str) -> list[str]:
    gaps: list[str] = []
    checked: set[pathlib.Path] = set()
    for raw in path_value.split(os.pathsep):
        if not raw:
            continue
        directory = pathlib.Path(raw).resolve()
        if directory in checked or not directory.is_dir():
            continue
        checked.add(directory)
        try:
            entries = sorted(directory.iterdir())
        except OSError:
            continue
        for candidate in entries:
            owner = parser_artifact_family(candidate.name)
            if owner is not None and owner != family:
                gaps.append(
                    "foreign_search_path_parser_executable:"
                    + candidate.resolve().as_posix()
                )
    return gaps


def decode_strace_string(value: str) -> str:
    try:
        return bytes(value, "utf-8").decode("unicode_escape")
    except UnicodeDecodeError:
        return value


def successful_execve_paths(trace_text: str) -> list[str]:
    return [decode_strace_string(match.group("path")) for match in EXECVE_RE.finditer(trace_text)]


def unexpected_parser_execs(exec_paths: list[str], family: str) -> list[str]:
    rows: list[str] = []
    for raw in exec_paths:
        owner = parser_artifact_family(pathlib.Path(raw).name)
        if owner is not None and owner != family:
            rows.append(raw)
    return sorted(set(rows))


def trace_files(prefix: pathlib.Path) -> list[pathlib.Path]:
    return sorted(
        path
        for path in prefix.parent.glob(prefix.name + "*")
        if path.is_file()
    )


def trace_bundle(prefix: pathlib.Path) -> tuple[str, list[dict[str, object]]]:
    files = trace_files(prefix)
    parts: list[str] = []
    reports: list[dict[str, object]] = []
    for path in files:
        text = read_text(path)
        parts.append(f"# {path.name}\n{text}")
        reports.append(
            {
                "path": str(path),
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return "\n".join(parts), reports


def process_children(pid: int) -> set[int]:
    children: set[int] = set()
    task_root = pathlib.Path(f"/proc/{pid}/task")
    if not task_root.is_dir():
        return children
    for task in task_root.iterdir():
        path = task / "children"
        try:
            text = path.read_text(encoding="ascii", errors="replace")
        except OSError:
            continue
        for token in text.split():
            if token.isdigit():
                children.add(int(token))
    return children


def descendant_pids(roots: list[int]) -> list[int]:
    discovered: set[int] = set()
    pending = list(roots)
    while pending:
        pid = pending.pop()
        if pid in discovered or not pathlib.Path(f"/proc/{pid}").is_dir():
            continue
        discovered.add(pid)
        pending.extend(process_children(pid) - discovered)
    return sorted(discovered)


def proc_text(pid: int, name: str, *, binary: bool = False) -> str:
    path = pathlib.Path(f"/proc/{pid}/{name}")
    try:
        if binary:
            return path.read_bytes().replace(b"\0", b" ").decode(
                "utf-8", errors="replace"
            ).strip()
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def proc_symlink(path: pathlib.Path) -> str:
    try:
        return os.readlink(path)
    except OSError:
        return ""


def capture_proc_snapshot(
    roots: list[int], output_dir: pathlib.Path, label: str
) -> tuple[list[dict[str, object]], str]:
    output_dir.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, object]] = []
    aggregate: list[str] = []
    for pid in descendant_pids(roots):
        base = pathlib.Path(f"/proc/{pid}")
        exe = proc_symlink(base / "exe")
        cmdline = proc_text(pid, "cmdline", binary=True)
        maps = proc_text(pid, "maps")
        status = proc_text(pid, "status")
        fds: list[str] = []
        fd_dir = base / "fd"
        if fd_dir.is_dir():
            try:
                fd_paths = sorted(fd_dir.iterdir(), key=lambda item: item.name)
            except OSError:
                fd_paths = []
            for fd in fd_paths:
                target = proc_symlink(fd)
                if target:
                    fds.append(f"{fd.name}\t{target}")
        text = (
            f"pid={pid}\nexe={exe}\ncmdline={cmdline}\n"
            f"--- status ---\n{status}--- maps ---\n{maps}"
            f"--- fds ---\n" + "\n".join(fds) + "\n"
        )
        path = output_dir / f"{label}.{pid}.txt"
        path.write_text(text, encoding="utf-8")
        aggregate.append(f"# {path.name}\n{text}")
        rows.append(
            {
                "pid": pid,
                "exe": exe,
                "cmdline": cmdline,
                "path": str(path),
                "sha256": sha256_file(path),
                "map_line_count": len(maps.splitlines()),
                "fd_count": len(fds),
            }
        )
    return rows, "\n".join(aggregate)


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(
    path: pathlib.Path, process: subprocess.Popen[bytes], timeout: float
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise GateError(
                f"process exited rc={process.returncode} while waiting for {path}"
            )
        time.sleep(0.05)
    raise GateError(f"timed out waiting for {path}")


def wait_for_tcp(
    port: int, process: subprocess.Popen[bytes], timeout: float
) -> None:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise GateError(
                f"listener exited rc={process.returncode} while waiting for port {port}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise GateError(f"timed out waiting for listener port {port}: {last_error}")


def start_traced(
    role: str,
    command: list[str],
    work: pathlib.Path,
    env: dict[str, str],
    strace: pathlib.Path,
) -> TracedProcess:
    role_dir = work / role
    role_dir.mkdir(parents=True, exist_ok=True)
    stdout = role_dir / f"{role}.stdout.txt"
    stderr = role_dir / f"{role}.stderr.txt"
    trace_prefix = role_dir / f"{role}.strace"
    traced_command = [
        str(strace),
        "-ff",
        "-qq",
        "-s",
        "4096",
        "-o",
        str(trace_prefix),
        "-e",
        f"trace={TRACE_EXPRESSION}",
        "--",
        *command,
    ]
    with stdout.open("wb") as stdout_handle, stderr.open("wb") as stderr_handle:
        proc = subprocess.Popen(
            traced_command,
            cwd=work,
            env=env,
            stdout=stdout_handle,
            stderr=stderr_handle,
            start_new_session=True,
        )
    return TracedProcess(role, command, proc, stdout, stderr, trace_prefix)


def stop_traced(traced: TracedProcess | None) -> dict[str, object]:
    if traced is None:
        return {"controlled_shutdown": False, "returncode": None}
    proc = traced.process
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)
    return {"controlled_shutdown": True, "returncode": proc.returncode}


def hex_text(value: str) -> str:
    return value.encode("utf-8").hex()


def local_password_fingerprint(verifier: str) -> str:
    digest = hashlib.sha256(verifier.encode("utf-8")).hexdigest()
    return f"local-password-verifier:v1:sha256:{digest}"


def grant_uuid(principal_uuid: str, right: str) -> str:
    digest = hashlib.sha256(f"{principal_uuid}:{right}".encode("utf-8")).hexdigest()
    return f"019f0a11-ce00-7000-8000-{digest[:12]}"


def authorization_context_successor(authority_event: str, generation: int) -> str:
    fields = authority_event.split("\t")
    if len(fields) < 3 or fields[0] != "SBSECPL1" or generation <= 0:
        raise GateError("security authority successor input is invalid")
    evidence = hashlib.sha256((authority_event + "\n").encode("utf-8")).hexdigest()
    return "\t".join(
        [
            "SBSECPL1",
            "AUTH_CONTEXT_SUCCESSOR",
            fields[2],
            str(generation),
            f"security-context-successor:v1:sha256:{evidence}",
        ]
    )


def write_auth_file(database: pathlib.Path) -> pathlib.Path:
    auth = pathlib.Path(str(database) + ".sb.local_password_auth")
    auth.write_text(f"alice\tlocal_password\t{VERIFIER}\n", encoding="utf-8")
    event = "\t".join(
        [
            "SBSECPL1",
            "PRINCIPAL",
            "0",
            PRINCIPAL_UUID,
            hex_text("alice"),
            "user",
            "active",
            hex_text(local_password_fingerprint(VERIFIER)),
            "1",
            "0",
        ]
    )
    grants: list[str] = []
    for generation, right in enumerate(("CONNECT", "SEC_IDENTITY_ADMIN"), start=2):
        grants.append(
            "\t".join(
                [
                    "SBSECPL1",
                    "GRANT",
                    "0",
                    grant_uuid(PRINCIPAL_UUID, right),
                    PRINCIPAL_UUID,
                    "principal",
                    "",
                    "",
                    right,
                    PRINCIPAL_UUID,
                    "allow",
                    str(generation),
                    "0",
                ]
            )
        )
    authority_events = [event, *grants]
    committed_events: list[str] = []
    for generation, authority_event in enumerate(authority_events, start=1):
        committed_events.extend(
            [authority_event, authorization_context_successor(authority_event, generation)]
        )
    pathlib.Path(str(database) + ".sb.security_principal_events").write_text(
        "\n".join(committed_events) + "\n", encoding="utf-8"
    )
    return auth


def load_registry(path: pathlib.Path) -> identity.Registry:
    payload = identity.strict_json_loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise GateError("ownership map root must be an object")
    return identity.Registry(payload)


def restricted_runtime_env(prefix: pathlib.Path, temporary: pathlib.Path) -> dict[str, str]:
    temporary.mkdir(parents=True, exist_ok=True)
    env = {
        "HOME": str(prefix),
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": os.pathsep.join((str(prefix / "bin"), "/usr/bin", "/bin")),
        "LD_LIBRARY_PATH": str(prefix / "lib"),
        "TMPDIR": str(temporary),
        "SB_COMPATIBILITY_FIREBIRD_PASSWORD": "local_password",
        "SB_REFERENCE_FIREBIRD_PASSWORD": "local_password",
        "SB_COMPATIBILITY_FIREBIRD_VERIFIER": VERIFIER,
        "SB_REFERENCE_FIREBIRD_VERIFIER": VERIFIER,
        "SB_COMPATIBILITY_FIREBIRD_PRINCIPAL_UUID": PRINCIPAL_UUID,
        "SB_REFERENCE_FIREBIRD_PRINCIPAL_UUID": PRINCIPAL_UUID,
    }
    return env


def package_identity(
    prefix: pathlib.Path,
    family: str,
    registry: identity.Registry,
    env: dict[str, str],
) -> tuple[pathlib.Path, dict[str, object], list[str], list[identity.Finding], str]:
    worker = package_gate.installed_worker(prefix, family)
    proc = subprocess.run(
        [str(worker), "--package-identity"],
        cwd=prefix,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=60,
        check=False,
    )
    if proc.returncode != 0:
        raise GateError(f"package identity failed rc={proc.returncode}: {proc.stdout[-1000:]}")
    manifest, manifest_gaps = identity.parse_package_identity(family, proc.stdout)
    if manifest is None:
        raise GateError("package identity is not a valid canonical manifest")
    descriptor = package_gate.load_descriptor(prefix, family)
    closure_gaps, _ = package_gate.validate_installed_closure(
        prefix, family, manifest, descriptor
    )
    gaps = sorted(set(manifest_gaps + closure_gaps + runtime_prefix_gaps(prefix, family)))
    listing = "\n".join(relative_file_hashes(prefix))
    findings: list[identity.Finding] = []
    findings.extend(identity.scan_text(registry, family, "runtime_prefix", listing))
    findings.extend(identity.scan_text(registry, family, "runtime_manifest", proc.stdout))
    findings.extend(
        identity.scan_text(
            registry,
            family,
            "runtime_descriptor",
            json.dumps(descriptor, sort_keys=True),
        )
    )
    if manifest.get("foreign_parser_fallback") is not False:
        gaps.append("foreign_parser_fallback=false")
    if manifest.get("standalone_package") is not True:
        gaps.append("standalone_package=true")
    return worker, manifest, sorted(set(gaps)), findings, proc.stdout


def deduplicate_findings(
    findings: list[identity.Finding],
) -> list[identity.Finding]:
    unique = {
        (row.family, row.channel, row.foreign_family, row.evidence): row
        for row in findings
    }
    return sorted(
        unique.values(),
        key=lambda row: (row.channel, row.foreign_family, row.evidence),
    )


def run_gate(args: argparse.Namespace) -> dict[str, object]:
    if args.family != "firebird":
        raise GateError(
            "the operational original-client adapter currently supports only firebird"
        )
    repo_root = args.repo_root.resolve()
    source_prefix = args.source_prefix.resolve()
    server = args.server.resolve()
    listener = args.listener.resolve()
    client = args.client_tool.resolve()
    ownership = args.ownership_map
    if not ownership.is_absolute():
        ownership = repo_root / ownership
    ownership = ownership.resolve()
    evidence_dir = args.evidence_dir.resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)
    work = (
        args.runtime_work_dir.resolve()
        if args.runtime_work_dir is not None
        else evidence_dir / "runtime"
    )
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    isolated_prefix = work / "isolated_prefix"
    shutil.copytree(source_prefix, isolated_prefix, symlinks=True)

    strace_path = pathlib.Path(shutil.which("strace") or "")
    if not strace_path.is_file():
        raise GateError("strace is required for PARSER-ISO-008")
    for role, path in (("server", server), ("listener", listener), ("client", client)):
        if not path.is_file() or not os.access(path, os.X_OK):
            raise GateError(f"{role} executable missing: {path}")

    registry = load_registry(ownership)
    if args.family not in registry.families:
        raise GateError(f"family is not registered: {args.family}")

    source_hashes = relative_file_hashes(source_prefix)
    runtime_hashes = relative_file_hashes(isolated_prefix)
    copy_gaps = [] if source_hashes == runtime_hashes else ["isolated_prefix_copy_hash_mismatch"]
    runtime_env = restricted_runtime_env(isolated_prefix, work / "tmp")
    worker, manifest, package_gaps, findings, manifest_stdout = package_identity(
        isolated_prefix, args.family, registry, runtime_env
    )
    package_gaps.extend(copy_gaps)
    package_gaps.extend(adjacent_parser_gaps([server, listener], args.family))
    package_gaps.extend(search_path_parser_gaps(runtime_env["PATH"], args.family))
    package_gaps = sorted(set(package_gaps))

    database = work / "database" / "runtime.sbdb"
    database.parent.mkdir(parents=True, exist_ok=True)
    auth_file = write_auth_file(database)
    server_endpoint = work / "server" / "control" / "sbps.sock"
    listener_port = args.port if args.port > 0 else find_free_port()
    if max(
        len(str(server_endpoint)),
        len(str(work / "listener" / "control" / "listener.management.sock")),
    ) >= 100:
        raise GateError(f"evidence path is too long for Unix socket safety: {work}")

    server_cmd = [
        str(server),
        "--foreground",
        "--no-listeners",
        "--create-if-missing",
        "--control-dir",
        str(work / "server" / "control"),
        "--runtime-dir",
        str(work / "server" / "runtime"),
        "--database",
        str(database),
        "--sbps-endpoint",
        str(server_endpoint),
        "--log",
        str(work / "server" / "server.jsonl"),
        "--log-level",
        "info",
    ]
    listener_cmd = [
        str(listener),
        "--foreground",
        "--protocol-family=firebird",
        "--listener-profile=firebird_runtime_isolation",
        "--bundle-contract-id=bundle.default@1",
        f"--database-selector=dev_bootstrap_path:{database}",
        f"--server-endpoint=unix:{server_endpoint}",
        f"--parser-executable={worker}",
        f"--control-dir={work / 'listener' / 'control'}",
        f"--runtime-dir={work / 'listener' / 'runtime'}",
        "--bind-address=127.0.0.1",
        f"--port={listener_port}",
        "--warm-pool-min=1",
        "--warm-pool-max=2",
        "--tls-required=false",
    ]

    traced_server: TracedProcess | None = None
    traced_listener: TracedProcess | None = None
    shutdown: dict[str, object] = {}
    snapshot_rows: list[dict[str, object]] = []
    snapshot_text_parts: list[str] = []
    client_trace_prefix = work / "client" / "client.strace"
    client_stdout_path = work / "client" / "client.stdout.txt"
    client_stderr_path = work / "client" / "client.stderr.txt"
    client_output_path = work / "client" / "isql.output.txt"
    client_script_path = work / "client" / "isql.input.sql"
    client_returncode: int | None = None
    client_stdout = ""
    client_stderr = ""
    startup_ready = False
    try:
        traced_server = start_traced(
            "server", server_cmd, work, runtime_env, strace_path
        )
        wait_for_path(server_endpoint, traced_server.process, args.startup_timeout)
        traced_listener = start_traced(
            "listener", listener_cmd, work, runtime_env, strace_path
        )
        wait_for_tcp(listener_port, traced_listener.process, args.startup_timeout)
        startup_ready = True

        for index in range(3):
            rows, text = capture_proc_snapshot(
                [traced_server.process.pid, traced_listener.process.pid],
                work / "proc",
                f"before_client_{index}",
            )
            snapshot_rows.extend(rows)
            snapshot_text_parts.append(text)
            if any(pathlib.Path(str(row["exe"])).resolve() == worker for row in rows if row["exe"]):
                break
            time.sleep(0.1)

        client_script_path.parent.mkdir(parents=True, exist_ok=True)
        client_script_path.write_text(
            "set bail on;\n"
            "select 1 as sb_iso_007_008_probe from rdb$database;\n"
            "quit;\n",
            encoding="utf-8",
        )
        database_dsn = f"127.0.0.1/{listener_port}:default"
        client_cmd = [
            str(client),
            "-q",
            "-user",
            "alice",
            "-password",
            "local_password",
            "-i",
            str(client_script_path),
            "-o",
            str(client_output_path),
            database_dsn,
        ]
        client_env = dict(runtime_env)
        client_lib_dir = args.client_library_dir
        if client_lib_dir is None:
            candidate = client.parent.parent / "lib"
            client_lib_dir = candidate if candidate.is_dir() else None
        if client_lib_dir is not None:
            client_env["LD_LIBRARY_PATH"] = os.pathsep.join(
                (str(client_lib_dir.resolve()), str(isolated_prefix / "lib"))
            )
        traced_client_cmd = [
            str(strace_path),
            "-ff",
            "-qq",
            "-s",
            "4096",
            "-o",
            str(client_trace_prefix),
            "-e",
            f"trace={TRACE_EXPRESSION}",
            "--",
            *client_cmd,
        ]
        with client_stdout_path.open("w", encoding="utf-8") as stdout_handle, client_stderr_path.open(
            "w", encoding="utf-8"
        ) as stderr_handle:
            client_proc = subprocess.run(
                traced_client_cmd,
                cwd=work,
                env=client_env,
                text=True,
                stdout=stdout_handle,
                stderr=stderr_handle,
                timeout=args.tool_timeout,
                check=False,
            )
        client_returncode = client_proc.returncode
        client_stdout = read_text(client_stdout_path)
        client_stderr = read_text(client_stderr_path)

        rows, text = capture_proc_snapshot(
            [traced_server.process.pid, traced_listener.process.pid],
            work / "proc",
            "after_client",
        )
        snapshot_rows.extend(rows)
        snapshot_text_parts.append(text)
    finally:
        shutdown["listener"] = stop_traced(traced_listener)
        shutdown["server"] = stop_traced(traced_server)

    client_output = read_text(client_output_path)
    client_session_passed = (
        client_returncode == 0
        and "SB_ISO_007_008_PROBE" in client_output.upper()
        and re.search(r"(?:^|\s)1(?:\s|$)", client_output) is not None
    )

    trace_sources = {
        "server": traced_server.trace_prefix if traced_server else work / "server/missing",
        "listener": traced_listener.trace_prefix if traced_listener else work / "listener/missing",
        "client": client_trace_prefix,
    }
    trace_texts: dict[str, str] = {}
    trace_reports: dict[str, list[dict[str, object]]] = {}
    for role, prefix in trace_sources.items():
        trace_texts[role], trace_reports[role] = trace_bundle(prefix)
    maps_text = "\n".join(snapshot_text_parts)
    logs_text = "\n".join(
        (
            read_text(traced_server.stdout_path) if traced_server else "",
            read_text(traced_server.stderr_path) if traced_server else "",
            read_text(traced_listener.stdout_path) if traced_listener else "",
            read_text(traced_listener.stderr_path) if traced_listener else "",
            read_text(work / "server" / "server.jsonl"),
            client_stdout,
            client_stderr,
            client_output,
        )
    )
    for channel, text in (
        ("runtime_process_file_socket_trace", "\n".join(trace_texts.values())),
        ("runtime_process_maps", maps_text),
        ("runtime_logs", logs_text),
    ):
        findings.extend(identity.scan_text(registry, args.family, channel, text))
    findings = deduplicate_findings(findings)

    all_trace = "\n".join(trace_texts.values())
    exec_paths = successful_execve_paths(all_trace)
    exact_worker_exec_count = sum(
        pathlib.Path(path).resolve() == worker for path in exec_paths
    )
    foreign_parser_execs = unexpected_parser_execs(exec_paths, args.family)
    allowed_execs = {server, listener, worker, client}
    unexpected_execs = sorted(
        {
            str(pathlib.Path(path).resolve())
            for path in exec_paths
            if pathlib.Path(path).resolve() not in allowed_execs
        }
    )
    worker_map_snapshot_count = sum(
        bool(row["exe"])
        and pathlib.Path(str(row["exe"])).resolve() == worker
        for row in snapshot_rows
    )
    trace_file_count = sum(len(rows) for rows in trace_reports.values())
    channel_evidence = {
        "capture_filter": TRACE_EXPRESSION,
        "process": {
            "capture_enabled": True,
            "successful_execve_count": len(exec_paths),
            "exact_selected_worker_exec_count": exact_worker_exec_count,
        },
        "file": {
            "capture_enabled": True,
            "syscall_count": len(FILE_SYSCALL_RE.findall(all_trace)),
        },
        "library": {
            "capture_enabled": True,
            "map_or_trace_entry_count": len(LIBRARY_RE.findall(maps_text + "\n" + all_trace)),
            "proc_snapshot_count": len(snapshot_rows),
            "selected_worker_map_snapshot_count": worker_map_snapshot_count,
        },
        "shared_memory": {
            "capture_enabled": True,
            "observed_access_count": len(SHARED_MEMORY_RE.findall(all_trace)),
            "zero_is_valid_absence_observation": True,
        },
        "socket": {
            "capture_enabled": True,
            "syscall_count": len(SOCKET_SYSCALL_RE.findall(all_trace)),
        },
    }

    iso007_gaps = list(package_gaps)
    if not startup_ready:
        iso007_gaps.append("listener_route=startup_ready")
    if not client_session_passed:
        iso007_gaps.append("genuine_firebird_isql_attach_query=passed")
    if exact_worker_exec_count < 1:
        iso007_gaps.append("exact_isolated_firebird_worker=executed")
    if findings:
        iso007_gaps.append("foreign_runtime_dependency_count=0")

    iso008_gaps: list[str] = []
    if any(not rows for rows in trace_reports.values()):
        iso008_gaps.append("trace_files=server_listener_client_complete")
    if trace_file_count < 3:
        iso008_gaps.append("trace_file_count>=3")
    if exact_worker_exec_count < 1:
        iso008_gaps.append("process_trace=exact_selected_worker_exec")
    if foreign_parser_execs:
        iso008_gaps.append("foreign_parser_exec_count=0")
    if unexpected_execs:
        iso008_gaps.append("unexpected_exec_count=0")
    if findings:
        iso008_gaps.append("foreign_trace_map_finding_count=0")
    if not snapshot_rows:
        iso008_gaps.append("proc_maps_snapshot=present")
    for channel in ("process", "file", "library", "shared_memory", "socket"):
        if not channel_evidence[channel]["capture_enabled"]:
            iso008_gaps.append(f"{channel}_trace_capture=enabled")
    if channel_evidence["file"]["syscall_count"] == 0:
        iso008_gaps.append("file_trace=syscalls_observed")
    if channel_evidence["library"]["map_or_trace_entry_count"] == 0:
        iso008_gaps.append("library_trace_or_maps=entries_observed")
    if channel_evidence["socket"]["syscall_count"] == 0:
        iso008_gaps.append("socket_trace=syscalls_observed")

    iso007_gaps = sorted(set(iso007_gaps))
    iso008_gaps = sorted(set(iso008_gaps))
    iso007_passed = not iso007_gaps
    iso008_passed = not iso008_gaps
    return {
        "schema_version": "scratchbird_parser_family_runtime_isolation_gate_v1",
        "gate": "parser_family_runtime_isolation_gate",
        "scope": {
            "parser_family": args.family,
            "family_count": 1,
            "families_proven": [args.family] if iso007_passed and iso008_passed else [],
            "all_parser_families_claimed": False,
            "operational_adapter": "genuine_firebird_5_isql_attach_query",
            "conformance_gates": ["PARSER-ISO-007", "PARSER-ISO-008"],
        },
        "scope_limitations": [
            "proves only the Firebird parser family",
            "does not claim PARSER-ISO-009 through PARSER-ISO-015",
            "the query is an operational attach/session isolation probe, not a full regression replay",
            "system runtime libraries and the genuine Firebird client library are neutral or client-side inputs, not ScratchBird parser packages",
        ],
        "timestamp_utc": utc_timestamp(),
        "status": "passed" if iso007_passed and iso008_passed else "failed",
        "conformance": {
            "PARSER-ISO-007": "passed" if iso007_passed else "failed",
            "PARSER-ISO-008": "passed" if iso008_passed else "failed",
        },
        "gate_definitions": {
            "PARSER-ISO-007": (
                "fresh copied empty-prefix package; no sibling worker in prefix or server/listener bin; "
                "manifest forbids fallback; exact prefix worker executes; genuine Firebird isql attach/query succeeds"
            ),
            "PARSER-ISO-008": (
                "strace process/file/network/IPC/memory capture plus live proc maps/fds; exact worker observed; "
                "zero foreign parser process/file/library/shared-memory/socket/fallback findings"
            ),
        },
        "gaps": {
            "PARSER-ISO-007": iso007_gaps,
            "PARSER-ISO-008": iso008_gaps,
        },
        "package": {
            "source_prefix": str(source_prefix),
            "runtime_prefix": str(isolated_prefix),
            "source_file_sha256": source_hashes,
            "runtime_file_sha256": runtime_hashes,
            "copy_hashes_equal": source_hashes == runtime_hashes,
            "selected_worker": str(worker),
            "selected_worker_sha256": sha256_file(worker),
            "manifest_sha256": sha256_text(manifest_stdout),
            "parser_family_uuid": manifest.get("parser_family_uuid"),
            "standalone_package": manifest.get("standalone_package"),
            "cross_parser_dependency_count": manifest.get("cross_parser_dependency_count"),
            "foreign_parser_fallback": manifest.get("foreign_parser_fallback"),
            "package_gaps": package_gaps,
        },
        "runtime": {
            "startup_ready": startup_ready,
            "listener_port": listener_port,
            "database": str(database),
            "auth_file": str(auth_file),
            "restricted_environment": {
                "HOME": runtime_env["HOME"],
                "PATH": runtime_env["PATH"],
                "LD_LIBRARY_PATH": runtime_env["LD_LIBRARY_PATH"],
                "TMPDIR": runtime_env["TMPDIR"],
            },
            "server_command": server_cmd,
            "listener_command": listener_cmd,
            "controlled_shutdown": shutdown,
            "tool_identities": {
                "server": executable_identity(server, "scratchbird_server"),
                "listener": executable_identity(listener, "scratchbird_listener"),
                "parser_worker": executable_identity(worker, "scratchbird_firebird_parser_worker"),
                "client": executable_identity(client, "genuine_firebird_isql"),
                "strace": executable_identity(strace_path, "runtime_trace_tool"),
            },
        },
        "original_client_session": {
            "tool": "isql",
            "genuine_client_binary": True,
            "returncode": client_returncode,
            "passed": client_session_passed,
            "input": str(client_script_path),
            "input_sha256": sha256_file(client_script_path),
            "output": str(client_output_path),
            "output_sha256": sha256_file(client_output_path),
            "output_tail": client_output[-2000:],
            "stdout_sha256": sha256_text(client_stdout),
            "stderr_sha256": sha256_text(client_stderr),
        },
        "trace": {
            "trace_file_count": trace_file_count,
            "trace_reports": trace_reports,
            "channel_evidence": channel_evidence,
            "successful_execve_paths": exec_paths,
            "exact_selected_worker_exec_count": exact_worker_exec_count,
            "foreign_parser_execs": foreign_parser_execs,
            "unexpected_execs": unexpected_execs,
            "proc_snapshots": snapshot_rows,
            "foreign_finding_count": len(findings),
            "findings": [finding.to_json() for finding in findings],
        },
    }


def write_failure_evidence(args: argparse.Namespace, exc: BaseException) -> None:
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": "scratchbird_parser_family_runtime_isolation_gate_v1",
        "gate": "parser_family_runtime_isolation_gate",
        "status": "failed",
        "timestamp_utc": utc_timestamp(),
        "scope": {
            "parser_family": args.family,
            "family_count": 1,
            "all_parser_families_claimed": False,
            "conformance_gates": ["PARSER-ISO-007", "PARSER-ISO-008"],
        },
        "conformance": {
            "PARSER-ISO-007": "failed",
            "PARSER-ISO-008": "failed",
        },
        "errors": [f"{type(exc).__name__}: {exc}"],
    }
    (args.evidence_dir / "parser_family_runtime_isolation_evidence.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--family", required=True)
    parser.add_argument("--source-prefix", required=True, type=pathlib.Path)
    parser.add_argument("--server", required=True, type=pathlib.Path)
    parser.add_argument("--listener", required=True, type=pathlib.Path)
    parser.add_argument("--client-tool", required=True, type=pathlib.Path)
    parser.add_argument("--client-library-dir", type=pathlib.Path)
    parser.add_argument("--ownership-map", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-dir", required=True, type=pathlib.Path)
    parser.add_argument(
        "--runtime-work-dir",
        type=pathlib.Path,
        help="optional short work path for Unix socket safety; evidence JSON remains in --evidence-dir",
    )
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--startup-timeout", type=float, default=60.0)
    parser.add_argument("--tool-timeout", type=int, default=60)
    args = parser.parse_args()
    args.evidence_dir = args.evidence_dir.resolve()
    evidence_file = args.evidence_dir / "parser_family_runtime_isolation_evidence.json"
    try:
        payload = run_gate(args)
        evidence_file.parent.mkdir(parents=True, exist_ok=True)
        evidence_file.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(
            "parser_family_runtime_isolation_gate="
            f"{payload['status']} family={args.family} "
            f"iso007={payload['conformance']['PARSER-ISO-007']} "
            f"iso008={payload['conformance']['PARSER-ISO-008']} "
            f"foreign_findings={payload['trace']['foreign_finding_count']} "
            f"evidence={evidence_file}"
        )
        if payload["status"] != "passed":
            print(json.dumps(payload["gaps"], indent=2, sort_keys=True), file=sys.stderr)
            return 1
        return 0
    except (
        GateError,
        OSError,
        ValueError,
        subprocess.SubprocessError,
        identity.DuplicateJsonKeyError,
        json.JSONDecodeError,
    ) as exc:
        write_failure_evidence(args, exc)
        print(f"parser_family_runtime_isolation_gate: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
