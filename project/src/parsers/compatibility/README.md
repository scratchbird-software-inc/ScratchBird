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

The last verified beta parser status before the SBLR stabilization hold is
documented in `docs/compatibility-parsers/README.md`. That historical claim is
bounded: the 25 compatibility parser lanes have gate evidence that every surfaced beta
parser operation is mapped to ScratchBird authority, emulated through a
controlled ScratchBird route, handled as parser-only presentation, or refused
with a deterministic diagnostic. This is not a production or drop-in
compatibility claim and does not establish conformance to the in-progress SBLR
baseline.

## Build gate during SBLR stabilization

Compatibility parser source, manifests, and historical evidence remain in the
repository, but compatibility parser workers, same-family parser-support UDRs,
and executable compatibility-parser tests are excluded from normal builds.
Only the SBsql parser is enabled by the standard release and full-test profiles
while the shared SBLR boundary is being expanded and stabilized.

Re-enable the compatibility lanes explicitly with
`-DSB_BUILD_COMPATIBILITY_PARSERS=ON`. Per-family options and
`SB_BUILD_STANDALONE_COMPATIBILITY_PARSER_PACKAGES` are evaluated only inside
that umbrella. Re-enabling the umbrella requires a clean compatibility build
and test review against the then-current SBLR contract.
