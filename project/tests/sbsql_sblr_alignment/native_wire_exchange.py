#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_WIRE_EXCHANGE (S4h part 1 regression test, 2026-12-08).

The REAL multi-process exchange of the thin slice, as a ctest: SBsql (the client, SBWP) -> SBgate (the listener) ->
SBParser (the SBsql parser worker) -> SBsrv (the server, started with NO --config) -> the engine, on a database made
by the production native create fixture (native_login_material create) at one page size. Plain TCP (--sslmode=disable);
TLS and the SBWP-over-TLS path are NOT exercised here (the listener's tls_required policy is a separate route).

Steps and assertions:
  1. SBsrv serves (lifecycle service_ready=true), then SBgate (TCP port chosen by bind(0)), each with a readiness
     wait, never a sleep. Control directories are short (the listener refuses MANAGEMENT_SOCKET_PATH_TOO_LONG).
  2. SBsql: `SELECT 1 AS one` prints the table with `one | 1`; `-q -A -t` prints `1`; `SELECT 2 AS two`;
     `SELECT 1 AS one, 2 AS two`. ROUTE B (2026-12-11; OLD: bare `SELECT 1` was SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED, route B "WILL flip
     this assertion"): bare `SELECT 1` (also `select 1`, `SELECT 1;`, `SELECT 2`, `VALUES (1)`, the int64 maximum) prints the table whose
     column carries the PARSER's default name `column1` (2026-12-12; OLD: `key_a`, a fixture-list name the parser's own binding gave the one
     column of a literal VALUES relation and that leaked into the user-visible result; NEW: the same rule as the unaliased scalar-projection
     route, column1, column2, ... by position; the engine sends no name) and the value; `-q -A -t` prints `1`. Refused shapes keep their machine tokens:
     `SELECT 1 + 0 AS s` and bare `SELECT 1 + 0`, `SELECT -1`, `VALUES (1), (2)` and CREATE TABLE are
     SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED, `SELECT 'a' AS t` is ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT, `SELECT 1 FROM t` is
     ENGINE.NATIVE.WIRE_NAME_RESOLVE_S5 (the parser's own refusal), `SELECT 1.5` is QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1 (refused by the
     executor after the gate). Inside ONE open line-protocol session every statement, accepted or refused, leaves the data file byte-identical
     (route B check). A WRONG password gives the generic
     SECURITY.AUTHENTICATION.LOGIN_FAILED and the database file is byte-identical before and after it.
     The line client (AUTH / EXECUTE over the same TCP port) gets RESULT query.evaluate_projection with row one = 1.
  3. SIGTERM, listener first then server: SBsrv exits 0; no SBgate / SBParser / SBsrv process of this run is left;
     the DATABASE directory holds ONLY the database file; the control directories hold exactly the expected files
     (a new stray file fails); the changed database pages are a subset of the transaction-state, transaction-header
     and session-ledger chains (native_login `pages`); the TIP and ledger (native_state_dump) show every session of
     the run ENDED: its transaction slot rolled_back, four ledger records per login (connection, session begin, attachment, session end), nothing orphaned.
  4. A restart of SBsrv serves again and leaves the file byte-identical (strace variant: 0 write-class syscalls on
     the data descriptor, 0 `.sb.` syscalls, for the whole restart run).
  strace variant (--strace): SBsrv runs under `strace -f` (strace_attribution.py, the pid-aware helper): 0 syscalls
     name a `.sb.` path; every write-class syscall on the data file is a 512-byte pwrite64, an fsync, or the one whole-page write of a chain acquiring a NEW page (a chain page listed by `pages`, written once); none before
     service_ready (the startup), none after SIGTERM except at most one shutdown fsync; the 512-byte pwrite64 count is AT MOST
     logins x per_login where per_login is DERIVED here, not a constant: the S4e child (native_login serve ... trace,
     run unstraced on a COPY of the freshly created file) reports the writes of two approvals and of ending two
     sessions; per_login = approvals/2 + ends/2 (6 + 3 = 9 at the measured configuration); a negative control (a
     create traced the same way) must show pwrite64 on the data file, or the detector is not trusted.
Keep the work directories for inspection with SB_WIRE_EXCHANGE_KEEP=1. Every child's log is printed on failure.
"""
import argparse
import hashlib
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from strace_attribution import data_descriptors, merge_split_lines  # noqa: E402

# The shared process-readiness helpers of the live route smoke (find_free_port, wait_for_tcp): not copied.
LIVE_ROUTE_DIR = os.path.join(os.path.dirname(HERE), "sbsql_parser_worker")
sys.path.insert(0, LIVE_ROUTE_DIR)
from sbsql_sb_isql_live_route_smoke import find_free_port, wait_for_tcp  # noqa: E402

USER = "fixture_owner"                       # the create fixture's bootstrap principal (native_login_material_test.cpp)
PASSWORD = "S4a-Bootstrap-Pw-7f3a91c2d8"     # its test password (a public test constant, as in that fixture)
WRONG = "S4c-Wrong-Pw-4d9e1a77c0b2"
WRITE_CALLS = r"(pwrite64|pwrite|write|writev|pwritev2?|ftruncate|fallocate|fsync|fdatasync|sync_file_range)"

PROCS = []          # (name, Popen) every child started, for the finally block
FAILURE_LOGS = []   # (title, path) printed on failure


class Fail(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Fail(message)


def sha256(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def read_text(path, tail=3000):
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            return handle.read()[-tail:]
    except OSError:
        return ""


def process_alive_for(work):
    """Pids whose command line names this run's work directory (children of the stack)."""
    found = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit() or int(entry) == os.getpid():
            continue
        try:
            with open(f"/proc/{entry}/cmdline", "rb") as handle:
                text = handle.read().decode("utf-8", errors="replace")
        except OSError:
            continue
        if work in text:
            found.append((int(entry), text.replace("\0", " ")[:160]))
    return found


def wait_ready(path, token, process, what, timeout=180):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise Fail(f"{what} exited {process.returncode} before it was ready")
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                if token in handle.read():
                    return
        except OSError:
            pass
        time.sleep(0.05)
    raise Fail(f"{what}: not ready within {timeout} s ({path} never held {token})")


def server_command(args, work, database, strace_log=None, ctl="sc", run="sr"):
    command = [args.sbsrv, "--foreground", "--no-listeners", "--database", database,
               "--control-dir", os.path.join(work, ctl), "--runtime-dir", os.path.join(work, run),
               "--sbps-endpoint", os.path.join(work, ctl, "sb_server.sbps.sock")]
    if strace_log:
        command = ["strace", "-f", "-qq", "-s", "1024", "-o", strace_log] + command
    return command


def start(name, command, work, env=None):
    out = os.path.join(work, f"{name}.out")
    FAILURE_LOGS.append((name, out))
    sink = open(out, "w", encoding="utf-8")
    process = subprocess.Popen(command, stdout=sink, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               start_new_session=True, env=env)
    sink.close()
    PROCS.append((name, process))
    return process


def start_server(args, work, database, tag, strace_log=None, ctl="sc", run="sr"):
    env = {k: v for k, v in os.environ.items() if k != "SCRATCHBIRD_CONFIG"}   # NO --config, NO config file from the environment
    process = start(tag, server_command(args, work, database, strace_log, ctl, run), work, env)
    wait_ready(os.path.join(work, ctl, "sb_server.lifecycle.state"), "service_ready=true", process, tag)
    check(os.path.exists(os.path.join(work, ctl, "sb_server.sbps.sock")), f"{tag}: serving but no SBPS socket")
    return process


def server_pid(work, ctl="sc"):
    with open(os.path.join(work, ctl, "sb_server.pid"), encoding="utf-8") as handle:
        return int(handle.read().strip())


def stop_server(process, work, tag, ctl="sc"):
    os.kill(server_pid(work, ctl), signal.SIGTERM)
    try:
        code = process.wait(timeout=120)
    except subprocess.TimeoutExpired:
        raise Fail(f"{tag}: no exit within 120 s of SIGTERM")
    check(code == 0, f"{tag}: exit {code} after SIGTERM")


def start_listener(args, work, database):
    last = None
    for attempt in range(3):                      # bind(0) then release is a race under -j parallel runs: retry on a new port
        port = find_free_port()
        process = start("listener", [
            args.listener, "--foreground", "--protocol-family=sbsql", "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1", f"--database-selector=dev_bootstrap_path:{database}",
            f"--server-endpoint=unix:{os.path.join(work, 'sc', 'sb_server.sbps.sock')}",
            f"--parser-executable={args.parser}", f"--control-dir={os.path.join(work, 'lc')}",
            f"--runtime-dir={os.path.join(work, 'lr')}", "--bind-address=127.0.0.1", f"--port={port}",
            "--warm-pool-min=1", "--warm-pool-max=2"], work)
        try:
            wait_for_tcp(port, timeout=60)
            if process.poll() is None:
                return process, port
        except Exception as exc:  # noqa: BLE001
            last = exc
        if process.poll() is None:
            process.kill()
        process.wait()
    raise Fail(f"the listener never accepted on a port: {last}")


def sbsql(args, work, port, label, sql, user=USER, password=PASSWORD, extra=()):
    command = [args.sbsql, os.path.join(work, "db", "t.sdb"), "--host=127.0.0.1", f"--port={port}",
               "--sslmode=disable", "-U", user, "-P", password, *extra, "-c", sql]
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired as exc:
        raise Fail(f"SBsql {label!r}: no answer within 120 s: {exc.stdout!r} {exc.stderr!r}")
    log = os.path.join(work, f"client-{label}.out")
    with open(log, "w", encoding="utf-8") as handle:
        handle.write(f"$ SBsql ... {' '.join(extra)} -c {sql!r}\nrc={done.returncode}\n--stdout--\n{done.stdout}\n--stderr--\n{done.stderr}\n")
    FAILURE_LOGS.append((f"client-{label}", log))
    return done


def line_client(port, lines, expect, after=None):
    """The line protocol of the parser (greeting, then AUTH / EXECUTE / QUIT). Each reply is read until its terminal
    line, with a deadline: no sleeps. `after(index, reply)` (optional) runs once each reply of lines[index] has been read."""
    conn = socket.create_connection(("127.0.0.1", port), timeout=30)
    conn.settimeout(30)
    buffer = b""

    def read_until(markers):
        nonlocal buffer
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            text = buffer.decode("utf-8", errors="replace")
            if any(re.search(marker, text, re.M) for marker in markers):
                taken, buffer = buffer, b""
                return taken.decode("utf-8", errors="replace")
            chunk = conn.recv(65536)
            if not chunk:
                break
            buffer += chunk
        raise Fail(f"line client: no terminal line {markers} in {buffer.decode('utf-8', errors='replace')[-800:]!r}")

    transcript = [read_until([r"parser ready"])]
    for index, (line, terminal) in enumerate(zip(lines, expect)):
        conn.sendall((line + "\n").encode())
        transcript.append(read_until(terminal))
        if after is not None:
            after(index, transcript[-1])
    conn.close()
    return transcript


def page_set(args, path, size):
    done = subprocess.run([args.login_bin, "pages", path, str(size)], capture_output=True, text=True, timeout=300)
    match = re.search(r"PAGES state=([\d,]*) header=([\d,]*) ledger=([\d,]*)", done.stdout)
    check(match, f"pages mode: {done.stdout!r} {done.stderr[-500:]!r}")
    allowed = set()
    for group in (1, 2, 3):
        allowed |= {int(x) for x in match.group(group).split(",") if x}
    return allowed


def state_dump(args, path, size):
    done = subprocess.run([args.state_dump, path, str(size)], capture_output=True, text=True, timeout=300)
    check(done.returncode == 0, f"native_state_dump: {done.stderr[-500:]}")
    tips = {int(m.group(1)): m.group(2) for m in re.finditer(r"^TIP order=(\d+) state=(\S+)", done.stdout, re.M)}
    ledger = re.findall(r"^LEDGER ordinal=(\d+) kind=(\S+)", done.stdout, re.M)
    return tips, ledger


def measure_per_login(args, work, created, size):
    """Derive the writes per login from the S4e child (native_login serve .. trace) on a COPY of the fresh file."""
    scratch = os.path.join(work, "measure")
    os.mkdir(scratch)
    copy = os.path.join(scratch, "m.sdb")
    shutil.copyfile(os.path.join(work, "db", "t.sdb"), copy)
    def field(name):
        return re.search(name + r"=([0-9a-f]+)", created).group(1)
    done = subprocess.run([args.login_bin, "serve", copy, "trace", field("database"), field("bootstrap"), field("service")],
                          capture_output=True, text=True, timeout=900)
    check(done.returncode == 0 and "LOGIN_OK" in done.stdout, f"the S4e measuring child failed: {done.stdout[-800:]} {done.stderr[-800:]}")
    got = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in re.finditer(r"MEASURE op=(\S+) writes=(\d+) syncs=(\d+)", done.stdout)}
    approve, ended = got["approve"][0], got["end_sessions"][0]
    check(approve % 2 == 0 and ended % 2 == 0 and approve > 0 and ended > 0, f"unexpected S4e counts {got}")
    shutil.rmtree(scratch)
    return approve // 2 + ended // 2, (approve // 2, ended // 2)


