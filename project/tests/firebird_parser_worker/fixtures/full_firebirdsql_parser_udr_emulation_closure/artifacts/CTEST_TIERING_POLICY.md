# Firebird CTest Tiering Policy

Status: draft
Search key: `FIREBIRD_CTEST_TIERING_POLICY`

## Required Tiers

| Tier | Label | Purpose |
| --- | --- | --- |
| Fast | `firebird_fast` | Parser unit and small generated matrix checks. |
| Standard | `firebird_standard` | Parser, UDR, SBLR, engine, diagnostics, catalog overlays. |
| Exhaustive | `firebird_exhaustive` | Every generated Firebird surface variation. |
| Reference native | `firebird_reference_native` | Firebird-built tools and original regression replay. |
| Differential oracle | `firebird_differential_oracle` | Normalized real-Firebird versus ScratchBird comparisons. |
| Soak | `firebird_soak` | Long-running concurrency, streaming, cancellation, fuzz, and repeated replay. |

## Rule

Default developer CTest runs may use fast or standard labels. Exhaustive, reference-native, differential-oracle, and soak labels must be explicit so long-running reference regression jobs do not run accidentally.
