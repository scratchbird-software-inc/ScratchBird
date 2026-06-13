# Interface Contracts

CEIC memory and index readiness manifests summarize source, CTest, and release-evidence inputs. They do not grant transaction finality, visibility, recovery, parser, reference, WAL, benchmark, optimizer-plan, index-finality, or agent-action authority.

The public cluster surface is an external cluster provider boundary and must
fail closed without that provider. Public evidence must refuse cluster
benchmark-clean and production-live claims for public stubs.

Memory and optimizer integration is advisory evidence only. It must not use
memory feedback as transaction visibility, transaction finality, transaction
recovery, parser, reference, WAL, benchmark, optimizer-plan, index-finality, or
agent-action authority.
Memory and optimizer integration must not use memory feedback as transaction or
visibility authority.
Provider booleans are not accepted as final authority.
Readiness booleans summarize evidence. They may not become optimizer plan authority, index generation
finality authority, or transaction finality authority.
Support bundles may collect memory, index, optimizer, and agent evidence only as
observability data and must reject authoritative transaction finality claims.
