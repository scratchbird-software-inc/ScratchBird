# Full-Surface Suite — Coverage Map (syntax_reference pages → tests)

Status: round-2 (language-surface extension)
Maps every `docs/documentation/draft/Language_Reference/syntax_reference/*.md` page to the
script(s) that exercise it with **executable** assertions, or identifies pages
whose semantics are exercised through suite infrastructure rather than a standalone statement script.

Legend: ✅ covered · ➕ added this round · ➖ not a standalone statement page

| Page | Status | Script(s) / notes |
| --- | --- | --- |
| `table` | ✅ | 010, 013, 015, 030 (+ generated 120/130/140) — create/columns/constraints/alter/drop |
| `index` | ✅ | 030, generated 140 (28 families) |
| `insert` | ✅ | 020, 030 |
| `update` | ✅ | 020 |
| `delete` | ✅ | 020 |
| `merge_and_upsert` | ✅ | 020 (MERGE, UPSERT, ON CONFLICT) |
| `select` | ✅ | 040, 045, 090, generated 150 |
| `from` | ✅ ➕ | 040 (inner/left), 045 (right/full/cross/lateral/using) |
| `where` | ✅ | 040, 045 |
| `group_by_and_having` | ✅ ➕ | 040, 045 (grouping sets/rollup/cube), 150 |
| `window` | ✅ | 040, generated 150 |
| `with` | ✅ | 040 (recursive CTE), 150 |
| `order_by_limit_offset` | ✅ ➕ | 045 (nulls first/last, limit/offset, fetch-first, limit 0), 090 |
| `projection` | ✅ | 040, 045 |
| `operators` | ✅ | 050, generated 170 |
| `operator_type_result_matrix` | ✅ | generated 170 (86×86 cast matrix) |
| `transaction_control` | ✅ | 060 (begin/commit/rollback/savepoint, MGA visibility) |
| `type_descriptor` | ✅ | 010, generated 120/130 (86 datatypes) |
| `security_and_privilege_statements` | ✅ ➕ | 080, 085 (grants/auth), 086 (role/group/grant/revoke) |
| `schema` | ✅ ➕ | 000 (create), 014 (nested, comment, drop) |
| `domain` | ➕ | 011 (create/check/default/not-null/use/alter/drop + check-violation refusal) |
| `sequence` | ➕ | 012 (create/options/NEXT VALUE FOR/alter/comment/drop) |
| `view` | ➕ | 013 (view + materialized view + refresh + drop) |
| `schema_tree_and_name_resolution` | ➕ | 014 (nested schema, qualified resolution, sibling isolation) |
| `policy_mask_and_rls` | ➕ | 086 (policy/mask/rls DDL + catalog introspection) |
| `cluster_gated_statements` | ➕ | 099 (single-node refusal of cluster statements via refusal mechanism) |
| `function` | ✅ ➕ | 052 (expression-body create/invoke/drop); 054 (procedural-body functions); 050/160 (builtin invocation) |
| `procedure` | ✅ ➕ | 056 (non-selectable procedure), 057 (selectable procedures), 058 (dynamic execution result set) |
| `trigger` | ✅ ➕ | 053 (row and statement triggers, OLD/NEW, side effects, drop) |
| `procedural_sql` | ✅ ➕ | 054, 056, 057, 058 |
| `procedural_sql_blocks` | ✅ ➕ | 054, 056, 057, 058 |
| `procedural_sql_control_flow` | ✅ ➕ | 054, 056, 057, 058 |
| `procedural_sql_cursors` | ✅ ➕ | 056 (FOR SELECT cursor loop) |
| `procedural_sql_exceptions` | ✅ ➕ | 056 (SIGNAL syntax inside procedure; positive valid-path assertions) |
| `procedural_sql_triggers_and_events` | ✅ ➕ | 053 |
| `copy` | ✅ ➕ | 059 (COPY FROM STDIN via in-band SB_COPY_INPUT payload, SBWP CopyData/CopyDone, row visibility assertions) |
| `multimodel_statements` | ✅ ➕ | 092 (descriptor DDL + document/KV/graph/time-series/search/vector routes) |
| `database` | ✅ ➕ | 093 (create/alter/maintenance/repair/drop lifecycle) |
| `filespace` | ✅ ➕ | 093 (create/alter/show/storage/grow/verify) |
| `agent` | ✅ ➕ | 093 (create/show/alter agent routes) |
| `backup_restore_replication_migration` | ✅ ➕ | 093 (backup/restore/archive/replicate/changefeed/migration routes) |
| `management_and_operations` | ✅ ➕ | 093 (show management/config/listeners, config statements, session settings) |
| `catalog_artifacts_and_external_git` | ✅ ➕ | 093 (catalog artifact and external-git snapshot/diff/rollback-plan routes through the server ABI) |
| `refusal_vectors` | ➖ | concept page; mechanism exercised via `expected_refusals` (080, 011, 099) |
| `script_tokens_and_identifiers` | ➖ | lexical; implicitly exercised by every script |
| `README` | ➖ | index, not a statement page |

## Summary

- **Added this round:** 17 scripts (`011`,`012`,`013`,`014`,`015`,`045`,`052`,`053`,`054`,`056`,`057`,`058`,`059`,`086`,`092`,`093`,`099`) —
  141 executable assertions and 5 new expected-refusals. Suite validator and compiler pass.
- **Now covered:** DDL lifecycle (domain/sequence/view/schema-tree/alter), query-language
  completeness, expression-body functions, security-object DDL, cluster-gated refusals.
- **Runner requirement:** driver runners must use the shared SET TERM- and comment-aware
  statement chunker; otherwise the procedural scripts are split incorrectly and must fail.
- **Admin and multimodel route coverage:** `092` and `093` are positive connected-route scripts.
  If live execution rejects one of those routes, that is an implementation defect to fix in the
  engine/server path, not an accepted suite gap.
- **Catalog-surface note:** introspection assertions in `052`/`086` were corrected against the
  confirmed `sys.*` surfaces described in FINDINGS §D.
