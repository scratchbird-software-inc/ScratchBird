#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_STRACE_ATTRIBUTION. Self-test of the strace attribution helper (strace_attribution.py) on
hand-built `strace -f -qq` logs. The helper is what every strace-based native test uses to decide "this write
went to the data file", so it must itself be proven in both directions: a pipe that reuses a descriptor number
must NOT be attributed (false failure, seen once under load), and a real data-file write must ALWAYS be caught
(false pass). Cases mirror what `strace -f` really prints, including split `<unfinished ...>` / `resumed` calls."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import strace_attribution as sa  # noqa: E402

P = "/tmp/t/db.sdb"
THREAD = (100, 'clone3({flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_THREAD}, 88) = 101')  # thread 101 of process 100
FD_OPEN = f'openat(AT_FDCWD, "{P}", O_RDWR|O_CLOEXEC) = 4'


def log(*rows):
    return [f"{pid}  {text}\n" for pid, text in rows]


def hits(rows, needle):
    """Indexes of lines containing `needle` that the helper attributes to the data file."""
    lines = log(*rows)
    result = sa.data_descriptors(lines, P)
    return [i for i, line in enumerate(lines) if needle in line and result.covers(i)], lines, result


class Attribution(unittest.TestCase):
    def test_real_pwrite_caught(self):
        got, _, result = hits([(100, FD_OPEN), (100, 'pwrite64(4, "x", 512, 0) = 512'), (100, 'fsync(4) = 0')], "pwrite64")
        self.assertEqual(got, [1])
        self.assertEqual(len(result), 1)

    def test_split_close_then_pipe_reuse_not_attributed(self):
        # The observed flake: thread 101 gets a pipe that reuses 4 while 100's close is still printing as split.
        rows = [THREAD, (100, FD_OPEN), (100, 'pwrite64(4, "x", 512, 0) = 512'),
                (100, 'close(4 <unfinished ...>'),
                (101, 'pipe2([4, 5], O_CLOEXEC) = 0'),
                (100, '<... close resumed>) = 0'),
                (101, 'write(4, "SBACRED1\\1", 9) = 9'), (101, 'write(5, "x", 1) = 1')]
        self.assertEqual(hits(rows, "pwrite64")[0], [2], "the real pwrite on the data fd is caught")
        self.assertEqual(hits(rows, "write(")[0], [], "the pipe writes, numbers 4 and 5, are not the data file")

    def test_split_close_then_pipe_after_resume_not_attributed(self):
        rows = [(100, FD_OPEN), (100, 'close(4 <unfinished ...>'), (100, '<... close resumed>) = 0'),
                (100, 'pipe2([4, 5], 0) = 0'), (100, 'write(4, "SBACRED1", 8) = 8')]
        got, _, _ = hits(rows, "SBACRED1")
        self.assertEqual(got, [])

    def test_close_then_open_other_file_reuse(self):
        rows = [(100, FD_OPEN), (100, 'close(4) = 0'), (100, 'openat(AT_FDCWD, "/etc/other", O_RDONLY) = 4'),
                (100, 'write(4, "y", 1) = 1')]
        self.assertEqual(hits(rows, "write(4")[0], [])

    def test_missed_close_cannot_attribute_a_reused_number(self):
        # No close line at all (e.g. a close_range we do not parse, or a lost line): a new descriptor returned
        # on the same number ends the stale span anyway.
        rows = [(100, FD_OPEN), (100, 'socket(AF_UNIX, SOCK_STREAM, 0) = 4'), (100, 'write(4, "SBACRED1", 8) = 8')]
        self.assertEqual(hits(rows, "SBACRED1")[0], [])

    def test_same_number_in_forked_child_without_the_data_fd(self):
        # The child is forked BEFORE the data file is opened: its table is a copy of the (empty) parent table.
        rows = [(100, 'clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x1) = 200'),
                (100, FD_OPEN), (200, 'pipe2([4, 5], 0) = 0'), (200, 'write(4, "SBACRED1", 8) = 8'),
                (100, 'pwrite64(4, "x", 512, 0) = 512')]
        got, lines, _ = hits(rows, "write(4")
        self.assertEqual(got, [])
        self.assertEqual(hits(rows, "pwrite64")[0], [4])

    def test_forked_child_that_inherited_the_data_fd_is_caught(self):
        rows = [(100, FD_OPEN), (100, 'clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x1) = 200'),
                (200, 'pwrite64(4, "x", 512, 0) = 512'), (200, 'close(4) = 0'),
                (100, 'pwrite64(4, "x", 512, 512) = 512')]
        got, _, _ = hits(rows, "pwrite64")
        self.assertEqual(got, [2, 4], "the child's copy and the parent's own, the child's close not affecting the parent")

    def test_thread_shares_the_table_even_when_its_line_precedes_the_clone_return(self):
        rows = [(100, FD_OPEN),
                (100, 'clone3({flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD, child_tid=0x1}, 88 <unfinished ...>'),
                (101, 'pwrite64(4, "x", 512, 0) = 512'),
                (100, '<... clone3 resumed>) = 101'),
                (100, 'close(4) = 0'),
                (101, 'fsync(4) = -1 EBADF (Bad file descriptor)')]
        got, _, _ = hits(rows, "pwrite64")
        self.assertEqual(got, [2])
        # a thread's open is seen by its siblings, a thread's close by all
        self.assertEqual(hits(rows, "fsync")[0], [], "after the shared close the number is not a data descriptor")

    def test_thread_open_visible_to_parent(self):
        rows = [(100, 'clone(child_stack=0x1, flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_THREAD|CLONE_SIGHAND, parent_tid=[101]) = 101'),
                (101, FD_OPEN), (100, 'pwrite64(4, "x", 512, 0) = 512')]
        self.assertEqual(hits(rows, "pwrite64")[0], [2])

    def test_dup_is_a_data_descriptor_and_split_dup_too(self):
        rows = [THREAD, (100, FD_OPEN), (100, 'fcntl(4, F_DUPFD_CLOEXEC, 3 <unfinished ...>'), (101, 'getpid() = 100'),
                (100, '<... fcntl resumed>) = 7'), (100, 'pwrite64(7, "x", 512, 0) = 512'),
                (100, 'dup(4) = 8'), (100, 'write(8, "x", 1) = 1'), (100, 'dup2(4, 9) = 9'), (100, 'fsync(9) = 0'),
                (100, 'dup3(4, 10, O_CLOEXEC) = 10'), (100, 'fdatasync(10) = 0')]
        lines = log(*rows)
        result = sa.data_descriptors(lines, P)
        self.assertEqual(len(result), 5)
        for needle in ("pwrite64(7", "write(8", "fsync(9", "fdatasync(10"):
            self.assertTrue(any(needle in l and result.covers(i) for i, l in enumerate(lines)), needle)
        self.assertEqual(result.origin(5), "dup")

    def test_dup_of_a_non_data_fd_is_not_one(self):
        rows = [(100, FD_OPEN), (100, 'dup(2) = 7'), (100, 'write(7, "x", 1) = 1')]
        self.assertEqual(hits(rows, "write(7")[0], [])

    def test_dup2_over_the_data_fd_ends_it(self):
        rows = [(100, FD_OPEN), (100, 'dup2(2, 4) = 4'), (100, 'write(4, "x", 1) = 1')]
        self.assertEqual(hits(rows, "write(4")[0], [])

    def test_split_open(self):
        rows = [THREAD, (100, f'openat(AT_FDCWD, "{P}", O_RDWR|O_CLOEXEC <unfinished ...>'), (101, 'getpid() = 1'),
                (100, '<... openat resumed>) = 4'), (100, 'pwrite64(4, "x", 512, 0) = 512')]
        got, _, result = hits(rows, "pwrite64")
        self.assertEqual(got, [4])
        self.assertEqual(len(result), 1)

    def test_split_write_is_attributed_at_its_start_and_merged_for_whole_call_regexes(self):
        rows = [THREAD, (100, FD_OPEN), (100, 'pwrite64(4, "x"..., 512, 0 <unfinished ...>'), (101, 'getpid() = 1'),
                (100, '<... pwrite64 resumed>) = 512')]
        got, lines, _ = hits(rows, "pwrite64")
        self.assertEqual(got, [2])
        merged = sa.merge_split_lines(lines)
        self.assertRegex(merged[2], r'pwrite64\((\d+),.*,\s*(\d+),\s*\d+\)\s*=\s*(\d+)')
        self.assertEqual(merged[4].strip(), "")
        self.assertEqual(len(merged), len(lines))

    def test_exec_drops_cloexec_descriptors_only(self):
        rows = [(100, FD_OPEN), (100, f'openat(AT_FDCWD, "{P}", O_RDWR) = 5'), (100, 'execve("/bin/x", ["x"], 0x1) = 0'),
                (100, 'write(4, "x", 1) = 1'), (100, 'write(5, "x", 1) = 1')]
        lines = log(*rows)
        result = sa.data_descriptors(lines, P)
        self.assertFalse(result.covers(3))
        self.assertTrue(result.covers(4))

    def test_close_range(self):
        rows = [(100, FD_OPEN), (100, 'close_range(3, 4294967295, 0) = 0'), (100, 'write(4, "x", 1) = 1')]
        self.assertEqual(hits(rows, "write(4")[0], [])

    def test_stray_pid_without_clone_record_is_loud(self):
        rows = [(100, FD_OPEN), (300, 'pwrite64(4, "x", 512, 0) = 512')]
        with self.assertRaises(sa.StrayPid) as caught:
            sa.data_descriptors(log(*rows), P)
        self.assertIn("pid 300", str(caught.exception))
        self.assertIn("line 2", str(caught.exception))

    def test_legitimately_cloned_pids_are_not_stray(self):
        rows = [(100, FD_OPEN),
                (100, 'clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x1 <unfinished ...>'),
                (200, 'getpid() = 200'), (100, '<... clone resumed>) = 200'),
                (100, 'vfork() = 300'), (300, 'execve("/bin/x", ["x"], 0x1) = 0'),
                (100, 'clone3({flags=CLONE_VM|CLONE_FILES|CLONE_THREAD}, 88) = 400'), (400, 'getpid() = 100'),
                (200, 'pwrite64(4, "x", 512, 0) = 512')]
        lines = log(*rows)
        self.assertEqual(len(sa.data_descriptors(lines, P)), 1)

    def test_no_open_means_no_descriptors(self):
        result = sa.data_descriptors(log((100, 'write(1, "x", 1) = 1')), P)
        self.assertFalse(result)

    def test_y_style_decoded_descriptors_parse(self):
        rows = [(100, FD_OPEN), (100, f'write(4<{P}>, "x", 1) = 1')]
        self.assertEqual(hits(rows, "write(4<")[0], [1])

    def test_pid_bracket_format(self):
        lines = ['[pid   100] ' + FD_OPEN + '\n', '[pid   100] pwrite64(4, "x", 512, 0) = 512\n']
        self.assertTrue(sa.data_descriptors(lines, P).covers(1))


if __name__ == "__main__":
    unittest.main(verbosity=1)
