# Donor Parsers

Search key: `P6_DONOR_COMPATIBILITY_PROFILE_MANIFEST`

Donor parser families map donor syntax and behavior to ScratchBird authority rows or refusal rows. Each donor parser must be independently installable.

`DonorCompatibilityProfileManifest.csv` is the P6 admission manifest for public donor compatibility profiles. The manifest records parser placement, source/reference evidence, seed-manifest authority, wire/API profile, datatype/index/diagnostic/metadata/migration/sandbox/builtin coverage, and the rule that donor engines never own ScratchBird SQL execution, storage, recovery, security, or MGA transaction finality.
