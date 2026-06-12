# Initial Resource Seed Pack

This directory is the private project placeholder for the create-time resource seed pack.

The first pack is expected to be built from the historical ScratchBird resource families for character sets, charset mappings, collations, locale/UCA data, IANA time-zone data, and the generated SBsql language resource pack. Those legacy files are reference inputs only. Runtime code must consume a normalized, checksummed seed pack from this tree or an installed package, not an absolute path into the legacy project.

The seed pack must be loaded during database creation before normal catalog use. Missing, corrupt, or incomplete seed data fails closed unless an explicit minimal-bootstrap or repair-only profile is selected.

The SBsql language resource pack under `resources/i18n/sbsql-language-resource-pack/` is generated from ScratchBird-owned registry and system-object baselines. It contains canonical English resources plus exact beta profiles for French, German, Italian, and Spanish; local parser or driver output remains untrusted until server-side SBLR, UUID, descriptor, authorization, policy, and MGA validation succeeds.
