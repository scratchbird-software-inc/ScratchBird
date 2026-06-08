# Firebird Source Hash Audit

Status: P0 seed
Search key: `FIREBIRD_SOURCE_HASH_AUDIT`

## Baseline

The local Firebird 5.0.4 reference packet is the initial behavior-evidence baseline. The packet is read-only evidence for clean-room implementation; it is not runtime code and must not be linked or copied into ScratchBird products.

| Evidence | Path | SHA-256 | Status |
| --- | --- | --- | --- |
| Release evidence manifest | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/RELEASE_EVIDENCE_MANIFEST.yaml` | `a68d035a37348e7a9c4c14f3d368bb8302c218fe8a661b8952066014929e4fc6` | present |
| Source tree manifest | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/TREE_MANIFEST.sha256` | `cf2a57a3e2921d0d6d9156c25a3d7a7da3bc52c53734e52ce2c0c1f42309d447` | present |
| Source archive hash pointer | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/source-archive/firebird-5.0.4.tar.gz.sha256` | `046be0738dc9505a58930487cd040e58cdd2ed12e67cbb6b01a06d14205e18e6` | present |
| Regression source pointers | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/regression/SOURCE_POINTERS.md` | `d8f966c616151270f851e9bdb3004811320f5060baf62ce747450b950e619c44` | present |
| Firebird QA donor replay manifest | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/regression/FIREBIRD_QA_DONOR_REPLAY_MANIFEST.csv` | `a0eae076cf547aff9ee9401067a143bb6afcfd5fa73d0a49c68a75ec805c34c1` | present |
| Firebird QA donor replay family manifest | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/regression/FIREBIRD_QA_DONOR_REPLAY_FAMILY_MANIFEST.csv` | `e86fb3be6ed47865e78fae4ef4c45d065a9c35b8b3da4699529c1783cdf1ab80` | present |
| Firebird QA candidate asset hash manifest | `project/tests/donor_regression/donor_release_acquisition/firebird/5.0.4/regression/FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv` | `e6791254c8bff660f0759b1ae6ee46f95fe3063f55f4f44227a852652da2f632` | present |
| Reference packet source authority matrix | `docs/reference/donor_reference_packets/emulation_1_to_1_engine_reference_packets_2026-04-02/firebird/source_authority_matrix.csv` | `86c6c6c959f07ceaba843101855f81aac2cdf9d6609b0a7ca9d4444f71cc71fa` | present |
| Builtin inventory seed | `project/tests/donor_regression/donor_catalog_seeds/firebird/firebird_5_builtin_inventory.csv` | `d6cfb675a6e6fa356de40c4d155232686068e7c73b9d349513c2dfce009d8503` | present |
| Catalog inventory seed | `project/tests/donor_regression/donor_catalog_seeds/firebird/firebird_5_catalog_inventory.csv` | `77603b1394691f9c5a8b35acf9ea779687c105350f0653143f0198f229d0b780` | present |

## Release Evidence Manifest Values

| Artifact | Manifest status | Manifest path | Manifest SHA-256 |
| --- | --- | --- | --- |
| Source archive or release tag clone | present_hashed | `source-archive/firebird-5.0.4.tar.gz` | `5e5f77592713ac77f12ba24284530e2855c400d9062333b351c677a509cfb0a3` |
| Release notes | present_hashed | `release-notes/RELEASE_NOTES_SOURCE.md` | `033344fd6c84c36c787c2d2a2b76a99d9690a38335f7e3d0c04bd9ddf72bdd2b` |
| License text pointer | present_hashed | `license/SOURCE_POINTERS.md` | `8a63d1f52766eb05f5f8fe57cba98802a09f91537df856d47d35a6dfbc5f6e8e` |
| Version proof | present_hashed | `version-proof/VERSION_PROOF_SOURCE.txt` | `9a4086b0ba99812f78c03bc532b0dbebb434ff1de5d053505558482dcb9fe3e1` |
| Catalog seed source extract | present_hashed | `evidence/catalog/SOURCE_EXTRACT.h` | `b11c1aba700a13f67961511b42f2a93911f2a8ac4eea3e612a9d62bd8d37b742` |
| Grammar source extract | present_hashed | `evidence/grammar/SOURCE_EXTRACT.y` | `d421d94a593cd8d8a40346191def1b2db7adbcccce09e566c9ff2703758b671b` |
| Wire/API source extract | present_hashed | `evidence/wire-api/SOURCE_EXTRACT.cpp` | `8dc59f4e28fe22122277d503d0c5967879ea7e488a7f624eb0b828af8efe92db` |
| Builtin/function/operator source extract | present_hashed | `evidence/builtins/SOURCE_EXTRACT.cpp` | `ccd3df2fc1106bae57fdcb489f3accc31d81508b747dd98f1768845a632f3e3d` |
| Datatype source extract | present_hashed | `evidence/datatypes/SOURCE_EXTRACT.h` | `c9a0310ca36b73f029697ce46fd933abb04a8e6d4421e91b7925fc3bc14841e2` |
| Index source extract | present_hashed | `evidence/indexes/SOURCE_EXTRACT.cpp` | `345d71ea6bb500192875090e76303ef3494ffc09411654367d8fdeb8254b24f2` |
| Upstream regression roots | present_hashed | `regression/SOURCE_POINTERS.md` | `1809704ba1b3d86133d72236259a47c18afe4d2e232bbfc6de615f551c718c0c` |
| Clean-room notes | present_hashed | `clean-room/NOTES.md` | `edaecca0678a6b87b1ed3b2e3b612813f13b20dbc2ada5e2f72e312179ebd777` |
| Redaction and visibility notes | present_hashed | `visibility-redaction/NOTES.md` | `291075019e017c24d0785b325b72eafcf28ad7309ee3ca78f2aecd6482b95c41` |

## Required Follow-Up Checks

- Recompute `TREE_MANIFEST.sha256` before any generated Firebird registry is promoted.
- Keep donor-original regression replay rows aligned with `regression/FIREBIRD_QA_DONOR_REPLAY_MANIFEST.csv`.
- Record exact rowset hashes for catalog seed extraction before catalog overlay implementation is marked complete.
- Keep every hash audit update tied to a local path and search key; do not use line-number anchors as implementation authority.
