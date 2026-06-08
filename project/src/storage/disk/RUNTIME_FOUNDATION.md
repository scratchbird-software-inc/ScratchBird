# Storage Disk Runtime Foundation

This package implements `RUNTIME-007`: the file/device abstraction used by later database create/open, page, and recovery code.

## Scope

The package owns:

- create/open/close for binary database files;
- read-at and write-at operations;
- sync operation using stream flush;
- size query;
- device capability query;
- deterministic disk diagnostics.

## Non-scope

This slice does not define database headers, page headers, checksums, page-size profiles, encryption, unknown-page handling, or cluster structure recognition. Those are later storage slices.

## Authority rules

- Disk I/O is not catalog authority, transaction authority, parser authority, or cluster authority.
- Reads and writes are byte-addressed primitives only.
- Higher layers must validate database headers, page headers, checksums, page families, and ownership before treating bytes as ScratchBird structures.

## Database header and page-size profile metadata

`RUNTIME-008` adds the serialized database header contract.

The header records:

- ScratchBird database magic;
- format major/minor version;
- serialized header byte count;
- page-size profile;
- checksum algorithm;
- UUIDv7 database identity;
- creation Unix epoch milliseconds;
- database feature flags;
- compatibility flags;
- header checksum.

The initial checksum algorithm is `fnv1a64`, used as deterministic header corruption detection until the storage checksum package selects the production checksum implementation.

Feature flags currently reserve:

- encrypted database;
- cluster structures present;
- variable page-size profile;
- compatibility UUID mapping present.

Compatibility flags currently reserve:

- public-node-safe header open;
- requires cluster authority;
- requires decryption password;
- unknown-page safe classification required.

This slice validates the database header only. It does not yet open a full database, classify pages, decrypt payloads, or activate cluster authority.

## Page header, page UUID, and safe classification metadata

`RUNTIME-009` adds the serialized page-header contract and safe page classification.

The page header records:

- ScratchBird page magic;
- serialized page-header byte count;
- page size;
- page type;
- checksum algorithm;
- UUIDv7 database identity;
- UUIDv7 filespace identity;
- UUIDv7 page identity;
- page number;
- page generation;
- page flags;
- page-header checksum.

The classifier distinguishes:

- supported local pages;
- reserved local pages;
- cluster-only pages;
- encrypted or opaque pages;
- unknown safe read-only pages;
- invalid magic;
- invalid headers;
- checksum mismatch.

Cluster-only pages are readable for safe classification but not writable by standalone public-node authority. Encrypted or opaque pages are readable only as opaque metadata until the required decryption authority is provided by a later slice. Unknown safe pages are read-only. Unknown unsafe pages fail closed.