EXPECT_SERVER_FILES = {
    "sb_server.audit.sbobs", "sb_server.lifecycle.journal", "sb_server.lifecycle.state", "sb_server.metrics.sbobs",
    "sb_server.owner", "sb_server.pid", "sb_server.sbobs", "sb_server.sbps.endpoint", "support-bundles"}
# After a clean shutdown the SBPS socket is gone; support-bundles is a directory the server creates holding its one index file.
LISTENER_CONTROL = {".management.sock", ".owner", ".owner.lock"}
LISTENER_RUNTIME = {".lifecycle.state", ".lifecycle.state.lock"}


def check_directories(work):
    check(os.listdir(os.path.join(work, "db")) == ["t.sdb"], f"the database directory holds more than the database file: {sorted(os.listdir(os.path.join(work, 'db')))}")
    got = set(os.listdir(os.path.join(work, "sc")))
    check(got == EXPECT_SERVER_FILES, f"server control directory: unexpected {sorted(got - EXPECT_SERVER_FILES)}, missing {sorted(EXPECT_SERVER_FILES - got)}")
    check(os.listdir(os.path.join(work, "sc", "support-bundles")) == ["support_bundle_index.sbobs"], f"support-bundles holds {os.listdir(os.path.join(work, 'sc', 'support-bundles'))}")
    for name, suffixes in (("lc", LISTENER_CONTROL), ("lr", LISTENER_RUNTIME)):
        names = os.listdir(os.path.join(work, name))
        ids = {n.split(".", 1)[0] for n in names}
        check(len(ids) == 1, f"{name}: expected one listener id, found {sorted(names)}")
        stems = {"." + n.split(".", 1)[1] for n in names}
        check(stems == suffixes, f"{name}: files {sorted(names)} are not exactly the expected {sorted(suffixes)}")
    check(os.listdir(os.path.join(work, "sr")) == [], f"server runtime directory is not empty: {os.listdir(os.path.join(work, 'sr'))}")


