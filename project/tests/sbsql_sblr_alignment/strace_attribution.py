#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_STRACE_ATTRIBUTION. Attribute the syscalls of an `strace -f -qq -o LOG` log to the data file.

Why this exists. The earlier helper keyed descriptor numbers across the whole log, matched a close only as one
line `close(N) = 0`, and ignored which process a line came from. Under load `strace -f` splits a call into
`name(... <unfinished ...>` and `<... name resumed>...) = r` (other processes' lines in between), and descriptor
tables are PER PROCESS (threads made with CLONE_FILES share one, fork/vfork children get a copy). A missed close
left a data-descriptor span open past the real close, and a pipe that reused the number (the SBACRED1 startup
credential frame) was then reported as a write to the data file (observed once, 16K, full suite at -j8). The
mirror error, a split open or dup that was never seen, would hide a real data-file write.

What it does. (1) Reassembles every split call per pid (a pid has at most one call in flight). (2) Follows the
clone family to give each pid its descriptor table: shared with the parent for CLONE_FILES, a copy otherwise
(fork, vfork, glibc fork's clone without CLONE_FILES); the child table is created at the parent's clone START
line, so a child line printed before the parent's `clone resumed` line is still attributed. (3) Per table, a data
descriptor is made by an open of the data path, or a dup / dup2 / dup3 / fcntl(F_DUPFD) of one; it ends at close,
close_range, or an overwrite by dup2/dup3; ANY syscall that returns a descriptor number (open of another file,
socket, pipe, accept, eventfd, ...) also ends a stale data span on that number, so a missed close can never
attribute a reused number; execve drops close-on-exec descriptors. (4) A line is "on the data file" when the
first argument of its syscall is a data descriptor in ITS pid's table at the moment the call STARTED.

Not followed (a sound strace parse cannot see them): descriptors received by SCM_RIGHTS, pidfd_getfd, mmap of
the data file, io_uring submission queues, and copy_file_range / splice / sendfile output descriptors (not the
first argument). A caller needing those must assert them by another means.
"""
import re

PID_RE = re.compile(r'^(?:\[pid\s+(\d+)\]|(\d+))\s+(.*)$', re.S)
UNFINISHED_RE = re.compile(r'^(.*?)\s*<unfinished \.\.\.>\s*$', re.S)
RESUMED_RE = re.compile(r'^<\.\.\.\s+(\w+)\s+resumed>(.*)$', re.S)
FIRST_ARG_RE = re.compile(r'^(\w+)\((\d+)(?:<[^>]*>)?[,)]')
NAME_RE = re.compile(r'^(\w+)\(')
RESULT_RE = re.compile(r'\)\s*=\s*(-?\d+|\?)')
CLONE_RE = re.compile(r'^(clone3?|fork|vfork)\(')
# Syscalls whose successful result is a NEW descriptor number (so that number cannot still be a data descriptor).
FD_RETURNING = frozenset((
    'open', 'openat', 'openat2', 'creat', 'socket', 'accept', 'accept4', 'dup', 'dup2', 'dup3',
    'eventfd', 'eventfd2', 'epoll_create', 'epoll_create1', 'timerfd_create', 'signalfd', 'signalfd4',
    'inotify_init', 'inotify_init1', 'memfd_create', 'pidfd_open', 'pidfd_getfd', 'userfaultfd',
    'io_uring_setup', 'perf_event_open', 'fanotify_init', 'fsopen', 'open_tree', 'mq_open', 'memfd_secret'))
FD_PAIR_RETURNING = frozenset(('pipe', 'pipe2', 'socketpair'))
# NEW (slow-cost2): the names whose branches in data_descriptors read the call's result / the table before it changes.
_DUP_NAMES = frozenset(('dup', 'dup2', 'dup3', 'fcntl'))
_RESULT_NAMES = (frozenset(('close', 'close_range', 'execve', 'execveat', 'fcntl')) | FD_PAIR_RETURNING | FD_RETURNING)


class StrayPid(Exception):
    """A pid appears in the log that is neither the first (root traced) pid nor the child of a parsed
    clone/fork/vfork. Its descriptor table would be unknown (an empty guess could hide a data-file write), so the
    attribution refuses rather than guess."""


class DataFds:
    """The result: which log lines are calls on the data file. `covers(index)` / `origin(index)` take the 0-based
    index of the line on which the call STARTED (for a split call, the `<unfinished ...>` line)."""

    def __init__(self, hits, descriptors):
        self._hits = hits
        self.descriptors = descriptors  # [(pid, fd, origin, start_index)] one per open/dup of the data file

    def __len__(self):
        return len(self.descriptors)

    def __bool__(self):
        return bool(self.descriptors)

    def origin(self, index):
        return self._hits.get(index)

    def covers(self, index):
        return index in self._hits


def _split_pid(line):
    match = PID_RE.match(line.rstrip('\n'))
    if not match:
        return None, None
    return int(match.group(1) or match.group(2)), match.group(3)


def reassemble(lines):
    """Calls in log order: (pid, start_index, end_index, text). A split call's text is its
    `<unfinished ...>` prefix joined to the remainder of its `resumed` line; end_index is the resumed line.
    A call still unfinished at the end of the log is returned with end_index None (its text is the prefix).
    Lines that are not calls (signals, exits, attach notes) are skipped."""
    pending = {}
    calls = []
    for index, line in enumerate(lines):
        pid, body = _split_pid(line)
        if pid is None:
            continue
        # NEW (slow-cost2, 2026-12-27): a literal test before each regex. OLD: both regexes ran on every line; UNFINISHED_RE's lazy
        # `(.*?)` walks a whole long line (the log is written with -s 1024) before failing, and it was ~40% of this module's time.
        # Each pattern can only match when its literal is present (RESUMED_RE starts with `<...`, UNFINISHED_RE contains
        # `<unfinished ...>`), so the result is the same match object or None as before.
        resumed = RESUMED_RE.match(body) if body.startswith('<...') else None
        if resumed:
            held = pending.pop(pid, None)
            if held is not None:
                calls.append((pid, held[0], index, held[1] + resumed.group(2)))
            continue
        unfinished = UNFINISHED_RE.match(body) if '<unfinished ...>' in body else None
        if unfinished:
            pending[pid] = (index, unfinished.group(1))
            continue
        if body.startswith('+++') or body.startswith('---'):
            continue
        calls.append((pid, index, index, body))
    for pid, (index, prefix) in pending.items():
        calls.append((pid, index, None, prefix))
    calls.sort(key=lambda call: call[1])
    return calls


def merge_split_lines(lines):
    """`lines` with every split call joined onto its start line and the resumed line blanked (same length and
    indexes), for callers that match whole-call regexes (a result on the line, e.g. `pwrite64(...) = 512`)."""
    out = list(lines)
    pending = {}
    for index, line in enumerate(lines):
        pid, body = _split_pid(line)
        if pid is None:
            continue
        # NEW (slow-cost2): the same literal tests as in reassemble (OLD: both regexes on every line).
        resumed = RESUMED_RE.match(body) if body.startswith('<...') else None
        if resumed:
            held = pending.pop(pid, None)
            if held is not None:
                out[held[0]] = held[1] + resumed.group(2) + '\n'
                out[index] = '\n'
            continue
        unfinished = UNFINISHED_RE.match(body) if '<unfinished ...>' in body else None
        if unfinished:
            pending[pid] = (index, line[:line.index(body)] + unfinished.group(1))
    return out


def _clone_flags_share_files(text):
    if text.startswith('fork(') or text.startswith('vfork('):
        return False
    return bool(re.search(r'\bCLONE_FILES\b', text))


def data_descriptors(lines, path):
    """`path` is the absolute data-file path, or a compiled regex matching the quoted path (for a test that
    only knows the file's name)."""
    calls = reassemble(lines)
    # Pass 1: the clone family's results, keyed by the parent's call start index.
    clone_at = {}
    for pid, start, end, text in calls:
        if CLONE_RE.match(text):
            result = RESULT_RE.search(text)
            if result and result.group(1) not in ('?',) and int(result.group(1)) > 0:
                clone_at[(pid, start)] = (int(result.group(1)), _clone_flags_share_files(text))
    open_re = re.compile(r'^(?:open|openat|openat2|creat)\(.*"' + (path.pattern if hasattr(path, 'pattern') else re.escape(path)) + r'"')
    tables = {}  # pid -> dict(fd -> [origin, cloexec]); threads sharing a table point at the same dict

    root = calls[0][0] if calls else None
    first_line = {}
    for pid, start, end, text in calls:
        first_line.setdefault(pid, (start, text))

    def table_of(pid):
        if pid not in tables:
            if pid != root:
                raise StrayPid(f"pid {pid} has no clone/fork/vfork record in the log and is not the root traced pid "
                               f"{root}; first line {first_line[pid][0] + 1}: {first_line[pid][1][:160]!r}")
            tables[pid] = {}
        return tables[pid]

    # Events: starts classify the call, ends change state. One call has both at the same index unless split.
    events = []
    for call in calls:
        pid, start, end, text = call
        events.append((start, 0, call))
        if end is not None:
            events.append((end, 1, call))
        else:
            events.append((len(lines) + start, 1, call))  # never completed: apply at the end, harmless
    events.sort(key=lambda event: (event[0], event[1]))
    hits = {}
    descriptors = []
    for _, kind, (pid, start, end, text) in events:
        table = table_of(pid)
        if kind == 0:
            # NEW (slow-cost2): with no data descriptor in the table nothing can be a hit, so the regex is skipped (OLD: always run).
            first = FIRST_ARG_RE.match(text) if table else None
            if first and first.group(2) in table:
                hits[start] = table[first.group(2)][0]
            made = clone_at.get((pid, start))
            if made:
                child, share = made
                tables[child] = table if share else {fd: list(v) for fd, v in table.items()}
            continue
        name_match = NAME_RE.match(text)
        if not name_match:
            continue
        name = name_match.group(1)
        # NEW (slow-cost2): the descriptor-table copy and the unanchored result search ran for EVERY call (OLD: dup_sources = set(table);
        # result = RESULT_RE.search(text)); only the branches below for these names read them, so they are made only for those names.
        dup_sources = set(table) if name in _DUP_NAMES else ()
        result = RESULT_RE.search(text) if name in _RESULT_NAMES else None
        ret = result.group(1) if result else None
        succeeded = ret is not None and ret != '?' and not ret.startswith('-')
        if name == 'close':
            arg = FIRST_ARG_RE.match(text)
            if arg and ret is not None and 'EBADF' not in text:
                table.pop(arg.group(2), None)
        elif name == 'close_range':
            args = re.match(r'^close_range\((\d+),\s*(\d+|~0U|-1)\s*,\s*([^)]*)\)', text)
            if args and succeeded:
                low = int(args.group(1))
                high = float('inf') if not args.group(2).isdigit() else int(args.group(2))
                for fd in [f for f in table if low <= int(f) <= high]:
                    if 'CLOSE_RANGE_CLOEXEC' in args.group(3):
                        table[fd][1] = True
                    else:
                        del table[fd]
        elif name == 'execve' or name == 'execveat':
            if succeeded and ret == '0':
                for fd in [f for f, v in table.items() if v[1]]:
                    del table[fd]
        elif name in FD_PAIR_RETURNING:
            if succeeded:
                for number in re.findall(r'\[(\d+),\s*(\d+)\]', text)[:1]:
                    for fd in number:
                        table.pop(fd, None)
        elif name in FD_RETURNING and succeeded:
            table.pop(ret, None)  # a new descriptor number: whatever held it was closed (a close we did not see)
            if open_re.match(text):
                table[ret] = ['open', 'O_CLOEXEC' in text]
                descriptors.append((pid, ret, 'open', start))
            elif name in ('dup', 'dup2', 'dup3'):
                dup = re.match(r'^dup[23]?\((\d+)(?:<[^>]*>)?(?:,\s*\d+(?:<[^>]*>)?)?(?:,\s*([^)]*))?\)', text)
                if dup and dup.group(1) in dup_sources:
                    table[ret] = ['dup', name == 'dup3' and 'O_CLOEXEC' in (dup.group(2) or '')]
                    descriptors.append((pid, ret, 'dup', start))
        elif name == 'fcntl' and succeeded:
            dup = re.match(r'^fcntl\((\d+)(?:<[^>]*>)?,\s*(F_DUPFD(?:_CLOEXEC)?)\b', text)
            if dup:
                table.pop(ret, None)
                if dup.group(1) in dup_sources:
                    table[ret] = ['dup', dup.group(2) == 'F_DUPFD_CLOEXEC']
                    descriptors.append((pid, ret, 'dup', start))
            setfd = re.match(r'^fcntl\((\d+)(?:<[^>]*>)?,\s*F_SETFD,\s*(\S+)\)', text)
            if setfd and setfd.group(1) in table:
                table[setfd.group(1)][1] = 'FD_CLOEXEC' in setfd.group(2)
    return DataFds(hits, descriptors)
