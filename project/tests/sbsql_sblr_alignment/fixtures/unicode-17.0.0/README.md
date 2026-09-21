# Unicode 17.0.0 normalization oracle

The companion `CollationTest_NON_IGNORABLE_SHORT.txt` is unchanged from
https://www.unicode.org/Public/17.0.0/uca/CollationTest.zip, imported2026-09-20.
Archive SHA-256: `9ba92cb7627c2d09ba537dc2035d006f44f0cafb4e2198f147897fdd9d71bb10`.
File SHA-256: `06a8d9d2191574c74623f66af960682feadeb714a9fa12ab4657c425f8683f53`.
The test checks all208040 scalar rows at primary/secondary/tertiary/identical
strength, comparison/key parity, and atomic refusal of all30 surrogate-bearing
rows. Root non-ignorable DUCET order is not a tailoring oracle. The license
notice linked below applies to both Unicode fixtures.

`NormalizationTest.txt` is the unchanged independent Unicode test data from
https://www.unicode.org/Public/17.0.0/ucd/NormalizationTest.txt, imported
2026-09-20. SHA-256:
`5019ffd530751a741900c849c0e010332f142a3612234639bd200b82138a87db`.

Copyright and permission notice: Unicode License v3 in
`../../../../resources/seed-packs/initial-resource-pack/resources/collations/uca/UNICODE-LICENSE.txt`.
The file itself also retains the upstream copyright and license reference.

The fixture checks all five NFD relations in every row and identity for every
Unicode scalar omitted from Part1, including unassigned/private-use/noncharacter
scalars. Production table loading uses separate UnicodeData bytes, not these
expected answers or a host Unicode library. NFC/NFKC/NFKD, collation weights,
tailorings, comparison/index/hash consistency and SQL transport remain separate
required coverage; this target makes no end-to-end claim.
