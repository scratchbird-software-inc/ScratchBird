# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Deterministically interleave both real transfer claims with Probe(second).

gdb -q -batch -x probes/transfer_admission_gdb.py --args TEST active_second
The only external writes release test rendezvous flags, never engine state.
"""
from pathlib import Path
import gdb

gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set debuginfod enabled off")
gdb.execute("set print thread-events off")
source = Path(__file__).resolve().parents[4] / "project/src/core/datatypes/datatype_blob.cpp"
lines = [n for n, line in enumerate(source.read_text().splitlines(), 1)
         if "const auto second_claim = claim(second);" in line]
assert len(lines) == 2
claims = [gdb.Breakpoint(str(source) + ":" + str(n), internal=True) for n in lines]
clock = gdb.Breakpoint("ReadClock", internal=True)
owner_return = gdb.Breakpoint("PairOwnerReturned", internal=True)
probe_return = gdb.Breakpoint("PairProbeReturned", internal=True)
clock.enabled = owner_return.enabled = probe_return.enabled = False

def frames(thread):
    thread.switch()
    frame = gdb.newest_frame()
    while frame:
        yield frame.name() or ""
        frame = frame.older()

def set_flag(expr):
    expr = expr.replace("g_pair_debug", "'(anonymous namespace)::g_pair_debug'")
    gdb.execute("set variable *((unsigned char*)&(" + expr + ")) = 1")

def require_frame(name):
    assert name in (gdb.newest_frame().name() or ""), gdb.newest_frame().name()

gdb.execute("run")
for case in range(4):
    owner = gdb.selected_thread()
    assert "operator()" not in (gdb.newest_frame().name() or "")
    assert int(gdb.parse_and_eval("first_claim.status")) == 0
    for point in claims:
        point.enabled = False
    probe = next(thread for thread in gdb.inferiors()[0].threads()
                 if thread != owner and any("PairProbeWorker" in n for n in frames(thread)))
    probe.switch()
    gdb.execute("set scheduler-locking on")
    set_flag("g_pair_debug->start_probe")
    clock.enabled = True
    gdb.execute("continue")
    require_frame("ReadClock")
    clock.enabled = False
    owner.switch()
    owner_return.enabled = True
    gdb.execute("continue")
    require_frame("PairOwnerReturned")
    owner_return.enabled = False
    set_flag("g_pair_debug->second_fake->finish_clock")
    probe.switch()
    probe_return.enabled = True
    gdb.execute("continue")
    require_frame("PairProbeReturned")
    probe_return.enabled = False
    for point in claims:
        point.enabled = True
    gdb.execute("set scheduler-locking off")
    gdb.execute("continue")
assert not gdb.inferiors()[0].threads(), "test did not exit"
assert int(gdb.parse_and_eval("$_exitcode")) == 0
gdb.write("PASS four active-second-claim interleavings, both address orders\n")
