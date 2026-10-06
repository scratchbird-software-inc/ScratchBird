#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
import json,re,sys
from pathlib import Path
BASE=Path(__file__).resolve().parents[4]
SRC=(BASE/'project/src/core/datatypes/datatype_blob.cpp').read_text()
import argparse
p=argparse.ArgumentParser(description="Compare runtime invariant masks against supplied Core authority")
p.add_argument("core", type=Path)
CORE=p.parse_args().core
CONTRACT=CORE/'registries/base-blob-lifetime-runtime-contract.yaml'
VECTORS=CORE/'conformance_manifests/base-blob-lifetime-runtime-vectors.yaml'
# Avoid a YAML dependency for the contract: each normative whitelist row has the
# four numeric fields in a fixed compact block.
ct=CONTRACT.read_text()
rows=[]
pat=re.compile(r'- event_token:.*?event_numeric: (\d+).*?operation_token:.*?operation_numeric: (\d+).*?phase_token:.*?phase_numeric: (\d+)',re.S)
for m in pat.finditer(ct[ct.index('event_operation_phase_whitelist:'):ct.index('callback_budget:')]):
    rows.append(tuple(map(int,m.groups())))
allowed=set(rows)
mask_text=SRC[SRC.index('kInvariantPhaseMaskByEventAndOperationV7'):SRC.index('static_assert([]() constexpr',SRC.index('kInvariantPhaseMaskByEventAndOperationV7'))]
vals=[int(x,16) for x in re.findall(r'UINT16_C\(0x([0-9a-fA-F]{4})\)',mask_text)]
assert len(vals)==13*21, len(vals)
mask_allowed={(e+1,o+1,p+1) for e in range(13) for o in range(21) for p in range(14) if vals[e*21+o]&(1<<p)}
assert len(rows)==647, len(rows)
assert len(allowed)==647, len(allowed)
assert mask_allowed==allowed, (len(mask_allowed-allowed),len(allowed-mask_allowed))

def accepted(f):
    e=f['event_numeric']; o=f['operation_numeric']; p=f['phase_numeric']; c=f['callback_code_or_255']
    domain=(1<=e<=13 and 1<=o<=21 and 1<=p<=14 and (0<=c<=18 or c==255))
    bindings=(f.get('authority_instance_uuid_hex','01'+'00'*15)!='0'*32 and
              f.get('lifetime_token_uuid_hex','02'+'00'*15)!='0'*32 and
              f.get('authority_instance_generation',1)!=0 and
              f.get('lifetime_token_generation',1)!=0 and
              f.get('immutable_binding_generation',1)!=0)
    return domain and bindings and (e,o,p) in mask_allowed
v=json.loads(VECTORS.read_text())
illegal=v['v7_invariant_illegal_domain_cases']
wrong=[x['case_id'] for x in illegal if accepted(x['factors'])]
assert len(illegal)==4136,len(illegal)
assert not wrong,wrong[:10]
# Every normative allowed triple is independently exercised for all legal callback
# domain values here; callback code is an independent scalar domain.
for triple in allowed:
    for c in list(range(19))+[255]:
        e,o,p=triple
        assert accepted({'event_numeric':e,'operation_numeric':o,'phase_numeric':p,
          'callback_code_or_255':c,'authority_instance_uuid_hex':'01'+'00'*15,
          'lifetime_token_uuid_hex':'02'+'00'*15,'authority_instance_generation':1,
          'lifetime_token_generation':1,'immutable_binding_generation':1})
print('authority_contract_rows=647')
print('implementation_mask_cells=273')
print('implementation_allowed_triples=647')
print('allowed_callback_domain_checks=12940')
print('illegal_vector_checks=4136')
print('result=PASS exact whitelist, callback domain, and binding scalar domain')
