# Compatibility Parsers

Search key: `P6_COMPAT_COMPATIBILITY_PROFILE_MANIFEST`

Compatibility parser families map compatibility syntax and behavior to
ScratchBird authority rows or refusal rows. Each compatibility parser must be
independently installable.

`CompatibilityProfileManifest.csv` is the P6 admission manifest for public
compatibility profiles. The manifest records parser placement, reference
evidence, seed-manifest authority, wire/API profile,
datatype/index/diagnostic/metadata/migration/sandbox/builtin coverage, and the
rule that compatibility profiles never own ScratchBird SQL execution, storage,
recovery, security, or MGA transaction finality.

Current public beta parser status is documented in
`docs/compatibility-parsers/README.md`. The public release claim is bounded:
the 25 compatibility parser lanes have gate evidence that every surfaced beta
parser operation is mapped to ScratchBird authority, emulated through a
controlled ScratchBird route, handled as parser-only presentation, or refused
with a deterministic diagnostic. This is not a production or drop-in
compatibility claim.
