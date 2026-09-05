# Known Limitations

ScratchBird is an early beta public source-review release.

Every public engine/API capability has an explicit implementation maturity in
`project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml`. The meanings,
promotion evidence, and current summary are documented in
`project/docs/public_api/IMPLEMENTATION_MATURITY.md`. In particular, an SBLR
`*_runtime.cpp` file may only implement a wire codec or descriptor carrier; its
filename is not evidence that the named operation executes or persists state.

Source-token and coverage-inventory checks are static policy signals, not
runtime evidence. Their classifications, claim exclusions, and the required
strongest-to-weakest reporting order are documented in
`project/docs/testing/EVIDENCE_REPORTING.md`.

## Public Release Scope

- The public repository is focused on the single-node open-core engine and public test/review surface.
- The production cluster provider is not included in this public source tree.
- Reference-system parser implementation source, reference-system documentation, raw upstream regression payloads, and native reference tools are not part of the public GitHub source surface. Reference regression tests are external fixtures consumed by ScratchBird-owned CTest harnesses.
- Driver and adaptor support is limited to the lanes that have current build, route, conformance, packaging, and release evidence.
- Benchmark harnesses are included for reproducibility. They are not performance claims.
- AI-facing services are release-candidate or tracked surfaces unless a release evidence matrix says otherwise.
- Unsupported features should fail closed with diagnostics rather than silently executing.

## Planned Driver Lanes Not Implemented

The following driver lanes are planned for the public beta driver surface but are not implemented in this source tree yet. They are present only as contract-package placeholders so the shared driver contract, package metadata shape, route requirements, and future conformance obligations remain visible.

These lanes do not currently contain runnable driver source, package builds, full-route tests, or live server proof. They must not be described as release-supported, release-candidate, or executable driver implementations until their source, packaging, conformance tests, and live server evidence are added.

| Driver lane | Current public status | Current tracked surface |
| --- | --- | --- |
| ADBC | Planned; not implemented | Contract package only at `project/drivers/driver/adbc/package_contract.json` |
| Flight SQL | Planned; not implemented | Contract package only at `project/drivers/driver/flightsql/package_contract.json` |
| Julia | Planned; not implemented | Contract package only at `project/drivers/driver/julia/package_contract.json` |
| Perl DBI | Planned; not implemented | Contract package only at `project/drivers/driver/perl/package_contract.json` |
| R2DBC | Planned; not implemented | Contract package only at `project/drivers/driver/r2dbc/package_contract.json` |

`project/drivers/DriverPackageManifest.csv` and related source-inventory fixtures list these lanes as `planned_not_implemented` / `tracked_not_released` package-contract rows. That tracking metadata does not imply executable support.

## SBsql Language Support

The public source tree includes beta SBsql language-resource support. The language-resource pack is generated, hashed, signed for source-review integrity, indexed in the seed-pack manifest, and verified by deterministic release gates.

Current profile status:

- `en-US` and `en-CA` are the canonical English profiles and are marked release-supported for this public source-review release.
- `fr-FR`, `fr-CA`, `de-DE`, `it-IT`, and `es-ES` are fully populated beta profiles. They require native technical review before they can be described as release-supported language profiles.

The current beta language support includes:

- a common SBsql language resource pack under `project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack`;
- exact-profile metadata for the supported language tags;
- canonical SBsql dialect, system-object name, phrase, topology, predictive grammar, diagnostics, rendering, Unicode, resolver, conformance, and provenance resources;
- Universal Dependencies derived topology metadata for the covered language families, without vendoring raw UD treebank files;
- deterministic localized resource rows with no generated English fallback rows in the profile data;
- stable database-message resources for the public diagnostic matrix and SBLR envelope diagnostics;
- an online translation verification corpus and optional online reference-check utility for beta review evidence;
- driver/tool metadata that lets clients discover the common resource-pack identity, digest, exact profiles, and language surface policy.

Current limits:

- The non-English profiles are machine-seeded and terminology-reviewed only to beta standard. They are not a substitute for native database-engine language review.
- Online translation checks are spot-verification evidence, not authoritative linguistic certification.
- The resource pack does not promise that every driver, editor, IDE integration, or external client already exposes localized parsing, predictive text, or rendering in its UI.
- Client-generated SBLR, UUID descriptors, localized streams, and locally cached command bundles are untrusted. The server must still revalidate authorization, descriptor policy, and transaction authority.
- Localized input must normalize before UUID resolution. Hidden object names, unauthorized schema paths, UUIDs, and policy-protected material must not be disclosed through language resources or predictive text.
- When a stream is not in the selected language profile or cannot be safely normalized, clients and parsers must fall back to canonical English SBsql or fail closed according to policy.
- SBLR-to-SBsql rendering in a preferred language is limited to renderable surfaces. Rendering lossiness must be classified, and source reconstruction from SBLR is not a support claim.
- The localized database-message catalog covers stable public diagnostics and SBLR envelope diagnostics. Internal transient debug messages, proof-only test diagnostics, and private operational details are outside the public language-support promise unless later promoted into the stable message catalog.

## Compatibility Parser Scope

The public source tree includes 25 compatibility parser lanes. Their retained
pre-hold beta evidence is documented in `docs/compatibility-parsers/README.md`.

Those parser workers and their parser-support UDRs are temporarily excluded
from standard builds while the common SBLR boundary stabilizes. They are
retained in source and require `SB_BUILD_COMPATIBILITY_PARSERS=ON`; public
release presets keep that gate OFF.

The retained pre-hold evidence classifies surfaced beta parser operations as
mapped, emulated, parser-only, or deterministic refusal, and records a passing
replay/isolation gate group against its historical SBLR baseline. It does not
establish compatibility with the in-progress SBLR baseline. This is not a
production-readiness claim or a promise of drop-in compatibility with any
reference system.

Raw upstream regression payloads and built original/reference tools are local
test inputs. The public repository tracks acquisition scripts, manifests, and
CTest gates, not the acquired payloads themselves.

Compatibility parsers must not own ScratchBird storage, recovery, security,
filesystem, cluster, or MGA transaction finality. Unsafe surfaces fail closed or
route through native ScratchBird management and policy-admitted bridge paths.

## Review Guidance

The presence of a source file, generated artifact, manifest row, test fixture, benchmark entry, or compatibility profile means the area is tracked. It is not by itself a support claim.
