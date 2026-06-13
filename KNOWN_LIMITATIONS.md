# Known Limitations

ScratchBird is an early beta public source-review release.

## Public Release Scope

- The public repository is focused on the single-node open-core engine and public test/review surface.
- The production cluster provider is not included in this public source tree.
- Reference-system parser implementation source, reference-system documentation, raw upstream regression payloads, and native reference tools are not part of the public GitHub source surface. Reference regression tests are external fixtures consumed by ScratchBird-owned CTest harnesses.
- Driver and adaptor support is limited to the lanes that have current build, route, conformance, packaging, and release evidence.
- Benchmark harnesses are included for reproducibility. They are not performance claims.
- AI-facing services are release-candidate or tracked surfaces unless a release evidence matrix says otherwise.
- Unsupported features should fail closed with diagnostics rather than silently executing.

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

## Review Guidance

The presence of a source file, generated artifact, manifest row, test fixture, benchmark entry, or compatibility profile means the area is tracked. It is not by itself a support claim.
