# Admin Docs and Sample Flow

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_ADMIN_DOCS_AND_SAMPLE_FLOW`

Implementation must include executable admin examples for the public single-node
route.

Required sample flow:

1. Create or open an example database.
2. Create a protected material record with redacted metadata.
3. Rotate protected material and verify version history.
4. Configure local cloud provider emulator profile.
5. Configure local KMS profile and secretless identity fixture.
6. Validate Kubernetes operator CRD schema and run dry-run reconcile.
7. Create an edge cache tag.
8. Commit a transaction that emits a post-commit invalidation outbox event.
9. Verify audit events and metrics.
10. Run negative examples for static secret refusal, unsupported provider, invalid
    CRD, unsafe redaction policy, and pre-commit invalidation.

Samples must run through documented CLI/admin or driver routes and must not rely
on external cloud accounts.
