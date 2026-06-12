# SB Diagnostic Functional Reference

Generation task: `sb_diagnostic`

Package namespace: `sb.fn.diagnostic`

Statement and procedural diagnostic helpers, including row-count context.

## How To Read This Page

Reads diagnostic context produced by the current statement or procedural frame.

Each entry below is written for a user reading SBsql, not for a registry maintainer. The technical fields are retained so an operator can connect the language surface to SBLR and engine diagnostics when troubleshooting.

Privileges, policy admission, sandboxing, and descriptor compatibility are still checked by the surrounding statement. A function being listed here does not grant access to catalog objects, protected material, files, network targets, or external services.

Every operation entry includes:

- `Purpose`: what the operation is for.
- `Call forms`: the public spelling or overload shapes recognized by SBsql.
- `Parameters`: the argument roles and descriptor/coercion rules.
- `Returns`: the result descriptor and value rule.
- `Behavior`: NULL, volatility, collation, timezone, side-effect, and execution notes.
- `Errors`: the message-vector conditions raised for invalid input or denied execution.
- `Example`: a representative SBsql usage shape. Examples use ordinary schema names such as `app.orders` and are meant to show the function form, not prescribe a schema.

## Package Inventory

| Kind | Records |
| --- | ---: |
| scalar | 1 |

## Operation Reference

### `row_count`

**Purpose:** Returns the row-count value recorded for the current statement or procedural context.

**Call Forms:**

- `row_count()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Descriptor rule: descriptor_authoritative context/procedural arguments bound by parser/lowering route.
- Coercion: no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted.
- NULL handling: context-backed typed null or diagnostic behavior follows the conformance fixtures row evidence.

**Returns:**

last row count from SBLR execution context as uint64; descriptor=uint64.

**Behavior:**

- Volatility: stable_per_statement.
- Determinism: not foldable for execution-context reads or procedural diagnostics.
- Side effects: none.
- Collation/charset: character descriptors use byte-stable ASCII fixture values unless context supplies text.
- Timezone: not applicable unless the context value is timezone-derived.
- Security and authority: reads diagnostic row-count context only; no DML fetch or storage authority.

**Errors:**

arity/type/context/procedural diagnostics use builtin error compatibility matrix and the conformance fixtures evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable.

**Example:**

```sql
insert into app.audit_log(message) values ('created');
select row_count() as affected_rows;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.fn.diagnostic.row_count |
| UUID | 019dffbb-f000-7c13-930f-6452499bdc47 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.row_count.v3 |
| AST binding | ast.expr.row_count |
| Engine entrypoint | row_count |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC016-row-count-context`.
