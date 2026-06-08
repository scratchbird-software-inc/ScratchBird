# Protocol Version Skew Matrix

Search key: `DRIVER_SERVER_PROTOCOL_VERSION_SKEW`.

## Purpose

Make protocol compatibility a first-class release gate instead of an implied
wire-spec detail. The normative behavior lives in
`public_contract_snapshot`
search key `DSR-018-PROTOCOL-VERSION-FEATURE-NEGOTIATION` and
`public_contract_snapshot`
search key `DSR-011-NATIVE-WIRE-SESSION-PAYLOAD-AUTHORITY`.

## Version Selection Matrix

| Case | Startup input | Required result | Diagnostic when refused |
| --- | --- | --- | --- |
| Old client to new server | Client window overlaps server window and required features exist in old version. | Server selects highest overlapping compatible version and returns it in `Ready.selected_protocol_version`. | none |
| Old client requires removed/renamed feature | Required feature is not available in any overlapping version. | Server refuses startup; no undocumented fallback. | `SBWP.FEATURE.REQUIRED_UNSUPPORTED` |
| New client to old server | Client window overlaps old server but optional new features are absent. | Client continues only with `Ready.negotiated_feature_bitmap`; it MUST NOT use absent features. | none |
| New client requires new feature from old server | Required bit is not known by server. | Server refuses startup. | `SBWP.FEATURE.UNKNOWN_REQUIRED` or `SBWP.FEATURE.REQUIRED_UNSUPPORTED` |
| No version overlap | Client max < server min or client min > server max. | Server refuses startup after `Startup`; connection closes after diagnostic flush. | `SBWP.VERSION.NO_COMMON_VERSION` |
| Downgrade accepted | Lower overlapping version satisfies all required bits and server policy admits downgrade. | Server selects lower version, emits `ParameterStatus protocol.selected_version`, and suppresses newer frames. | none |
| Downgrade refused by policy | Lower version could satisfy bits but policy forbids weaker behavior. | Server refuses startup. | `SBWP.VERSION.DOWNGRADE_REFUSED_POLICY` |
| Server policy requires feature absent from client | Server cannot safely run without a feature the client did not offer. | Server refuses startup. | `SBWP.FEATURE.SERVER_REQUIRED_UNOFFERED` |

## Feature Bit Matrix

| Feature class | Unknown to receiver | Required behavior |
| --- | --- | --- |
| Client required core bit | Server does not know the bit. | Refuse startup with `SBWP.FEATURE.UNKNOWN_REQUIRED`. |
| Client optional core bit | Server does not know the bit. | Ignore and omit from `Ready.negotiated_feature_bitmap`. |
| Server negotiated known bit | Client knows the bit. | Client may use exactly as specified by selected version. |
| Server negotiated unknown mandatory bit | Client does not know the bit and extension marks it mandatory. | Client refuses session with `SBWP.FEATURE.UNKNOWN_SERVER_MANDATORY`. |
| Server negotiated unknown ignorable bit | Client does not know the bit and extension marks it ignorable. | Client ignores it and continues. |
| Server emits unregistered unknown bit | No selected extension metadata. | Client refuses; server implementation is non-conformant. |

## Downgrade Suppression Matrix

When a lower protocol version is selected, the server MUST suppress these frames
unless the selected version and negotiated feature bitmap admit them.

| Surface | Feature bit | Downgrade behavior |
| --- | --- | --- |
| Multi-result frames | `kFeatureMultiResult` | Render only first result or refuse before execution when caller required all results. |
| Generated keys | `kFeatureGeneratedKeys` | Refuse generated-key request before execution when required; otherwise omit optional generated-key side channel. |
| OUT/INOUT parameters | `kFeatureOutParameters` | Refuse procedure call if values cannot be represented without semantic loss. |
| Batch result status | `kFeatureBatch` | Refuse batch API before execution when per-member status is required. |
| Pipeline execution | `kFeaturePipeline` | Disable pipeline; require sync barriers. |
| Array bind | `kFeatureArrayBind` | Refuse array-bind execute or split only when client explicitly allowed split semantics. |
| Bulk rejects | `kFeatureBulkRejects` | Refuse collect-reject mode if caller required reject details. |
| LOB locators/chunks | `kFeatureLobLocator` | Inline only when size/policy permits; otherwise refuse. |
| Cursor status | `kFeatureCursors` | Disable explicit cursor lifecycle frames; use ordinary portal flow if semantically equivalent. |
| Copy backpressure | `kFeatureCopyBackpressure` | Use non-credit copy only when server policy permits; otherwise refuse. |
| Session reset | `kFeatureSessionReset` | Client must close/reconnect for pool cleanup. |
| Reauth | `kFeatureReauth` | Server must close and require reconnect when auth refresh is needed. |
| Failover hints | `kFeatureFailoverHints` | Omit hints; client uses generic reconnect policy. |
| Trace context update | `kFeatureTraceContext` | Accept only startup trace keys; later updates refused. |

## Extension Negotiation Matrix

| Case | Required result | Diagnostic when refused |
| --- | --- | --- |
| Registered required extension in supported version range | Select extension and list it in `Ready.negotiated_extensions`. | none |
| Registered optional extension unsupported | Omit extension from `Ready.negotiated_extensions`; continue. | none |
| Required extension unsupported | Refuse startup. | Extension row's refusal diagnostic, or `SBWP.EXTENSION.REQUIRED_UNSUPPORTED`. |
| Unknown required extension | Refuse startup. | `SBWP.EXTENSION.UNKNOWN_REQUIRED`. |
| Unknown ignorable extension | Ignore offer; continue. | none |
| Extension claims core bit without registry row | Refuse startup and mark implementation non-conformant. | `SBWP.EXTENSION.UNREGISTERED_FEATURE_BIT` |
| Extension downgrade incompatible | Refuse selected downgrade. | `SBWP.EXTENSION.DOWNGRADE_INCOMPATIBLE` |

## Required Compatibility Tests

| Test id | Fixture |
| --- | --- |
| `DSR018-old-client-new-server-basic` | v1.0 client negotiates with v1.1+ server; only v1.0 frames emitted. |
| `DSR018-new-client-old-server-optional` | New client offers optional P1 bits to old server; bits are omitted and client suppresses use. |
| `DSR018-new-client-old-server-required` | New client requires `kFeatureLobLocator`; old server refuses before auth. |
| `DSR018-no-overlap` | Client/server version windows do not overlap. |
| `DSR018-downgrade-accepted` | Server selects lower version and emits `protocol.selected_version`. |
| `DSR018-downgrade-refused-policy` | Policy forbids downgrade; deterministic refusal. |
| `DSR018-unknown-required-feature` | Unknown required bit rejects. |
| `DSR018-unknown-optional-feature` | Unknown optional bit ignored. |
| `DSR018-required-extension-unknown` | Unknown required extension rejects. |
| `DSR018-ignorable-extension-unknown` | Unknown ignorable extension ignored. |
| `DSR018-extension-unregistered-bit` | Extension claims an unregistered feature bit; reject. |
| `DSR018-frame-without-feature` | Server/client rejects a P1 extension frame when its bit was not negotiated. |

## Closure Rule

`DSR-018` is closed when the native-wire spec contains version, feature-bit,
downgrade, and extension rules, and validation includes old/new, new/old,
unknown feature, unknown extension, downgrade, and frame-without-feature cases.
