# Parser SBLR Lowering

Minimal logical envelope lowerer for the parser vertical slice.

Current scope:

- lowers `BoundShowIdentity` only;
- lowers parser-supplied, already-selected SBLR route descriptors without
  consulting any parser-family catalog;
- emits deterministic logical envelope JSON;
- does not implement binary SBLR encoding;
- does not own grammar-to-operation mapping;
- does not execute or bypass engine-side gates.
