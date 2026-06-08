# Cache Epoch Correctness Report

Status: complete
Search key: `FSPE-012A-CACHE-EPOCH-CORRECTNESS-REPORT`
Owning slice: `FSPE-012A`

## Summary

FSPE-012A expands the parser SBLR template cache key to include every authority dimension required by the slice:

- statement shape hash
- catalog epoch
- security policy epoch
- descriptor epoch
- UDR epoch
- search path hash
- language profile
- policy profile
- parser profile
- result contract hash

The cache exposes targeted invalidation methods for catalog, security, descriptor, UDR, search path, language, policy profile, parser profile, and result contract changes. Cache metrics now report key dimensions and invalidation count.

## Gate

| Field | Value |
| --- | --- |
| CTest label | `sbsql_cache_epoch_correctness_conformance` |
| Source | `project/tests/sbsql_parser_worker/sbsql_cache_epoch_correctness_conformance.cpp` |
| Cache source | `project/src/parsers/sbsql_worker/cache/sblr_template_cache.*` |
| Unexpected failures | 0 |

## Coverage

The conformance gate verifies:

- Stable cache keys include all authority dimensions.
- Lookup misses occur when any authority dimension changes.
- Targeted invalidation removes stale entries while retaining entries matching the active authority dimension.
- Cache flush removes entries and increments invalidation metrics.
- Snapshot JSON exposes the complete key dimension set.

## Result

No cached SBLR can be reused after catalog, security, descriptor, UDR, search-path, language, policy-profile, parser-profile, or result-contract authority changes.
