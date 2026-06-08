# Fuzz Malicious Input Report

Status: complete
Search key: `FSPE-012B-FUZZ-MALICIOUS-INPUT-REPORT`
Owning slice: `FSPE-012B`

## Summary

FSPE-012B materialized `sbsql_fuzz_malicious_input_gate` as a fast malicious-input and hostile-packet CTest gate.

The gate verifies:

- Unterminated literals, invalid UTF-8, invalid UUID literals, and unknown WAL-prefixed statements fail closed with canonical parser diagnostics.
- Parser resource budgets reject oversized statements, identifiers, literals, token streams, parameter lists, AST depth, and SBLR envelopes.
- Parser-support UDR calls fail closed without trusted engine context or debug decompile policy.
- SBPS decoding rejects truncated frames, bad magic, unknown message types, invalid payload CRCs, and oversized payloads.
- Server SBLR admission rejects raw SQL payloads and malformed parser envelopes before engine dispatch.
- Message-vector diagnostics round trip through SBPS encoding/decoding.

## Gate

| Field | Value |
| --- | --- |
| CTest label | `sbsql_fuzz_malicious_input_gate` |
| Source | `project/tests/sbsql_parser_worker/generated/hardening/sbsql_fuzz_malicious_input_gate.cpp` |
| Corpus | `project/tests/sbsql_parser_worker/generated/hardening/MALICIOUS_INPUT_FIXTURES.csv` |
| Unexpected failures | 0 |

## Coverage

The corpus includes lexer/CST/AST, binder/lowering, UDR, SBPS packet, message-vector, and full-route client input routes. The gate intentionally remains a fast CTest smoke gate; extended fuzz expansion remains outside the fast validation label.

## Result

Malicious parser, UDR, server-admission, SBPS, and message-vector inputs fail closed with canonical diagnostics and without granting parser-side authority.
