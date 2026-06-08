# Parser Bound-AST Model

Minimal bound-AST model and binder for the parser vertical slice.

Current scope:

- binds `ShowIdentityAst` only;
- validates database UUID, principal UUID, and package profile context;
- emits `BoundShowIdentity` with operation key, result shape, diagnostic shape, rights, scope, and trace key.

This is not SBLR lowering or execution authority.
