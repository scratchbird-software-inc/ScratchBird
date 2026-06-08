# ScratchBird Implementation Root

This directory is the implementation root defined by `public_contract_snapshot`.

Rules:

- Implementation source belongs under `project/`, not the repository root.
- Generated build output belongs under repository-level `build/`, not under source directories.
- ScratchBird-Legacy source is reference-only and must not be copied here without a controlled source-intake execution_plan.
- Engine execution authority is SBLR/internal envelopes only.
- Parser packages are untrusted translation products.
- Private cluster source remains private and must not be included in public/open-source packages.
