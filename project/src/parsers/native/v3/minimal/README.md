# SBsql Minimal Parser

Initial vertical-slice SBsql parser.

Current scope:

- recognizes `SHOW VERSION`;
- recognizes `SHOW DATABASE`;
- emits syntax-only `ShowIdentityAst`;
- emits deterministic parser diagnostics for unsupported commands.

No binding, SBLR lowering, execution, donor syntax, management syntax, or cluster syntax is implemented here.
