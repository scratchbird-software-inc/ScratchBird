# Initial Resource Seed Pack

This directory is the private project placeholder for the create-time resource seed pack.

The first pack is expected to be built from the historical ScratchBird resource families for character sets, charset mappings, collations, locale/UCA data, and IANA time-zone data. Those legacy files are reference inputs only. Runtime code must consume a normalized, checksummed seed pack from this tree or an installed package, not an absolute path into the legacy project.

The seed pack must be loaded during database creation before normal catalog use. Missing, corrupt, or incomplete seed data fails closed unless an explicit minimal-bootstrap or repair-only profile is selected.
