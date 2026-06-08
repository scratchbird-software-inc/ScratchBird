# Donor Low-Level Utility Unsupported Policy

This tracked test contract records the donor-parser authority rule for
database repair, verification, compaction, checkpoint, native service-tool, and
similar low-level utility surfaces.

Donor parsers must not perform or route database repair, database verification,
physical checkpoint, vacuum/compact/check/analyze/reindex table repair, donor
native service-tool execution, or other low-level database utility authority.
Those surfaces are SBsql-only ScratchBird management/lifecycle authority.

Required donor-parser behavior:

- the parser recognizes the donor surface only far enough to classify it;
- the parser returns a fail-closed unsupported-denied message vector;
- the diagnostic code is `<DONOR>.AUTHORITY.UNSUPPORTED_DENIED`;
- no parser-support UDR route is used for the utility;
- no ScratchBird lifecycle repair/verify API is reached from donor syntax;
- no SBLR operation for repair, verify, checkpoint, or low-level utility work is
  emitted by the donor parser;
- the donor engine is not executed;
- real donor file effects remain false;
- MGA transaction and recovery authority remain engine-owned.

SBsql may expose ScratchBird-native verification, repair planning, checkpoint,
and lifecycle operations. Donor parsers must not expose those operations through
donor-compatible utility syntax, because that would make donor grammar appear to
own ScratchBird storage/recovery authority.

Search key: `DONOR_LOW_LEVEL_UTILITY_UNSUPPORTED_DENIED_POLICY`.
