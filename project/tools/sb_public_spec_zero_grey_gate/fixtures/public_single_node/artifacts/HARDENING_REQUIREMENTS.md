# Hardening Requirements

Search key: `PUBLIC_SINGLE_NODE_HARDENING_REQUIREMENTS`

## Required Before Implementation

- Reusable target zero-grey gate must accept arbitrary execution_plan roots and target
  counts.
- Every target row must have at least one CTest label before closure.
- Persistent storage/page/datatype/index/catalog format changes must define
  version, upgrade, downgrade refusal, repair/diagnostic behavior, and tests.
- Cloud storage must include provider interface boundaries and local emulator
  fixtures; missing real credentials may not skip implementation.
- Donor original regression must compile/use donor tools where local donor
  source exists and must preserve ScratchBird engine authority.
- Driver/adaptor/tool build artifacts must remain under build directories.
- No code may add SQLite, PRAGMA, WAL, donor engine storage, parser-owned
  transaction finality, or hidden SQL execution as a shortcut.
- No target row may close with TODO, FIXME, placeholder, stub, or
  not-implemented behavior in the target path.

## Required Fault Families

- Parser malformed profile, wrong dialect, profile-not-installed, meta-command
  misuse, and hostile input.
- Storage torn write, wrong filespace UUID, wrong database UUID, stale snapshot,
  cloud credential missing, and provider refusal.
- Datatype overflow, invalid cast, collation mismatch, 128-bit numeric edge,
  domain method missing, and wire/storage mismatch.
- Index stale descriptor, invalid index family, insert/update optimization
  mismatch, and metrics inconsistency.
- Security bad credential, policy deny, privilege deny, audit write failure,
  encryption key unavailable, and plugin manifest mismatch.
- Wire/driver reconnect ambiguity, timeout/cancel, failed parser process,
  listener shutdown, protocol version mismatch, and idempotency conflict.
- Donor incompatible feature, missing donor toolchain, donor diagnostic mapping,
  metadata overlay mismatch, and original regression divergence.