def analyse_strace(log, path, logins, per_login, label, restart, size=0, allowed=frozenset(), map_pages=frozenset(), boundaries=0):
    with open(log, encoding="utf-8", errors="replace") as handle:
        lines = merge_split_lines(handle.readlines())
    sb_lines = [line for line in lines if ".sb." in line]
    check(not sb_lines, f"{label}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip() if sb_lines else ''}")
    spans = data_descriptors(lines, path)
    check(spans, f"{label}: no open of the data file was traced (the detector sees nothing)")
    region = "startup"
    counts = {"startup": [0, 0], "serving": [0, 0], "shutdown": [0, 0]}
    map_writes = []      # 512-byte writes to the page-allocation map page(s)
    full_pages = []      # (page number) of every whole-page write: a chain acquiring a NEW page is written whole, not in 512-byte sectors
    for index, line in enumerate(lines):
        if region == "startup" and "service_ready=true" in line:
            region = "serving"
        elif region == "serving" and re.search(r"--- SIGTERM ", line):
            region = "shutdown"
        m = re.search(r"\bpwrite64\((\d+),.*,\s*(\d+),\s*(\d+)\)\s*=\s*(-?\d+)", line)
        if m and spans.covers(index):
            count, offset, result = m.group(2), int(m.group(3)), m.group(4)
            if count == str(size) and result == count and offset % size == 0 and size != 512:
                full_pages.append(offset // size)
                check(region == "serving", f"{label} {region}: a whole-page data write outside the serving region: {line.strip()[:200]}")
                continue
            check(count == "512" and result == "512", f"{label} {region}: a {count}-byte data write at {offset} (returned {result}): {line.strip()[:200]}")
            if size and offset // size in map_pages:
                map_writes.append(offset // size)
            else:
                counts[region][0] += 1
            continue
        m = re.search(r"\b(?:fsync|fdatasync)\((\d+)\)\s*=\s*0", line)
        if m and spans.covers(index):
            counts[region][1] += 1
            continue
        m = re.search(r"\b" + WRITE_CALLS + r"\((\d+)", line)
        if m and spans.covers(index):
            raise Fail(f"{label} {region}: a write-class syscall that is not a 512-byte pwrite64 (or a whole new chain page) or an fsync on the data file: {line.strip()[:200]}")
    check(region != "startup", f"{label}: the service_ready marker was never traced")
    check(counts["startup"] == [0, 0], f"{label}: {counts['startup']} writes/syncs on the data file during the open")
    check(not full_pages or set(full_pages) <= set(allowed), f"{label}: whole-page writes to pages {sorted(full_pages)} that are not chain pages {sorted(allowed)}")
    check(len(full_pages) == len(set(full_pages)), f"{label}: a page was written whole more than once: {sorted(full_pages)}")
    counts["full_pages"] = sorted(full_pages)
    counts["map_writes"] = len(map_writes)
    if restart:
        check(all(counts[k] == [0, 0] for k in ("startup", "serving", "shutdown")) and not full_pages, f"{label}: a restart wrote or synced the data file: {counts}")
    else:
        # The S4e per-login count covers the state, header and ledger chains' sectors. A chain acquiring a NEW page adds, per new page, exactly the
        # whole-page write (counted apart), ONE 512-byte link write into the chain's previous page, and at most TWO 512-byte writes to the allocation map.
        # S5: each COMMIT / ROLLBACK boundary is ONE CompleteAndBegin: at most 4 sector writes (T1: 4 for an odd n, 3 for an even n) and ONE sync
        # under the default policy; the boundary adds NO ledger record.
        bound = logins * per_login + len(full_pages) + 4 * boundaries
        check(counts["serving"][0] <= bound, f"{label}: {counts['serving'][0]} chain-sector pwrite64 on the data file, at most {logins} logins x {per_login} + {len(full_pages)} link writes + 4 x {boundaries} boundaries = {bound} are implied by the approvals and the {len(full_pages)} new chain pages")
        check(len(map_writes) <= 2 * len(full_pages), f"{label}: {len(map_writes)} allocation-map writes for {len(full_pages)} new chain pages (at most 2 each)")
        every_write = counts["serving"][0] + len(full_pages) + len(map_writes)
        check(counts["serving"][1] <= every_write, f"{label}: more syncs ({counts['serving'][1]}) than data-file writes ({every_write})")
        check(counts["serving"][0] > 0, f"{label}: no pwrite64 at all for {logins} logins: the detector is blind")
        check(counts["shutdown"][0] == 0 and counts["shutdown"][1] <= 1, f"{label}: shutdown wrote {counts['shutdown']}")
    return counts, len(lines)


def header_dump(args, path, size):
    done = subprocess.run([args.state_dump, path, str(size)], capture_output=True, text=True, timeout=300)
    check(done.returncode == 0, f"native_state_dump: {done.stderr[-500:]}")
    out = {}
    for m in re.finditer(r"^HEADER order=(\d+) (.*)$", done.stdout, re.M):
        out[int(m.group(1))] = dict(item.split("=", 1) for item in m.group(2).split())
    return out


def run(args, work):
    size = args.size
    for name in ("db", "sc", "sr", "lc", "lr"):
        os.mkdir(os.path.join(work, name))
    database = os.path.join(work, "db", "t.sdb")
    created = subprocess.run([args.material_bin, "create", database, str(size)], capture_output=True, text=True, timeout=900)
    check(created.returncode == 0 and created.stdout.startswith("CREATED "), f"create: {created.stdout[-400:]} {created.stderr[-1500:]}")
    per_login, (per_approve, per_end) = measure_per_login(args, work, created.stdout, size)
    with open(database, "rb") as handle:
        image_before = handle.read()
    tips0, ledger0 = state_dump(args, database, size)
    chains_before = page_set(args, database, size)
    check(len(tips0) == 1 and not ledger0, f"a fresh file should hold one TIP slot and no ledger record: {tips0} {ledger0}")

    log1 = os.path.join(work, "srv1.strace") if args.strace else None
    server = start_server(args, work, database, "server1", log1)
    listener, port = start_listener(args, work, database)
    logins = 0

    # --- the wrong password first: generic failure, the file is untouched ---
    before = sha256(database)
    bad = sbsql(args, work, port, "wrong", "SELECT 1 AS one", password=WRONG)
    check(bad.returncode != 0 and "SECURITY.AUTHENTICATION.LOGIN_FAILED" in bad.stdout + bad.stderr,
          f"a wrong password must be refused with the generic LOGIN_FAILED, got rc={bad.returncode}: {bad.stdout!r} {bad.stderr!r}")
    for leak in (WRONG, PASSWORD):
        check(leak not in bad.stdout + bad.stderr, f"the failure output echoes a password")
    check(sha256(database) == before, "a wrong password changed the database file")

    # --- the accepted shapes ---
    good = sbsql(args, work, port, "one", "SELECT 1 AS one"); logins += 1
    check(good.returncode == 0 and re.search(r"^\|\s*one\s*\|\s*$", good.stdout, re.M) and re.search(r"^\|\s*1\s*\|\s*$", good.stdout, re.M)
          and "(1 row)" in good.stdout, f"SELECT 1 AS one: rc={good.returncode} {good.stdout!r} {good.stderr!r}")
    quiet = sbsql(args, work, port, "quiet", "SELECT 1 AS one", extra=("-q", "-A", "-t")); logins += 1
    check(quiet.returncode == 0 and quiet.stdout.strip() == "1", f"-q -A -t: {quiet.stdout!r} {quiet.stderr!r}")
    two = sbsql(args, work, port, "two", "SELECT 2 AS two"); logins += 1
    check(two.returncode == 0 and re.search(r"^\|\s*two\s*\|\s*$", two.stdout, re.M) and re.search(r"^\|\s*2\s*\|\s*$", two.stdout, re.M),
          f"SELECT 2 AS two: {two.stdout!r} {two.stderr!r}")
    both = sbsql(args, work, port, "both", "SELECT 1 AS one, 2 AS two"); logins += 1
    check(both.returncode == 0 and re.search(r"^\|\s*one\s*\|\s*two\s*\|\s*$", both.stdout, re.M)
          and re.search(r"^\|\s*1\s*\|\s*2\s*\|\s*$", both.stdout, re.M), f"SELECT 1 AS one, 2 AS two: {both.stdout!r} {both.stderr!r}")

    # --- ROUTE B: bare `SELECT 1` (the object-free literal VALUES relation, query.execute) runs natively. The column name is the PARSER's
    # default for the one column of a literal VALUES relation (2026-12-12; OLD: `key_a`, a fixture-list name from its own binding context; NEW:
    # `column1`, the same rule as the unaliased scalar-projection route); the engine sends no name. ---
    for label, sql, value in (("bare1", "SELECT 1", "1"), ("bare-lower", "select 1", "1"), ("bare-semi", "SELECT 1;", "1"),
                              ("bare2", "SELECT 2", "2"), ("values1", "VALUES (1)", "1"),
                              ("bare-max", "SELECT 9223372036854775807", "9223372036854775807")):
        bare = sbsql(args, work, port, label, sql); logins += 1
        check(bare.returncode == 0 and re.search(r"^\|\s*column1\s*\|\s*$", bare.stdout, re.M) and re.search(r"^\|\s*" + value + r"\s*\|\s*$", bare.stdout, re.M)
              and "(1 row)" in bare.stdout, f"{sql!r}: rc={bare.returncode} {bare.stdout!r} {bare.stderr!r}")
    quiet_bare = sbsql(args, work, port, "quiet-bare", "SELECT 1", extra=("-q", "-A", "-t")); logins += 1
    check(quiet_bare.returncode == 0 and quiet_bare.stdout.strip() == "1", f"bare SELECT 1 with -q -A -t: {quiet_bare.stdout!r} {quiet_bare.stderr!r}")
    # two statements in one call: each runs on its own (one login, two executions)
    pair = sbsql(args, work, port, "bare-pair", "SELECT 1; SELECT 2"); logins += 1
    check(pair.returncode == 0 and re.search(r"^\|\s*1\s*\|\s*$", pair.stdout, re.M) and re.search(r"^\|\s*2\s*\|\s*$", pair.stdout, re.M)
          and "(2 rows)" in pair.stdout, f"SELECT 1; SELECT 2: rc={pair.returncode} {pair.stdout!r} {pair.stderr!r}")

    # --- the refused shapes: machine tokens, a failed exit ---
    for label, sql, token in (("plus", "SELECT 1 + 0 AS s", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"),
                              ("text", "SELECT 'a' AS t", "ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT"),
                              # ROUTE B (2026-12-11). OLD: ("bare", "SELECT 1", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED") -- bare SELECT 1 now RUNS (above).
                              ("bare-plus", "SELECT 1 + 0", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"),
                              ("bare-neg", "SELECT -1", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"),
                              ("values2", "VALUES (1), (2)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"),
                              ("bare-text", "SELECT 'a'", "ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT"),
                              ("bare-dec", "SELECT 1.5", "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1"),
                              ("bare-from", "SELECT 1 FROM t", "ENGINE.NATIVE.WIRE_NAME_RESOLVE_S5"),
                              ("ddl", "CREATE TABLE x (a int)", "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED")):
        refused = sbsql(args, work, port, label, sql); logins += 1
        check(refused.returncode != 0 and token in refused.stdout + refused.stderr,
              f"{sql!r}: expected {token}, rc={refused.returncode}: {refused.stdout!r} {refused.stderr!r}")

    # --- S5: COMMIT / ROLLBACK from statements (plan 36 CompleteAndBegin): the session keeps working in the REPLACEMENT transaction ---
    boundaries = {}   # login index (0-based, counting only logins that hold a slot) -> the finalities its statements ended, in order
    def boundary_login(label, sql, finalities, pattern=None, expect_rc=0, token=None):
        nonlocal logins
        done = sbsql(args, work, port, label, sql)
        index = logins
        logins += 1
        if token is not None:
            check(done.returncode != 0 and token in done.stdout + done.stderr, f"{sql!r}: expected {token}, rc={done.returncode}: {done.stdout!r} {done.stderr!r}")
        else:
            check(done.returncode == 0, f"{sql!r}: rc={done.returncode}: {done.stdout!r} {done.stderr!r}")
            if pattern:
                check(re.search(pattern[0], done.stdout, re.M) and re.search(pattern[1], done.stdout, re.M), f"{sql!r}: {done.stdout!r}")
        boundaries[index] = list(finalities)
        return done
    boundary_login("commit-one", "COMMIT; SELECT 1 AS one", ["committed"], (r"^\|\s*one\s*\|\s*$", r"^\|\s*1\s*\|\s*$"))
    boundary_login("rollback-two", "ROLLBACK; SELECT 2 AS two", ["rolled_back"], (r"^\|\s*two\s*\|\s*$", r"^\|\s*2\s*\|\s*$"))
    boundary_login("commit-work", "COMMIT WORK; ROLLBACK WORK; SELECT 1 AS one", ["committed", "rolled_back"], (r"^\|\s*one\s*\|\s*$", r"^\|\s*1\s*\|\s*$"))
    boundary_login("repeat", "COMMIT; COMMIT; ROLLBACK; COMMIT; SELECT 2 AS two", ["committed", "committed", "rolled_back", "committed"],
                   (r"^\|\s*two\s*\|\s*$", r"^\|\s*2\s*\|\s*$"))
    # RETAINING (Q169) is NOT built on the native path: ONE specific code, nothing written (the statement ends no transaction).
    boundary_login("retain", "COMMIT RETAIN", [], token="ENGINE.NATIVE.OPERATION_NOT_BUILT")
    boundary_login("retaining", "COMMIT RETAINING", [], token="ENGINE.NATIVE.OPERATION_NOT_BUILT")
    # Strict tokens (controller Q1): nothing after COMMIT / ROLLBACK is silently ignored; ROLLBACK RETAIN[ING] has no lowering yet.
    boundary_login("rb-retain", "ROLLBACK RETAIN", [], token="SBSQL.PARSER.ROLLBACK_RETAINING_NOT_BUILT")
    boundary_login("rb-retaining", "ROLLBACK RETAINING", [], token="SBSQL.PARSER.ROLLBACK_RETAINING_NOT_BUILT")
    boundary_login("rb-chain", "ROLLBACK AND CHAIN", [], token="SBSQL.PARSER.TRANSACTION_CONTROL_TOKEN_UNKNOWN")
    boundary_login("commit-garbage", "COMMIT garbage", [], token="SBSQL.PARSER.TRANSACTION_CONTROL_TOKEN_UNKNOWN")

    # --- the line client over the same port (AUTH / EXECUTE / QUIT) ---
    transcript = line_client(port, [f"AUTH {USER} {PASSWORD}", "EXECUTE SELECT 1 AS one", "QUIT"],
                             [[r"^OK AUTHENTICATED"], [r"^RESULT \S+ \d+ \S+", r"^ERROR"], [r"^OK BYE"]]); logins += 1
    result = [line for line in transcript[2].splitlines() if line.startswith("RESULT ")]
    check(result and result[0].startswith("RESULT query.evaluate_projection 1 canonical_binary_payload_hex="), f"line client result: {transcript[2][-600:]!r}")
    payload = bytes.fromhex(result[0].split("canonical_binary_payload_hex=", 1)[1].strip())
    check(payload.startswith(b"SBRES002") and b"scalar_projection_rows" in payload and b"one:int64:not_null" in payload,
          f"line client result payload is not the one-column scalar projection: {payload[:200]!r}")
    check(payload.endswith(b"one:int64:not_null"), "line client payload: the column meta is not last")
    check(re.search(rb"row\[0\]\x04\x24\x00{7}SBRES002", payload) is not None, "line client payload: row[0] is not an SBRES002 row")

    # --- ROUTE B, one OPEN line-protocol session: after the login the data file is not touched by ANY statement, accepted or refused ---
    held = {}
    def untouched(index, reply):
        if index == 0:
            held["image"] = sha256(database)            # after AUTH: the approval and the first transaction are written
        elif index in (1, 2, 3, 4, 5, 6):
            check(sha256(database) == held["image"], f"route B: statement {index} changed the data file (reply {reply[-200:]!r})")
    routeb_lines = [f"AUTH {USER} {PASSWORD}", "EXECUTE SELECT 1", "EXECUTE SELECT 2", "EXECUTE SELECT 1 + 0",
                    "EXECUTE SELECT -1", "EXECUTE VALUES (1), (2)", "EXECUTE SELECT 1.5", "QUIT"]
    routeb = line_client(port, routeb_lines,
                         [[r"^OK AUTHENTICATED"]] + [[r"^RESULT \S+ \d+ \S+", r"^MESSAGE ERROR"]] * 6 + [[r"^OK BYE"]], after=untouched); logins += 1
    results = [[l for l in t.splitlines() if l.startswith(("RESULT ", "MESSAGE ERROR"))] for t in routeb[2:8]]
    check(all(r for r in results), f"route B line session: a statement gave neither RESULT nor MESSAGE ERROR: {routeb!r}")
    check(results[0][0].startswith("RESULT query.execute 1 canonical_binary_payload_hex="), f"route B line SELECT 1: {results[0][0][:200]!r}")
    payload_b = bytes.fromhex(results[0][0].split("canonical_binary_payload_hex=", 1)[1].strip())
    check(payload_b.startswith(b"SBRES002") and b"scalar_projection_rows" in payload_b and payload_b.endswith(b"column1:int64:not_null"),
          f"route B line SELECT 1 payload is not the one-column row built by the parser: {payload_b[:120]!r}")
    check(re.search(rb"row\[0\][\x00-\xff]{1,16}SBRES002", payload_b, re.S) is not None, f"route B line payload: row[0] is not an SBRES002 row: {payload_b[-120:]!r}")
    check(results[1][0].startswith("RESULT query.execute 1 "), f"route B line SELECT 2: {results[1][0][:200]!r}")
    for index, token in ((2, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"), (3, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"),
                         (4, "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED"), (5, "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1")):
        check(any(token in line for line in results[index]), f"route B line statement {index + 1}: expected {token}, got {results[index]!r}")

    # --- shutdown, listener first, then the server ---
    listener_process = listener
    os.kill(listener_process.pid, signal.SIGTERM)
    try:
        listener_process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        raise Fail("the listener did not exit within 60 s of SIGTERM")
    stop_server(server, work, "server1")
    leftovers = process_alive_for(work)
    check(not leftovers, f"processes of this run are still alive after the shutdown: {leftovers}")

    check_directories(work)
    with open(database, "rb") as handle:
        image_after = handle.read()
    check(len(image_after) == len(image_before), f"the file length changed ({len(image_before)} -> {len(image_after)})")
    changed = {n for n in range(len(image_after) // size) if image_before[n * size:(n + 1) * size] != image_after[n * size:(n + 1) * size]}
    allowed = page_set(args, database, size)
    # A chain that acquires a NEW page (a ledger or header chain growing under the sessions of this run) updates the page-allocation map page
    # (page type word 3, the same type as pages 1 and 2): observed at page 12 once a chain grew past its first page. So a changed page that is
    # not in a chain is accepted ONLY if it is a type-3 page AND a chain grew; anything else (a catalog, data or index page) fails.
    grew = allowed > chains_before
    page_type = lambda image, n: int.from_bytes(image[n * size + 16:n * size + 20], "little")
    outside = changed - allowed
    outside_ok = {n for n in outside if page_type(image_after, n) == 3}
    check(changed and changed - allowed <= {n for n in outside if page_type(image_after, n) == 3 and grew},
          f"changed pages {sorted(changed)} are not a subset of the TIP / header / ledger pages {sorted(allowed)} "
          f"(plus allocation-map pages when a chain grew; chains grew: {grew}; outside types: "
          f"{ {n: page_type(image_after, n) for n in outside} })")
    tips, ledger = state_dump(args, database, size)
    map_pages = outside_ok
    # Expected TIP: slot 1 is the create; each login then holds one slot per boundary it ran plus the replacement that its end rolled back.
    expected = ["committed"]
    boundary_total = 0
    for login in range(logins):
        ended = boundaries.get(login, [])
        boundary_total += len(ended)
        expected.extend(ended + ["rolled_back"])
    check([tips[order] for order in sorted(tips)] == expected and sorted(tips) == list(range(1, len(expected) + 1)),
          f"TIP slots: the boundaries must leave exactly {expected}; found {tips}")
    # HEADER evidence (plan 36 CompleteAndBegin): every boundary's ended slot is final, its replacement is the NEXT order, and the
    # replacement keeps the principal (+64), attachment (+80) and session (+96) of the ended transaction and its isolation and read-only.
    headers = header_dump(args, database, size)
    check(sorted(headers) == sorted(tips), f"header slots {sorted(headers)} differ from the TIP slots {sorted(tips)}")
    order = 2
    for login in range(logins):
        ended = boundaries.get(login, [])
        for finality in ended + ["rolled_back"]:
            header = headers[order]
            check(header["final"] == finality and header["completed_at"] == "1", f"slot {order}: header {header} is not final {finality}")
            check(header["commit_time"] == ("1" if finality == "committed" else "0"), f"slot {order}: commit time wrong: {header}")
            check(set(header["session"]) != {"0"} and set(header["attachment"]) != {"0"} and set(header["principal"]) != {"0"},
                  f"slot {order}: header identities not bound: {header}")
            if order > 2 and (order - 1) in headers and headers[order - 1]["session"] == header["session"]:
                previous = headers[order - 1]
                check(previous["principal"] == header["principal"] and previous["attachment"] == header["attachment"] and
                      previous["isolation"] == header["isolation"] and previous["read_only"] == header["read_only"],
                      f"slot {order}: the replacement does not keep the identity or settings of slot {order - 1}: {previous} vs {header}")
            order += 1
    sessions_of = [headers[o]["session"] for o in sorted(headers) if o != 1]
    check(len(set(sessions_of)) == logins, f"{logins} sessions expected across the headers, found {len(set(sessions_of))}")
    kinds = [kind for _, kind in ledger]
    check(kinds == ["connection", "session", "attachment", "session"] * logins,
          f"each login must leave exactly connection, session (begin), attachment, session (end) in the ledger, {logins} logins; found {len(ledger)} records: {kinds}")

    counts1 = None
    lines1 = 0
    if args.strace:
        counts1, lines1 = analyse_strace(log1, database, logins, per_login, "run", False, size, allowed, map_pages, boundary_total)

    # --- restart: serves again; nothing written ---
    image_restart_before = sha256(database)
    log2 = os.path.join(work, "srv2.strace") if args.strace else None
    # A fresh control directory: restarting in the SAME one after a clean shutdown is refused by the server
    # (SERVER.RUNTIME.OWNER_TOKEN_AMBIGUOUS: the previous run's owner / pid / lifecycle files remain); reported, not worked around by deleting them.
    os.mkdir(os.path.join(work, "sc2")); os.mkdir(os.path.join(work, "sr2"))
    server2 = start_server(args, work, database, "server2", log2, "sc2", "sr2")
    stop_server(server2, work, "server2", "sc2")
    check(sha256(database) == image_restart_before, "a restart of SBsrv changed the database file")
    check(state_dump(args, database, size) == (tips, ledger), "a restart changed the TIP / ledger state")
    counts2 = None
    if args.strace:
        counts2, _ = analyse_strace(log2, database, 0, per_login, "restart", True, size, allowed, map_pages)
    leftovers = process_alive_for(work)
    check(not leftovers, f"processes of this run are still alive after the restart: {leftovers}")
    check(os.listdir(os.path.join(work, "db")) == ["t.sdb"], "the database directory holds more than the database file after the restart")

    summary = (f"native_wire_exchange {size}: {logins} logins + 1 refused; one=1 through SBsql, SBsql -q -A -t and the line client; "
               f"refused shapes keep their tokens; wrong password: generic failure, file unchanged; SBsrv exit 0; db dir = the file only; "
               f"changed pages {sorted(changed)} within the chains; {logins} sessions ended, {boundary_total} COMMIT/ROLLBACK boundaries (TIP slots {len(tips)}, {len(ledger)} ledger records, 4 per login); restart serves")
    if args.strace:
        summary += (f"; strace: 0 .sb. syscalls; 512-byte pwrite64 {counts1['serving'][0]} + whole new chain pages {counts1['full_pages']} + {counts1['map_writes']} map writes <= {logins}x{per_login} (S4e: {per_approve} per approval + {per_end} per end), "
                    f"fsync {counts1['serving'][1]}, shutdown {counts1['shutdown']}; restart {counts2}; {lines1} syscalls traced")
    print(summary)


def negative_control(args, work):
    control_dir = os.path.join(work, "control")
    os.mkdir(control_dir)
    path = os.path.join(control_dir, "control.sdb")
    log = os.path.join(work, "control.strace")
    subprocess.run(["strace", "-f", "-qq", "-s", "64", "-o", log, args.material_bin, "create", path, "8192"],
                   capture_output=True, text=True, timeout=900)
    with open(log, encoding="utf-8", errors="replace") as handle:
        lines = merge_split_lines(handle.readlines())
    spans = data_descriptors(lines, path)
    writes = [line for index, line in enumerate(lines) if re.search(r"\bpwrite64\(", line) and spans.covers(index)]
    check(writes, "negative control: the detector saw no pwrite64 on the data file during a create; it cannot be trusted")
    print(f"negative control: {len(writes)} pwrite64 on the data file during create")


def main():
    parser = argparse.ArgumentParser()
    for name in ("sbsrv", "listener", "parser", "sbsql", "material-bin", "login-bin", "state-dump"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--strace", action="store_true")
    args = parser.parse_args()
    if args.strace and shutil.which("strace") is None:
        print("FAIL strace is required")
        return 1
    work = tempfile.mkdtemp(prefix="sbwx", dir="/tmp")     # short: the listener refuses long control paths
    status = 1
    try:
        run(args, work)
        if args.strace:
            negative_control(args, work)
        status = 0
    except Fail as exc:
        print(f"FAIL native_wire_exchange {args.size}: {exc}")
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL native_wire_exchange {args.size}: unexpected {type(exc).__name__}: {exc}")
    finally:
        for name, process in PROCS:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except OSError:
                    pass
                process.wait()
        # the stack's own children (parser workers) may have left their group: kill anything naming this run
        for pid, _ in process_alive_for(work):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        if status != 0:
            for title, path in FAILURE_LOGS:
                text = read_text(path)
                if text:
                    print(f"--- {title} ({path}) ---\n{text}")
        if os.environ.get("SB_WIRE_EXCHANGE_KEEP") == "1":
            print(f"kept work directory {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
