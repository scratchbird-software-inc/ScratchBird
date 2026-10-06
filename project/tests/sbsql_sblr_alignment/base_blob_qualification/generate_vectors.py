#!/usr/bin/env python3
import json, yaml
from pathlib import Path
import argparse
p=argparse.ArgumentParser(description="Regenerate test vectors from a supplied authoritative Core corpus")
p.add_argument("core", type=Path)
a=p.parse_args()
root=a.core
runtime=json.loads((root/"conformance_manifests/base-blob-lifetime-runtime-vectors.yaml").read_text())
contract=yaml.safe_load((root/"registries/base-blob-lifetime-runtime-contract.yaml").read_text())
out=Path(__file__).resolve().parents[1]/"base_blob_lifetime_v7_generated_vectors.inc"
allowed=contract["private_receiver_services"]["invariant_domain_rules"]["event_operation_phase_whitelist"]
illegal=runtime["v7_invariant_illegal_domain_cases"]
malformed=runtime["malformed_result_cases"]
assert len(allowed)==647 and len(illegal)==4136 and len(malformed)==110
phase_map={"retain":1,"probe":2,"begin_access":3,"end_access":4,"release":5}
stimuli=["code_19","code_255",*[f"reserved_byte_{i}" for i in range(1,8)],"malformed_ok_phase_ticket","dirty_non_ok_phase_ticket"]
stim_map={s:i+1 for i,s in enumerate(stimuli)}
mutation_map={"invalid_binding_or_generation":0,"illegal_triple":0,"illegal_callback_code":0,"unknown_event":0,"unknown_operation":0,"unknown_phase":0}
lines=["// Copyright (c) 2026 ScratchBird Software Inc.", "// SPDX-License-Identifier: MPL-2.0", "// Generated lifetime contract vectors. Do not hand edit.",
"struct InvariantDomainRow { std::uint8_t event, operation, phase, callback_code, mutation; };",
f"constexpr std::array<InvariantDomainRow, {len(allowed)}> kAllowedInvariantRows{{{{"]
for r in allowed:
 lines.append(f"  {{{r['event_numeric']},{r['operation_numeric']},{r['phase_numeric']},255,0}},")
lines.append("}};")
lines.append(f"constexpr std::array<InvariantDomainRow, {len(illegal)}> kIllegalInvariantRows{{{{")
for r in illegal:
 f=r["factors"]; mut=0
 cid=r["case_id"]
 if cid.endswith("NIL-AUTHORITY-UUID"): mut=1
 elif cid.endswith("NIL-LIFETIME-UUID"): mut=2
 elif cid.endswith("ZERO-AUTHORITY-GENERATION"): mut=3
 elif cid.endswith("ZERO-LIFETIME-GENERATION"): mut=4
 elif cid.endswith("ZERO-BINDING-GENERATION"): mut=5
 lines.append(f"  {{{f['event_numeric']},{f['operation_numeric']},{f['phase_numeric']},{f['callback_code_or_255']},{mut}}},")
lines.append("}};")
lines.append("struct MalformedRow { std::string_view case_id; std::uint8_t phase, stimulus; std::uint64_t limit; std::uint8_t retain, probe, begin, end, release, invariants, expected_event, expected_invariant_phase, expected_invariant_callback; std::string_view expected_return, expected_diagnostic, expected_reason, expected_process_termination; };")
lines.append(f"constexpr std::array<MalformedRow, {len(malformed)}> kMalformedRows{{{{")
for r in malformed:
 f=r["factors"]; counts=r["stages"][-1]["cumulative_counts"]
 # The public primary token is not always the private invariant event token.
 # Event 5 is legal only for a newly returned retain/begin ticket. Mutation of
 # an inout/const phase ticket in probe/end/release is dirty_failure_output.
 if f["stimulus"] in ("code_19", "code_255") or f["stimulus"].startswith("reserved_byte_"):
  event=3
 elif f["stimulus"] == "malformed_ok_phase_ticket" and f["phase"] in ("retain", "begin_access"):
  event=5
 else:
  event=4
 reason_token=r["expected"]["diagnostic_parameters"]["ordered_parameters"][-1]["token"]
 invariant_phase={"retain":2,"probe":3,"begin_access":4,"end_access":6,"release":7}[f["phase"]]
 invariant_callback=255 if f["stimulus"] in ("code_19","code_255") else (18 if f["stimulus"] == "dirty_non_ok_phase_ticket" else 0)
 lines.append("  {\"%s\",%d,%d,UINT64_C(%d),%d,%d,%d,%d,%d,%d,%d,%d,%d,\"%s\",\"%s\",\"%s\",\"%s\"}," % (
   r["case_id"],
   phase_map[f["phase"]],stim_map[f["stimulus"]],r["pre"]["ledgers"]["ledger0"]["limit_u64"],
   counts["retain"],counts["probe"],counts["begin_access"],counts["end_access"],counts["release"],counts["invariant_service"],event,invariant_phase,invariant_callback,
   r["expected"]["return_status"],r["expected"]["diagnostic_code"],reason_token,r["expected"]["process_termination"]))
lines.append("}};")
out.write_text("\n".join(lines)+"\n")
