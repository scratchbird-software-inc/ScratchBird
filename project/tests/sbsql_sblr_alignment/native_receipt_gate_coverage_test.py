#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
#
# SEARCH_KEY: SB_NATIVE_RECEIPT_GATE_COVERAGE (S4f exposure close).
# Owner direction (2026-10-19 / 2026-11-16): the database is ONE file; no other files are created; legacy ".sb.*" side files are drift.
# A native statement-context receipt is consumed by about 65 entry points of the server-engine bridge, and most of them reach a side-file
# family. The engine gates them at ONE helper (native_receipt_consumer_gate / native_session_consumer_gate in engine/public_abi.cpp) with
# an explicit allow-list table. This is the SOURCE-POLICY half; it fails when a consumer escapes the gate:
#
#   1. CONSUMER DISCOVERY (signature based, not name-prefix based). Parsed with comments stripped:
#        a. every function DECLARED in server_engine_bridge/statement_context.hpp / prepared_metadata_binding.hpp whose parameters
#           include a StatementContextReceiptHandle BY VALUE, or a pointer to a request struct that has a StatementContextReceiptHandle
#           member, or an sb_engine_session_t / a request struct with an engine_session member;
#        b. every function DEFINED in engine/public_abi.cpp with such a parameter, or named Bind*/Coordinate*/Issue*/Negotiate*.
#      The exports map (sbcore.exports.map = sb_engine_*) is NOT used: the C ABI takes no receipt; the consumers are the bridge.
#   2. EXEMPT (by name, with the reason; a new function is never exempt by default): the functions that CREATE a handle or only read a
#      result / an already-issued handle, and the private handle of a gated begin.
#   3. EVERY consumer is a row of kNativeReceiptConsumerTable (between the NATIVE_RECEIPT_CONSUMER_TABLE markers), and every row is a
#      discovered consumer. The ALLOW-LIST (rows that are not `refuse`) is PINNED here: wrongly allow-listing an entry fails this test.
#   4. Every `refuse` row's definition calls the gate as its FIRST consumer-visible statement: before any use of
#      g_live_statement_context_receipts and before any return (except the session validity check of a session-keyed entry), with its OWN
#      name as the entry literal. An `own_gate` row must contain its own native refusal (token pinned below).
#   5. The runtime test's table (native_receipt_gate_test.cpp) lists every refuse row with the SAME family and requires token.
#
# Usage: native_receipt_gate_coverage_test.py <project-root>      (exit 0 = pass)
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."

# ---- pinned data ------------------------------------------------------------------------------------------------------------
# S4g part 1: CopyStatementNameResolveAuthorityV1 joins the allowed set (it reads the receipt's in-memory bind authority; no file, no registry),
# beside the NAME_RESOLVE availability row, so the bound descriptor can be read back natively.
# S4g part 2 (owner decision 2026-12-06, after an end-to-end audit): AcquireStatementPackageAdmissionReservation joins the allowed set (OLD: refused,
# native_package_admission_registry): its body reads only the receipt and the session's in-memory resource ledger and records the reservation in a
# process-local map; no file, no registry, no storage. Dispatch of the allow-listed envelope needs it.
# S4h part 1 (real parser route): NegotiateStatementLiteralDescriptorsV1 and FinalizeStatementLiteralBindingV1 join the allowed set (OLD: refused,
# native_literal_descriptor_registry): they read only the receipt's in-memory state and the compiled datatype table; no file, no registry, no storage.
PINNED_ALLOWED = {"AcquireStatementContextReceipt", "ReleaseStatementContextReceipt", "CopyStatementContextEngineContextV1", "CopyStatementNameResolveAuthorityV1",
                  "AcquireStatementPackageAdmissionReservation", "NegotiateStatementLiteralDescriptorsV1", "FinalizeStatementLiteralBindingV1"}
PINNED_OWN_GATE = {
    "DispatchStatementContextReceipt": "native_operation_not_built(native_session, out_result, \"dispatch_statement_context\"",
    "BindStatementNameResolveAuthorityV1": "context.native_served_facts && view.name_resolve_executor_availability_generation == 0",
    "BeginStatementParameterExecutionCoordinationV1": "native_operation_not_built(request->engine_session, out_result, \"parameter_coordination_begin\"",
}
EXEMPT = {
    "OpenServedEngine": "creates the engine handle (no receipt, no session yet)",
    "SealPreparedStatementParameterTemplateV1": "takes the private handle of BeginStatementParameterExecutionCoordinationV1, which refuses a native session: no native path makes one",
    "ReadStatementQueryExecuteResultHandle": "reads an already-issued result handle (memory only)",
    "ReadStatementCatalogIntrospectResultHandle": "reads an already-issued result handle (memory only)",
    "ReadStatementResultPageMaterial": "reads an already-issued result (memory only)",
    "ReadStatementQueryExplainMaterial": "reads an already-issued result (memory only)",
    "ReleaseStatementPackageAdmissionReservation": "releases a handle that only the gated Acquire can issue",
    "ReleasePreparedMetadataBinding": "releases a handle that only the gated Create can issue",
    "SetPreparedMetadataBindingDispatchTestHookForTesting": "a test hook, not a consumer",
    "CopyEngineDiagnosticSnapshot": "reads an already-issued result's diagnostic (memory only)",
}
failures = []


