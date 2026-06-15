# Client and Editor Language Surface

## Purpose

This page describes the driver and editor side of ScratchBird language support:
the language surface manifest, capability negotiation, per-component capability
posture, the editor tool protocol schema, and how client-produced draft SBLR is
handled. The Client and Driver Guide references this page for language surface
details rather than restating them.

## The Language Surface Manifest

The language surface manifest is located at:

```
project/drivers/language/sbsql_language_surface_manifest.json
```

It is the authoritative declaration of how every driver, adaptor, and tool
exposes language support. Its top-level fields are:

| Field | Value |
|-------|-------|
| `schema_version` | `sbsql.driver_language_surface_manifest.v1` |
| `resource_identity` | `sbsql.common_resource_pack.v1` |
| `protocol_schema` | `project/drivers/language/sbsql_editor_tool_protocol.schema.json` |
| `driver_package_manifest` | `project/drivers/DriverPackageManifest.csv` |

The manifest contains three top-level sections: `common_resource_contract`,
`common_resource_pack_metadata`, and `components`.

## The common_resource_contract

All seven invariants in `common_resource_contract` apply uniformly to every
component. They are described in detail in
[overview_and_authority_model.md](overview_and_authority_model.md). No driver or
adaptor may relax them.

## The common_resource_pack_metadata

The `common_resource_pack_metadata` section describes the shared resource pack
used by all components:

| Field | Value |
|-------|-------|
| `resource_identity` | `sbsql.common_resource_pack.v1` |
| `support_state` | `release_supported` |
| `resource_pack_path` | `project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack` |
| `resource_pack_manifest_sha256` | `sha256:a7a30e7650ad7d4f2402bf3ab502d37cb8ff8b0dde171752320ee288aaab9ec2` |
| `resource_hash` | `sha256:f5469159a874fad2a22765e4c75d938e6d4cc8ac740169fb215a96dcac2b1be3` |
| `resource_pack_common_resource_hash` | `sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc` |

The `supported_exact_profiles` field lists the seven supported locale tags:
`en-US`, `en-CA`, `fr-FR`, `fr-CA`, `de-DE`, `it-IT`, `es-ES`.

Deterministic validation requirements:
- `no_wall_clock_fields: true`
- `sort_keys_for_hash: true`
- `stable_utf8_json: true`

## The Components Array

The `components` array lists 31 entries — one per driver, adaptor, or tool that
exposes a language surface. Each entry carries the required fields defined by
the editor tool protocol schema.

### Component Categories

Components fall into three categories:

| `component_category` | Count | Description |
|---------------------|-------|-------------|
| `driver` | 21 | Language drivers that implement the common resource protocol directly |
| `adaptor` | 9 | Adaptors that delegate language surface operations to an underlying driver or common protocol consumer |
| `tool` | 1 | Native CLI tool |

### Drivers

The following component IDs have `component_category: driver`:

`driver:adbc`, `driver:flightsql`, `driver:julia`, `driver:perl`,
`driver:r2dbc`, `driver:cpp`, `driver:dart`, `driver:dotnet`,
`driver:elixir`, `driver:go`, `driver:jdbc`, `driver:mojo`,
`driver:node`, `driver:odbc`, `driver:pascal`, `driver:php`,
`driver:python`, `driver:r`, `driver:ruby`, `driver:rust`, `driver:swift`

All drivers carry `implementation_state: runtime_integrated_with_tests`.

### Adaptors

The following component IDs have `component_category: adaptor`:

`adaptor:scratchbird-airbyte`, `adaptor:scratchbird-dbt-adapter`,
`adaptor:scratchbird-looker`, `adaptor:scratchbird-powerbi`,
`adaptor:scratchbird-tableau`, `adaptor:scratchbird-dbeaver-driver`,
`adaptor:scratchbird-hibernate-dialect`, `adaptor:scratchbird-metabase-driver`,
`adaptor:scratchbird-prisma-adapter`, `adaptor:scratchbird-sqlalchemy-dialect`,
`adaptor:scratchbird-superset-driver`, `adaptor:scratchbird-typeorm-adapter`

### Tool

`tool:cli` — the native command-line interface.

## Capability Fields

Every component entry declares the following capability fields. The permitted
values for each field are defined by the editor tool protocol schema.

| Field | Driver value | Adaptor value |
|-------|-------------|---------------|
| `local_parse` | `common_resource_pack_required` | `delegates_to_common_resource_pack_consumer` |
| `draft_sblr` | `local_draft_allowed_server_revalidated` | `delegates_to_driver_local_draft_server_revalidated` |
| `completion` | `common_protocol_required` | `delegates_to_common_protocol_consumer` |
| `diagnostics` | `canonical_message_vector_keys_required` | `delegates_to_canonical_message_vector_consumer` |
| `canonical_preview` | `required` | `delegates_to_common_protocol_consumer` |
| `renderer` | `preferred_language_then_canonical_english` | `delegates_to_common_renderer_consumer` |
| `renderer_lossiness` | `classified_required` | `delegates_to_common_renderer_consumer` |
| `predictive` | `resource_bounded_no_hidden_objects` | `delegates_to_common_protocol_consumer` |
| `standard_english_fallback` | `enabled_only_when_preferred_profile_fails` | `delegates_to_common_parser_consumer` |
| `offline_cache` | `signed_hash_epoch_scoped` | `delegates_to_common_resource_cache` |
| `redaction_metadata` | `required_no_query_text_or_hidden_identifiers` | `delegates_to_common_protocol_consumer` |
| `capability_negotiation` | `exact_resource_identity_required` | `delegates_to_common_negotiation_consumer` |
| `fail_closed_on_mismatch` | `true` | `true` |
| `server_revalidation_authority` | `required` | `required` |
| `authority_boundary` | `client_resources_are_untrusted_until_server_revalidation` | `client_resources_are_untrusted_until_server_revalidation` |
| `resource_identity` | `sbsql.common_resource_pack.v1` | `sbsql.common_resource_pack.v1` |

