# Storage Module Authority Boundaries

This document records the implementation ownership split for the canonical DML
storage route. It is an implementation-maintenance contract, not transaction or
recovery authority and not runtime-conformance evidence.

| Module | Owns | Consumes but does not own | Invariant |
| --- | --- | --- | --- |
| `mga_relation_store/mga_relation_store.cpp` | Canonical persisted relation projection and relation-store facade | MGA transaction inventory, descriptors, index providers, cleanup and recovery services | A relation mutation is never its own finality evidence. |
| `mga_relation_store/mga_bulk_import_publication.cpp` | Durable bulk-import preparation, publication-state evidence, imported-row evidence and idempotent replay | Scoped relation point lookup and MGA transaction identity supplied by the engine context | Bulk-import publication records classify companion-stream state only; durable transaction inventory decides finality and visibility. |
| `mga_relation_store/mga_contextual_text_descriptor.cpp` | Canonical relation-descriptor parsing, contextual-text identity validation/rewrite, public projection material and sealed descriptor construction | Contextual-text policy and descriptor identity authorities | Descriptor material is metadata interpretation and cannot decide transaction visibility or finality. |
| `mga_relation_store/mga_event_sequence_allocator.cpp` | Durable range allocation, bootstrap scanning and process cache for MGA companion-stream event sequences | Canonical row, index and metadata stream paths | Event sequences order stream records only and never determine transaction visibility or finality. |
| `mga_relation_store/mga_large_value_store.cpp` | Large-value chunk persistence, locator expansion and reclaim evidence | Transaction-inventory-derived read visibility supplied through the internal store bridge | Payload or reclaim presence never decides row visibility or transaction finality. |
| `mga_relation_store/mga_secondary_index_coordination.cpp` | Secondary-index delta staging, transactional overlay, merge, recovery validation/repair and garbage cleanup | Canonical relation state plus transaction-inventory-derived validation and visibility | Delta-ledger state may coordinate index maintenance but cannot decide transaction finality or row visibility. |
| `mga_relation_store/mga_relation_store_internal_support.hpp` | Narrow visibility projection, mutation-request validation, savepoint rollback predicates, migration-lineage visibility and scoped-summary maintenance for extracted store modules | Canonical MGA transaction inventory and savepoint authority | The bridge exposes derived visibility, validation and physical-summary maintenance only; it cannot publish or finalize transactions. |
| `mga_relation_store/mga_relation_metadata_store.cpp` | Persisted relation-metadata and descriptor-field decoding, immutable generation caches, and raw metadata snapshots | Savepoint rollback predicate supplied by the canonical relation/MGA authority | Metadata records and cache generations cannot decide transaction visibility or finality. |
| `mga_relation_store/mga_row_codec.cpp` | Text/binary row-version framing, typed/native value materialization, bounded decode accounting and format validation | Row publication context and MGA-filtered read coordination | Encoded creator/event identities are data only; the codec cannot decide visibility or finality. |
| `mga_relation_store/mga_row_version_reader.cpp` | Scoped row-version physical decoding, bounded read accounting and decoded-segment cache validation | Row codec and transaction-inventory-filtered relation read coordination | Segment presence, cache state and encoded transaction fields never decide visibility or finality. |
| `mga_relation_store/mga_row_version_writer.cpp` | Row-version append materialization plus the shared row/index hot-append buffering and flush coordinator | Event-sequence allocation, scoped-summary maintenance and caller-supplied MGA transaction identity | Writing row or index evidence does not commit, publish visibility or finalize the transaction. |
| `mga_relation_store/mga_savepoint_store.cpp` | Durable generic savepoint markers, bounded parsing, active-savepoint projection and rollback-range classification | UPDATE statement-savepoint projection supplied by the durable UPDATE store | A savepoint is a transaction-local boundary; it cannot allocate a transaction or decide transaction finality. |
| `mga_relation_store/mga_relation_statistics.cpp` | Derived relation, row-version and index size estimates | Canonical relation read facade and MGA-filtered compatibility projection | Statistics are observations and cannot affect visibility, mutation or finality. |
| `mga_relation_store/mga_update_durable_store.cpp` | Exact durable UPDATE operation frames, UPDATE statement barriers, authenticated recovery inspection and append/fsync sequencing | Durable transaction-inventory state and generic savepoint rollback projection | UPDATE journals and barriers are subordinate mutation/recovery evidence; only transaction inventory publishes finality. |
| `mga_relation_store/mga_heap_executor.cpp` | Executor heap acquisition, physical-node dispatch and result projection | Prepared relation authority and visible-row/count/stream facades | Executor code cannot persist transaction outcome or mutate canonical rows. |
| `mga_relation_store/mga_heap_runtime_support.hpp` | Narrow read observation and memory-accounting bridge | Relation reader counters | Observations are derived evidence and cannot affect visibility. |
| `dml/insert_physical_integration.cpp` | Page reservation/selection plus filespace, overflow and strict-load coordination | Engine transaction identity and subsystem ledgers | Successful integration cannot decide commit or row visibility. |
| `dml/direct_physical_bulk_append.cpp` | Direct bulk row preparation and canonical row/index publication orchestration | Transactional relation facade, index provider and MGA transaction handle | Whole-statement publication remains transaction-owned and all-or-nothing. |
| `dml/direct_bulk_generated_projection.cpp` | Deterministic generated-value projection | Bound table descriptors and request options | Generated values have no storage or finality authority. |
| `dml/direct_bulk_uuid_authority.cpp` | Batched UUIDv7 identity allocation | Request identity and result evidence sink | UUID order is never transaction order or finality evidence. |

Dependency direction is executor or DML coordinator to the canonical relation
facade, then to engine-owned MGA/index/page services. The relation facade does
not call back into executor projection. Parser SQL, donor state, source tokens,
and UUID ordering never become transaction authority.

`storage_module_authority_gate.py` ratchets the extracted module sizes, build
enrollment, unique ownership anchors, forbidden authority calls and the ban on
implementation-fragment includes. Its CTest labels explicitly classify it as a
source contract. Runtime correctness remains proven by the insert integration
probe, DML lifecycle suites, index lifecycle matrix and real kill/restart DML
recovery gate.