def fail(message):
    failures.append(message)


def read(rel):
    with open(f"{ROOT}/{rel}", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append("\n")
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def split_params(text):
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch in "<(":
            depth += 1
        if ch in ">)":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    parts.append(cur.strip())
    return [" ".join(p.split()) for p in parts if p.strip()]


# ---- 1a. declarations -------------------------------------------------------------------------------------------------------
context_hpp = strip_comments(read("src/server_engine_bridge/statement_context.hpp"))
binding_hpp = strip_comments(read("src/server_engine_bridge/prepared_metadata_binding.hpp"))
structs_with_receipt, structs_with_session = set(), set()
for m in re.finditer(r"\nstruct\s+(\w+)\s*\{(.*?)\n\};", context_hpp, re.S):
    if re.search(r"\bStatementContextReceiptHandle\s+receipt\b", m.group(2)):
        structs_with_receipt.add(m.group(1))
    if re.search(r"\bsb_engine_session_t\s+engine_session\b", m.group(2)):
        structs_with_session.add(m.group(1))


def classify(params):
    for p in params:
        if re.match(r"StatementContextReceiptHandle\s+\w+$", p):
            return "receipt"
        mm = re.match(r"(?:const\s+)?(?:\w+::)*(\w+)\s*\*\s*\w+$", p)
        if mm and mm.group(1) in structs_with_receipt:
            return "request->receipt"
        if mm and mm.group(1) in structs_with_session:
            return "request->engine_session"
        if re.match(r"sb_engine_session_t\s+\w+$", p):
            return "session"
    return None


declared = {}
for text in (context_hpp, binding_hpp):
    for m in re.finditer(r"\n(?:sb_engine_status_t|bool|void)\s+([A-Z][A-Za-z0-9_]*)\(([^;{]*?)\)\s*;", text):
        declared[m.group(1)] = classify(split_params(m.group(2)))
consumers = {n for n, k in declared.items() if k}
for n in list(declared):
    if n in EXEMPT:
        consumers.discard(n)
if len(consumers) < 60:
    fail(f"only {len(consumers)} consumers were discovered in the headers: the discovery is broken")

# ---- 1b. definitions in public_abi.cpp --------------------------------------------------------------------------------------
abi = read("src/engine/public_abi.cpp")
abi_code = strip_comments(abi)
definitions = {}
for m in re.finditer(r"\n(?:sb_engine_status_t|bool)\s+([A-Z][A-Za-z0-9_]*)\(([^;{]*?)\)\s*\{", abi_code):
    definitions.setdefault(m.group(1), []).append((m.start(), split_params(m.group(2))))
for name, defs in definitions.items():
    for _, params in defs:
        prefix = re.match(r"(Bind|Coordinate|Issue|Negotiate)[A-Z]", name)
        if (classify(params) or prefix) and name not in EXEMPT and name not in declared:
            consumers.add(name)  # defined here, consumer-shaped, but not declared in the headers
            fail(f"{name}: a consumer-shaped function defined in public_abi.cpp that the bridge headers do not declare")

# ---- 3. the engine's table --------------------------------------------------------------------------------------------------
begin = abi.index("NATIVE_RECEIPT_CONSUMER_TABLE_BEGIN")
end = abi.index("NATIVE_RECEIPT_CONSUMER_TABLE_END")
rows = {}
for m in re.finditer(r'\{"(\w+)", "(\w+)", "(\w*)", (kRefuse|kAllowed|kOwnGate)\}', abi[begin:end]):
    if m.group(1) in rows:
        fail(f"{m.group(1)}: listed twice in kNativeReceiptConsumerTable")
    rows[m.group(1)] = (m.group(2), m.group(3), m.group(4))
for n in sorted(consumers - set(rows)):
    fail(f"{n}: a receipt/session consumer that is NOT in kNativeReceiptConsumerTable (a native receipt could reach it ungated)")
for n in sorted(set(rows) - consumers):
    fail(f"{n}: a row of kNativeReceiptConsumerTable that is not a discovered consumer")
allowed = {n for n, r in rows.items() if r[2] == "kAllowed"}
own = {n for n, r in rows.items() if r[2] == "kOwnGate"}
if allowed != PINNED_ALLOWED:
    fail(f"the ALLOWED set is {sorted(allowed)}; it is pinned to {sorted(PINNED_ALLOWED)} (the owner enables an entry only after its native storage exists: edit this pin with the table)")
if own != set(PINNED_OWN_GATE):
    fail(f"the OWN_GATE set is {sorted(own)}; it is pinned to {sorted(PINNED_OWN_GATE)}")
for n, r in rows.items():
    if r[2] == "kRefuse" and not r[1]:
        fail(f"{n}: a refused row without a requires token")

# ---- 4. the gate call in every definition -----------------------------------------------------------------------------------
def body_of(name):
    defs = [m for m in re.finditer(r"\nsb_engine_status_t " + re.escape(name) + r"\(", abi_code)]
    if len(defs) != 1:
        return None
    start = defs[0].start()
    # the signature ends at the first ") {" ; the body ends at the first line that starts with "}"
    sig_end = abi_code.index("{", abi_code.index(")", start))
    stop = abi_code.index("\n}", sig_end)
    return abi_code[sig_end + 1:stop]


for name, (family, token, disp) in sorted(rows.items()):
    body = body_of(name)
    if body is None:
        fail(f"{name}: expected exactly one definition in public_abi.cpp")
        continue
    if disp == "kRefuse":
        gate = re.search(r'native_(?:receipt|session)_consumer_gate\("' + re.escape(name) + r'",', body)
        if not gate:
            fail(f"{name}: the definition does not call the consumer gate with its own name")
            continue
        before = body[:body.rfind("if (const auto native_gate", 0, gate.start())]
        if "g_live_statement_context_receipts" in before:
            fail(f"{name}: the receipt registry is used BEFORE the consumer gate")
        is_session = "native_session_consumer_gate" in gate.group(0)
        if not is_session:
            statements = [s.strip() for s in re.split(r"[;{}]", before) if s.strip()]
            # allowed before the gate: clear_result and a single-line output reset
            for s in statements:
                if not (s == "clear_result(out_result)" or re.match(r"if \(out_\w+( != nullptr)?\) .*", s)):
                    fail(f"{name}: a statement precedes the consumer gate: {s[:80]!r}")
        else:
            if "valid_session(session)" not in before:
                fail(f"{name}: the session-keyed gate must follow the entry's own valid_session check")
        if not re.search(r"\)\s*return \*native_gate;", body[gate.start():gate.start() + 400]):
            fail(f"{name}: the gate's result is not returned")
    elif disp == "kOwnGate":
        if PINNED_OWN_GATE[name] not in body:
            fail(f"{name}: its own native gate ({PINNED_OWN_GATE[name]!r}) is missing from the body")
        if "native_receipt_consumer_gate(" in body:
            fail(f"{name}: an own_gate entry must not also call the consumer gate (a double gate hides which one answers)")

# the gate's own contract
helper = abi_code[abi_code.index("std::optional<sb_engine_status_t> native_receipt_consumer_decide("):]
helper = helper[:helper.index("\n}\n")]
for needle in ("if (!facts) return std::nullopt;", "row == nullptr", "native_receipt_consumer_unlisted", "NativeReceiptConsumerDisposition::refuse"):
    if needle not in helper:
        fail(f"native_receipt_consumer_decide lost: {needle}")

# ---- 5. the runtime test's table --------------------------------------------------------------------------------------------
test = read("tests/sbsql_sblr_alignment/native_receipt_gate_test.cpp")
test_rows = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'\{"(\w+)", "(\w+)", "(\w+)", \[\]\(', test)}
for name, (family, token, disp) in sorted(rows.items()):
    if disp != "kRefuse":
        continue
    if name in ("CreatePreparedMetadataBinding", "DispatchWithPreparedMetadataBinding"):
        if f'"{name}"' not in test or f'"{token}"' not in test:
            fail(f"{name}: not exercised (with its requires token) by native_receipt_gate_test.cpp")
        continue
    if name not in test_rows:
        fail(f"{name}: missing from the runtime test's table")
    elif test_rows[name] != (family, token):
        fail(f"{name}: the runtime test pins {test_rows[name]} but the engine's table says {(family, token)}")
for name in sorted(set(test_rows) - set(rows)):
    fail(f"{name}: in the runtime test's table but not in the engine's")
for name in own | allowed:
    if f"bridge::{name}" not in test and name not in ("AcquireStatementContextReceipt", "ReleaseStatementContextReceipt"):
        fail(f"{name}: an allowed / own-gate entry that the runtime test does not call")

if failures:
    for f in failures:
        print("FAIL:", f)
    print(f"native_receipt_gate_coverage: {len(failures)} failure(s)")
    sys.exit(1)
print(f"native_receipt_gate_coverage: {len(consumers)} consumers discovered; {len(rows)} table rows "
      f"({len(allowed)} allowed, {len(own)} own gate, {len(rows) - len(allowed) - len(own)} refused); every refused row calls the gate first; "
      f"the runtime test pins {len(test_rows)} rows")
