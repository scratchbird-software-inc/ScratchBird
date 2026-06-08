# Diagnostic Code Matrix

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_DIAGNOSTIC_CODE_MATRIX`

Every refusal path must have a stable diagnostic code, retryability, audit
expectation, and transaction-finality behavior.

| Condition | Diagnostic family | Retryability | Audit required | Finality |
| --- | --- | --- | --- | --- |
| Protected material access denied | `SB-PROTECTED-MATERIAL-ACCESS-DENIED` | no unless policy changes | yes | no side effect |
| Protected material release denied | `SB-PROTECTED-MATERIAL-RELEASE-DENIED` | no unless policy changes | yes | no release |
| Protected material purge denied | `SB-PROTECTED-MATERIAL-PURGE-DENIED` | no unless policy changes | yes | no purge |
| Protected material version conflict | `SB-PROTECTED-MATERIAL-VERSION-CONFLICT` | retry transaction | yes | rollback or no commit |
| Missing cloud provider profile | `SB-CLOUD-PROVIDER-NOT-FOUND` | no until configured | yes | no side effect |
| Unsupported provider capability | `SB-CLOUD-CAPABILITY-UNSUPPORTED` | no unless profile changes | yes | no side effect |
| Static secret forbidden | `SB-CLOUD-STATIC-SECRET-FORBIDDEN` | no unless explicit policy | yes | no auth/session |
| Unsupported KMS mode | `SB-CLOUD-KMS-MODE-UNSUPPORTED` | no unless profile changes | yes | no release |
| KMS version stale | `SB-CLOUD-KMS-VERSION-STALE` | retry after refresh | yes | no release |
| Invalid Kubernetes CRD | `SB-K8S-CRD-INVALID` | retry after fix | yes | no lifecycle action |
| Cluster-only Kubernetes field | `SB-K8S-CLUSTER-FIELD-REFUSED` | no in public build | yes | no lifecycle action |
| Operator reconcile conflict | `SB-K8S-RECONCILE-CONFLICT` | retry idempotently | yes | no duplicate action |
| Unsafe edge redaction policy | `SB-EDGE-REDACTION-UNSAFE` | no unless policy changes | yes | no event |
| Pre-commit edge invalidation | `SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED` | retry after commit | yes | no event |
| Unsupported CDN provider | `SB-EDGE-PROVIDER-UNSUPPORTED` | no unless profile changes | yes | no event |

The implementation phase must add these codes to the canonical diagnostic
registry or map them to existing equivalent codes with explicit citations.
