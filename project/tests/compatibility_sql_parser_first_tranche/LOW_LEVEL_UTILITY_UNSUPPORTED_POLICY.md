# Compatibility Low-Level Utility Unsupported Policy

This tracked test contract records the compatibility-parser authority rule for
database repair, verification, compaction, checkpoint, native service-tool, and
similar low-level utility surfaces.

Compatibility parsers must not perform or route database repair, database verification,
physical checkpoint, vacuum/compact/check/analyze/reindex table repair, compatibility
native service-tool execution, or other low-level database utility authority.
Those surfaces are SBsql-only ScratchBird management/lifecycle authority.

Required compatibility-parser behavior:

- the parser recognizes the compatibility surface only far enough to classify it;
- the parser returns a fail-closed unsupported-denied message vector;
- the diagnostic code is `<COMPATIBILITY>.AUTHORITY.UNSUPPORTED_DENIED`;
- no parser-support UDR route is used for the utility;
- no ScratchBird lifecycle repair/verify API is reached from compatibility syntax;
- no SBLR operation for repair, verify, checkpoint, or low-level utility work is
  emitted by the compatibility parser;
- the compatibility engine is not executed;
- real compatibility file effects remain false;
- MGA transaction and recovery authority remain engine-owned.

SBsql may expose ScratchBird-native verification, repair planning, checkpoint,
and lifecycle operations. Compatibility parsers must not expose those operations through
compatibility-compatible utility syntax, because that would make compatibility grammar appear to
own ScratchBird storage/recovery authority.

Search key: `COMPATIBILITY_LOW_LEVEL_UTILITY_UNSUPPORTED_DENIED_POLICY`.
