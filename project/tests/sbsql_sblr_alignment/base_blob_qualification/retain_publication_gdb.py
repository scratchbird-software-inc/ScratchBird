# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Pause a real second Retain caller through retain/probe publication.

gdb -q -batch -x probes/retain_publication_gdb.py --args TEST retain_publication
Only a test rendezvous flag is written; no engine state or counters are forged.
"""
import gdb

gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set debuginfod enabled off')
gdb.execute('set print thread-events off')
prefix='scratchbird::core::datatypes::BlobRetainedLifetimeLeaseV3::'
local=gdb.Breakpoint(prefix+'LocalStateFailure', internal=True)
wait=gdb.Breakpoint(prefix+'AtomicControl::WaitForClosingQuiescence', internal=True)
returned=gdb.Breakpoint('RetainContenderReturned', internal=True)
wait.enabled=returned.enabled=False

def has_frame(thread, fragment):
    thread.switch()
    frame=gdb.newest_frame()
    while frame:
        if fragment in (frame.name() or ''): return True
        frame=frame.older()
    return False

gdb.execute('run')
for case in range(2):
    assert 'LocalStateFailure' in (gdb.newest_frame().name() or '')
    contender=gdb.selected_thread()
    owner=next(t for t in gdb.inferiors()[0].threads()
               if t!=contender and has_frame(t,'RetainPublicationOwner'))
    owner.switch()
    gdb.execute('set scheduler-locking on')
    gdb.execute("set variable *((unsigned char*)&('(anonymous namespace)::g_retain_debug'->fixture->fake.finish_clock)) = 1")
    local.enabled=False
    wait.enabled=True
    gdb.execute('continue')
    assert 'WaitForClosingQuiescence' in (gdb.newest_frame().name() or '')
    word=int(gdb.parse_and_eval('this->word_._M_i'))
    assert word & 16 and ((word >> 25) & 0xffffffff)==2
    wait.enabled=False
    contender.switch()
    returned.enabled=True
    gdb.execute('continue')
    assert 'RetainContenderReturned' in (gdb.newest_frame().name() or '')
    returned.enabled=False
    local.enabled=True
    gdb.execute('set scheduler-locking off')
    gdb.execute('continue')
assert not gdb.inferiors()[0].threads()
assert int(gdb.parse_and_eval('$_exitcode'))==0
gdb.write('PASS retain/probe keep successful ticket through metadata-only contender\n')
