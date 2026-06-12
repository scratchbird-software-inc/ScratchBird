# Resources

Versioned product resource packs: catalog seeds, reference seeds, charset, collation, timezone, diagnostics, policies, and provider profiles.

## Create-time resource rule

Normal database creation must consume a private resource seed pack that contains the required timezone, character set, collation, locale/calendar, and diagnostic/message resources.

The engine must not depend on the frozen legacy ScratchBird repository or mutable host operating-system resource directories during normal database creation. Legacy resource files are intake source material only until they are packaged with provenance, hashes, manifests, and validation profiles under this resource tree.

## Required initial pack shape

- `seed-packs/initial-resource-pack/RESOURCE_SEED_MANIFEST.csv`
- charset descriptors and mapping artifacts
- collation descriptors, locale manifest, UCA manifest, and UCA weight source artifacts
- timezone version, IANA source tables, zone tables, and leap-second artifacts
- package manifest, source lineage, license/provenance, hashes, defaults, and conformance IDs

## Create-time seed packs

Database creation requires a versioned resource seed pack for character sets, charset mappings, collations, locale/UCA data, and time-zone data.

The initial private seed-pack placeholder is under `seed-packs/initial-resource-pack/`. It is expected to be built from the historical ScratchBird resource families, but runtime code must consume the normalized private seed pack and must not depend on legacy repository paths.

Missing, corrupt, or incomplete required seed data fails closed by default. A minimal-bootstrap or repair-only profile may open a database without full resource activation only when explicitly requested, and must reject resource-dependent operations.
