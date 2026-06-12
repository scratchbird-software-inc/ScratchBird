# Compatibility Parsers

Search key: `P6_COMPAT_COMPATIBILITY_PROFILE_MANIFEST`

Compatibility parser families map compatibility syntax and behavior to ScratchBird authority rows or refusal rows. Each compatibility parser must be independently installable.

`CompatibilityProfileManifest.csv` is the P6 admission manifest for public compatibility compatibility profiles. The manifest records parser placement, source/reference evidence, seed-manifest authority, wire/API profile, datatype/index/diagnostic/metadata/migration/sandbox/builtin coverage, and the rule that compatibility engines never own ScratchBird SQL execution, storage, recovery, security, or MGA transaction finality.
