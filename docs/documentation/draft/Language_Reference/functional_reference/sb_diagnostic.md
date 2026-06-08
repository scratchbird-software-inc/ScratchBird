# Sb Diagnostic Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_diagnostic`


## Package Boundary

`sb.diagnostic` contains 1 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 1 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `row_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.fn.diagnostic.row_count |
| UUID | 019dffbb-f000-7c13-930f-6452499bdc47 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | row_count() |
| Return Type Rule | last row count from SBLR execution context as uint64; descriptor=uint64 |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_statement |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.row_count.v3 |
| AST Binding | ast.expr.row_count |
| Engine Entrypoint | row_count |
| Security Policy | reads diagnostic row-count context only; no DML fetch or storage authority |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select row_count() from app.sample_values;
```

