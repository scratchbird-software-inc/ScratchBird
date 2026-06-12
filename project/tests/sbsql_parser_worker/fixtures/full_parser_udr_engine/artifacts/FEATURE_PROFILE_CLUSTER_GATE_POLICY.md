# Feature Profile and Cluster Gate Policy

Status: complete
Search key: `FSPE-FEATURE-PROFILE-CLUSTER-GATE-POLICY`

## Purpose

This artifact defines the runtime policy gates that every P1+ implementation slice must use when converting registry rows into parser, UDR, server, engine, diagnostic, and regression work.

## Status Vocabulary

| Source status | Implementation policy | Standalone `sb_server` behavior | Parser behavior | UDR behavior | Required evidence |
| --- | --- | --- | --- | --- | --- |
| `native_now` | Implement full native behavior or exact canonical refusal if the canonical spec requires refusal. | Admit only after server security, descriptor, transaction, and resource revalidation. | Parse, bind through public resolver, lower to SBLR, render message vectors. | Validate/parse/describe under engine context where applicable. | Full-route fixture plus parser, UDR if applicable, server, engine, and diagnostic evidence. |
| `native_future` | P0 closure action is promotion to implemented behavior or canonical reclassification to refusal/policy gate. It cannot close by remaining an unimplemented accepted row. | No accidental admission before promotion/reclassification evidence exists. | May not silently accept as implemented without generated backlog assignment. | May not claim support without engine-context fixture. | `NATIVE_FUTURE_PROMOTION_AUDIT.csv` row plus implementation or canonical refusal evidence. |
| `cluster_private` | Gate by cluster profile. Non-cluster builds fail closed. | Return cluster-unavailable or policy-blocked message vector; no cluster authority inference. | Parse/lower only when profile permits; standalone path must render exact fail-closed diagnostic. | Return message vector under engine context; no direct mutation or SQL execution. | Cluster-profile fixture and standalone refusal fixture. |
| `policy_blocked` | Refuse by explicit policy rule. | Return policy-blocked message vector with redaction-safe fields. | Render policy refusal without hidden object leakage. | Preserve policy refusal under engine context. | Negative conformance fixture and diagnostic row. |
| `refused` | Refuse by canonical product behavior. | Return exact refusal diagnostic. | Render canonical refusal. | Return refusal message vector if UDR-visible. | Negative conformance fixture and diagnostic row. |
| reference/profile alias | Map to native SBSQL behavior or exact reference/profile refusal. | Enforce native server/engine authority, not reference authority. | Accept only inside the relevant reference/profile parser mode and render reference-compatible output where required. | No reference alias may bypass engine context. | Reference alias fixture from `REFERENCE_ALIAS_COVERAGE_BACKLOG.csv`. |

## Non-Cluster Fail-Closed Rules

- A cluster-private row must not activate cluster semantics in standalone `sb_server`.
- Standalone refusal must be represented by a canonical message vector and parser rendering template.
- The parser may identify syntax/profile shape, but it must not infer hidden cluster object existence or engine capability.
- The engine remains SBLR/internal-procedure only. No SQL text reaches `sb_engine`.

## Promotion/Reclassification Rules

- Promotion means the row receives parser, UDR when applicable, SBLR, server, engine, diagnostic, and reusable regression evidence.
- Reclassification means a manifest-listed canonical contract or implementation packet imported by such a spec records the exact refusal or policy gate.
- A source status field may preserve `native_future` as historical evidence, but no closure action may use that status as a reason to skip implementation.

## Fixture Requirements

Every batch in `REGISTRY_FAMILY_BATCHING_PLAN.csv` must produce:

- a positive parse fixture for implemented rows;
- a negative or refusal fixture for policy-blocked/refused/standalone cluster-private rows;
- a full-route fixture for every executable non-cluster behavior;
- a reference/profile rendering fixture when the row is reachable through a reference alias;
- a diagnostic/message-vector fixture for every refusal or failure path.

## Acceptance

No P1+ slice may start implementation for a row unless its profile behavior can be traced to this policy, the generated P0 backlog row, and the relevant canonical spec or implementation-packet input.
