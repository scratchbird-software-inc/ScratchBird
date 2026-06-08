# Parser AST Model

Minimal parser AST model for the parser vertical slice.

Current scope:

- syntax-only AST header;
- `ShowIdentityAst` for identity/session `SHOW` forms;
- deterministic JSON serialization for conformance evidence.

This is not binding, lowering, security authority, or executor authority.
