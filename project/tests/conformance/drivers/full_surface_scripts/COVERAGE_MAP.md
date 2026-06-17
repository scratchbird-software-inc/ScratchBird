# Full-Surface Suite — Coverage Map (syntax_reference pages → tests)

Status: round-2 (language-surface extension)
Maps every `docs/documentation/draft/Language_Reference/syntax_reference/*.md` page to the
script(s) that exercise it with **executable** assertions, or marks it blocked/out-of-scope.

Legend: ✅ covered · ➕ added this round · ⛔ harness-blocked (see FINDINGS §A) · ⏳ future round · ➖ not a statement page

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
| `function` | ✅ ➕ | 052 (expression-body create/invoke/introspect/drop); 050/160 (builtin invocation). Procedural-body form ⛔ |
| `procedure` | ⛔ | harness cannot carry multi-statement bodies (FINDINGS §A) |
| `trigger` | ⛔ | multi-statement bodies (FINDINGS §A) |
| `procedural_sql` | ⛔ | only exercisable inside a routine body |
| `procedural_sql_blocks` | ⛔ | " |
| `procedural_sql_control_flow` | ⛔ | " |
| `procedural_sql_cursors` | ⛔ | " |
| `procedural_sql_exceptions` | ⛔ | " |
| `procedural_sql_triggers_and_events` | ⛔ | " |
| `copy` | ⏳ | in-band COPY — future round |
| `multimodel_statements` | ⏳ | needs backing-object CREATE DDL confirmed (cf. example-DB findings) |
| `database` | ⏳ | admin; limited in a connected-driver context |
| `filespace` | ⏳ | admin |
| `agent` | ⏳ | admin |
| `backup_restore_replication_migration` | ⏳ | admin |
| `management_and_operations` | ⏳ | admin |
| `catalog_artifacts_and_external_git` | ⏳ | admin |
| `refusal_vectors` | ➖ | concept page; mechanism exercised via `expected_refusals` (080, 011, 099) |
| `script_tokens_and_identifiers` | ➖ | lexical; implicitly exercised by every script |
| `README` | ➖ | index, not a statement page |

## Summary

- **Added this round:** 9 scripts (`011`,`012`,`013`,`014`,`015`,`045`,`052`,`086`,`099`) —
  89 executable assertions and 5 new expected-refusals. Suite validator and compiler pass.
- **Now covered:** DDL lifecycle (domain/sequence/view/schema-tree/alter), query-language
  completeness, expression-body functions, security-object DDL, cluster-gated refusals.
- **Blocked by the harness:** the entire procedural-SQL surface (procedures, multi-statement
  functions/triggers) — see FINDINGS §A; the fix is a terminator-aware / block-aware splitter
  in the driver runners.
- **Future rounds (⏳):** COPY, multimodel statements, and the admin statement families.
- **Caveat:** some introspection assertions in `052`/`086` use unconfirmed `sys.*` surface
  names — see FINDINGS §D.
