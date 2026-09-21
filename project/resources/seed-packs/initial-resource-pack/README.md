# Initial Resource Seed Pack

This directory is the source-controlled baseline for the create-time resource seed pack.

The first pack is expected to be built from the historical ScratchBird resource families for character sets, charset mappings, collations, locale/UCA data, IANA time-zone data, and the generated SBsql language resource pack. Those legacy files are reference inputs only. Runtime code must consume a normalized, checksummed seed pack from this tree or an installed package, not an absolute path into the legacy project.

The seed pack must be loaded during database creation before normal catalog use. Missing, corrupt, or incomplete seed data fails closed unless an explicit minimal-bootstrap or repair-only profile is selected.

The UCA 17.0.0 baseline also retains the complete Unicode 17.0.0
`UnicodeData.txt` for canonical decomposition and combining classes, imported
unchanged from `https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt`
on 2026-09-20. Its SHA-256 is recorded in `uca_manifest.json`. The accompanying
`resources/collations/uca/UNICODE-LICENSE.txt` is Unicode License v3, obtained
from `https://www.unicode.org/license.txt` on the same date. These are Unicode
data assets, not donor implementation source. Runtime normalization uses the
owning database's retained artifact, never this source pathname or host ICU.
The independently pinned normalization conformance vectors are test-only and
are not loaded into database resource catalogs. NFD support alone is not UCA
comparison, tailoring, sort-key, index or SQL-route conformance.

The same cohort now retains unchanged Unicode17.0.0 `PropList.txt`, imported
2026-09-20 from `https://www.unicode.org/Public/17.0.0/ucd/PropList.txt` under
the accompanying Unicode License v3. Its SHA-256 is in `uca_manifest.json`.
The root weight interpreter uses its Unified_Ideograph property together with
UnicodeData assignment ranges; a Unicode block is not proof of assignment.
Root non-ignorable DUCET qualification does not qualify any locale tailoring.

The SBsql language resource pack under `resources/i18n/sbsql-language-resource-pack/` is generated from ScratchBird-owned registry and system-object baselines. It contains canonical English resources plus exact beta profiles for French, German, Italian, and Spanish; local parser or driver output remains untrusted until server-side SBLR, UUID, descriptor, authorization, policy, and MGA validation succeeds.
