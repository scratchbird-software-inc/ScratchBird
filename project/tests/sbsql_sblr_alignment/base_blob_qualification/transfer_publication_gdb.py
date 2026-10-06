# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Author-side exact-CAS check; run inside GDB on the debug runtime test.

gdb -q -batch -x probes/transfer_publication_gdb.py --args TEST transfers
No production test hook or scheduler timing assumption is used. This is not
an independent source audit or a substitute for the broader concurrency gate.
"""

import gdb


class PublicationBreakpoint(gdb.Breakpoint):
    def __init__(self):
        super().__init__(
            "scratchbird::core::datatypes::BlobRetainedLifetimeLeaseV3::"
            "AtomicControl::TryFinalize", internal=True
        )
        self.counts = {"CloneInto": 0, "AdoptFrom": 0}
        self.failure = None

    def stop(self):
        frame = gdb.newest_frame()
        if int(frame.read_var("final_state")) != 2:  # idle
            return False
        control = frame.read_var("this").dereference()
        word = int(control["word_"]["_M_i"])
        frame = frame.older()
        while frame is not None:
            name = frame.name() or ""
            # Exclude lambda frames: the owning API frame provides operands.
            if "lambda" not in name:
                operation = next(
                    (op for op in self.counts
                     if name.startswith(
                         "scratchbird::core::datatypes::"
                         "BlobRetainedLifetimeLeaseV3::" + op)), None
                )
                if operation is not None:
                    self.counts[operation] += 1
                    if not (word & 16) or ((word >> 25) & 0xffffffff) != 1:
                        self.failure = operation + " attempted idle without sole closed owner"
                        return True
                    if (word & 15) not in (1, 5):
                        self.failure = operation + " attempted idle without active reservation"
                        return True
                    return False
            frame = frame.older()
        return False


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set debuginfod enabled off")
gdb.execute("set print thread-events off")
check = PublicationBreakpoint()
gdb.execute("run")
if check.failure:
    gdb.write("FAIL " + check.failure + "\n")
    gdb.execute("quit 1")
if gdb.inferiors()[0].threads():
    gdb.write("FAIL transfer test stopped before normal exit\n")
    gdb.execute("quit 1")
if int(gdb.parse_and_eval("$_exitcode")) != 0:
    gdb.execute("quit 1")
if check.counts["CloneInto"] < 2 or check.counts["AdoptFrom"] < 6:
    gdb.write("FAIL insufficient exact-publication coverage\n")
    gdb.execute("quit 1")
gdb.write("PASS exact idle publication reservations " + str(check.counts) + "\n")