The tool:cli entry matches the driver posture for all fields.

## Capability Negotiation

All direct-implementation components (`component_category: driver` and
`tool:cli`) declare `capability_negotiation: exact_resource_identity_required`.
This means:

- The client must present the exact resource identity (`sbsql.common_resource_pack.v1`)
  during capability negotiation.
- A mismatch between the client's declared resource identity and the server's
  admitted resource identity fails closed (`fail_closed_on_mismatch: true`).

Adaptor components delegate negotiation (`delegates_to_common_negotiation_consumer`)
to the underlying driver they consume.

## Canonical Preview

Drivers and `tool:cli` declare `canonical_preview: required`. Before a locally
drafted statement is submitted to the server, the driver must produce a
canonical preview — a render of the canonical form based on the local resource
pack. This preview is produced by the local renderer and carries a lossiness
classification. It is not authoritative; the server revalidates on submission.

## Local Draft SBLR

Drivers may produce a local draft SBLR from the locally parsed canonical element
stream. The `draft_sblr` field `local_draft_allowed_server_revalidated` means:

- The driver is permitted to produce a draft SBLR locally for responsiveness.
- The draft SBLR is not trusted by the server until revalidation.
- `draft_sblr_is_untrusted_until_server_admission: true` from
  `common_resource_contract` applies.

A local draft SBLR that the server refuses produces
`SBSQL.LANG_RESOURCE.LOCAL_DRAFT_SBLR_REFUSED`.

## Localized Diagnostics

All direct-implementation components declare
`diagnostics: canonical_message_vector_keys_required`. Diagnostic messages
delivered to the application must use canonical message vector keys. Localized
message text is applied by the diagnostics resource for the active locale; keys
are stable across locales and driver versions.

Diagnostic key stability is required because:
- Applications may programmatically inspect diagnostic codes.
- Localized message text may change between pack versions.
- Keys never change once admitted.

## The Editor Tool Protocol Schema

The editor tool protocol schema is located at:

```
project/drivers/language/sbsql_editor_tool_protocol.schema.json
```

Its fields:

| Field | Value |
|-------|-------|
| `schema_version` | `sbsql.editor_tool_protocol.schema.v1` |
| `protocol_version` | `sbsql.editor_tool.v1` |

The schema declares the `required_component_fields` that every component entry
in the language surface manifest must carry:

`component_id`, `component_category`, `resource_identity`,
`syntax_profile_order`, `local_parse`, `draft_sblr`, `completion`,
`diagnostics`, `canonical_preview`, `renderer`, `renderer_lossiness`,
`predictive`, `standard_english_fallback`, `offline_cache`,
`redaction_metadata`, `capability_negotiation`, `fail_closed_on_mismatch`,
`server_revalidation_authority`, `authority_boundary`,
`implementation_state`

The schema specifies the `syntax_profile_order` (four deterministic steps),
`renderer_lossiness_classes` (five classes), `fallback_diagnostics` (two
codes), and `rendering_diagnostics` (three codes) that every conforming
component must declare. These are cross-verified in the validation function
`ValidateEditorToolProtocol` in `language_resource_contract.cpp`.

The `allowed_component_values` section of the schema enumerates the valid
string values for each field. An `unsupported_release_blocked` value is
available for any capability not yet supported; using it in a component means
that capability is explicitly refused at the release boundary.

## Offline Cache

Direct-implementation components declare
`offline_cache: signed_hash_epoch_scoped`. The offline cache is scoped by:
- Signed resource hash — the cache is keyed to a specific signed pack version.
- Epoch — the cache is invalidated when the resource pack epoch changes.

A cache with a hash that does not match the currently admitted resource pack is
refused.

## Redaction Metadata

All components declare `redaction_metadata: required_no_query_text_or_hidden_identifiers`
(or delegate to a consumer that does). This means:

- Diagnostic payloads must not include raw query text.
- Diagnostic payloads must not include hidden object identifiers.
- Telemetry and support bundle exports require explicit redaction evidence.

## Cross-References

- Common resource contract invariants: [overview_and_authority_model.md](overview_and_authority_model.md)
- Rendering lossiness classes: [rendering_and_fallback.md](rendering_and_fallback.md)
- Diagnostic codes: [diagnostics_reference.md](diagnostics_reference.md)
- Client and Driver Guide: [../Client_Driver_Guide/README.md](../Client_Driver_Guide/README.md)
