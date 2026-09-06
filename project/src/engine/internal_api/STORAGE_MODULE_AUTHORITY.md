# Storage Module Authority Boundaries

This document records the implementation ownership split for the canonical DML
storage route. It is an implementation-maintenance contract, not transaction or
recovery authority and not runtime-conformance evidence.

| Module | Owns | Consumes but does not own | Invariant |
| --- | --- | --- | --- |
| `mga_relation_store/mga_relation_store.cpp` | Canonical persisted relation projection and relation-store facade | MGA transaction inventory, descriptors, index providers, cleanup and recovery services | A relation mutation is never its own finality evidence. |
| `mga_relation_store/mga_event_sequence_allocator.cpp` | Durable range allocation, bootstrap scanning and process cache for MGA companion-stream event sequences | Canonical row, index and metadata stream paths | Event sequences order stream records only and never determine transaction visibility or finality. |
| `mga_relation_store/mga_relation_statistics.cpp` | Derived relation, row-version and index size estimates | Canonical relation read facade and MGA-filtered compatibility projection | Statistics are observations and cannot affect visibility, mutation or finality. |
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
