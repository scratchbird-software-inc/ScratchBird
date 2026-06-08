# Parser SBLR Lowering

Minimal logical envelope lowerer for the parser vertical slice.

Current scope:

- lowers `BoundShowIdentity` only;
- emits deterministic logical envelope JSON;
- does not implement binary SBLR encoding;
- does not execute or bypass engine-side gates.
