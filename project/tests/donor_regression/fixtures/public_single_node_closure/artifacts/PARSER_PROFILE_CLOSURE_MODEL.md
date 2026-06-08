# Parser Profile Closure Model

Search key: `PUBLIC_SINGLE_NODE_PARSER_PROFILE_CLOSURE_MODEL`

Parser profiles are installation-independent. SBSQL, Native V3, and each donor
parser/UDR package may share code only within its dialect package. Cross-dialect
dependencies are forbidden because any parser may be absent.

Closure requires:

- Stable profile manifest and admission rules.
- Native V3 management syntax aligned to canonical syntax.
- SBLR operation/lowering/runtime matrix coverage.
- Lossless CST, AST, and bound-AST evidence.
- Profile-specific diagnostics and exact refusals.
- Parser cache invalidation keyed by catalog/security/profile epochs.
- Full-route parser-to-engine tests through SBWP/TLS, listener, parser pool,
  IPC, server, engine security, SBLR execution, and MGA transaction authority.

P1 implementation evidence:

- `sbsql_parser_v3_profile_meta_conformance` covers meta-command registry rows,
  exact refusals, Native V3 management classification, public-profile
  cluster-private refusal, and lossless CST source-buffer evidence.
- `sbsql_sblr_operation_matrix_static_gate` verifies mapped SBLR operation rows
  against engine dispatch/opcode evidence and trusted parser-support UDR
  boundary evidence.
- `sbsql_cache_epoch_correctness_conformance` covers parser cache dimensions and
  invalidations for catalog, security, grant, descriptor, UDR, name-resolution,
  resource, parser-package, disclosure, security-authority, cluster-policy, TTL,
  memory-pressure, connection, transaction, role, group, search-path, language,
  policy-profile, parser-profile, and result-contract changes.
