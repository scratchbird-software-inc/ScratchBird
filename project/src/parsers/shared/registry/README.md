# Parser Registry Snapshot

This component provides a shared, deterministic registry snapshot reader for parser tooling.

It is deliberately evidence-oriented and not execution-oriented:

- It reads caller-supplied registry files.
- It extracts command surface fields used by parser generation and conformance tooling.
- It computes a stable snapshot hash over the profile, file identities, file contents, and extracted entries.
- It reports missing files, missing search keys, missing operation keys, and duplicate search keys.
- It does not execute SBLR, call the engine, or treat registry text as runtime authority.

Runtime authority remains the engine-owned catalog, security policy, transaction state, and cluster decision proof chain.
