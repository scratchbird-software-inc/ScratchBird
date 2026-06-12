# Third-Party Notices

This file summarizes third-party material categories for the public source-review repository.

## Categories

| Category | Public Use | License Handling |
| --- | --- | --- |
| Reference-system regression tests | External CTest fixtures consumed by ScratchBird-owned harnesses | Raw upstream payloads are not tracked; upstream license applies to the externally supplied fixture |
| Reference-system command-line tools | External test execution where allowed | Native/downloaded tools are not tracked; upstream license applies to the externally supplied tool |
| Resource packs | Locale, time-zone, seed, or configuration resources | Upstream license retained with the resource |
| SBsql language resource pack | ScratchBird-generated canonical SBsql language resources and deterministic beta language profiles | Generated from ScratchBird-owned registry and system-object baselines under MPL-2.0; no upstream engine source or documentation included |
| Universal Dependencies PUD topology reference | Derived clause and phrase topology metrics for SBsql language profiles | Raw treebank files are not tracked; derived metrics are attributed to Universal Dependencies PUD treebanks under CC BY-SA 3.0 |
| Driver ecosystem files | Driver/adaptor build and conformance material | License retained in each driver/adaptor subtree |
| Generated artifacts | Reproducible source-review evidence | Generated from public inputs or marked with provenance metadata |

## Boundary

Reference-system implementation source, public reference-system documentation, raw upstream regression payloads, and downloaded/native reference tools are not part of the tracked public GitHub source surface. The public repository tracks only ScratchBird-owned harnesses, manifests, fixture locators, acquisition metadata, and gates for compatibility or refusal evidence.
