# ScratchBird Default Local Password Policy Pack

Search key: POLICY_PACK_MIGRATION

`default-local-password` is the first public release policy seed pack.

The pack is create-time input only. Database creation must validate the pack and
materialize the declared security provider, role, group, grant, and policy
profile records into durable catalog state. Ordinary database open, reload,
authorization, optimizer planning, agent execution, support-bundle collection,
and post-create policy mutation must not re-read this filesystem pack.

The default security provider is local password only. External providers are
declared as disabled or unsupported by default and require explicit future
provider work before production enablement.
