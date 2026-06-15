

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_spatial.md -->

<a id="ch-language-reference-functional-reference-sb-spatial-md"></a>

# SB Spatial Functional Reference

Generation task: `sb_spatial`

Package namespace: `sb.spatial`

Bounded spatial geometry helpers for WKT, narrow GeoJSON, point WKB text, predicates, measurements, and construction.

## How To Read This Page

Provides bounded in-core spatial helpers for geometry construction, inspection, measurement, and predicates.

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
| scalar | 93 |

## Operation Reference

### `geom_collect`

**Purpose:** Performs the `geom collect` spatial helper on bounded geometry input.

**Call Forms:**

- `geom_collect(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select geom_collect(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_collect |
| UUID | 019dffbb-f000-759b-847c-9f3ed02b3742 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_geom_collect.v3 |
| AST binding | ast.expr.scalar_geom_collect |
| Engine entrypoint | geom_collect |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-geom-collect;SBSFC036-st-x-invalid`.

### `geom_collect_geometry`

**Purpose:** Performs the `geom collect geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `geom_collect(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the geom_collect_geometry.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select geom_collect(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_collect_geometry |
| UUID | 019dffbb-f002-7012-8a00-000000000012 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_geom_collect_geometry.v3 |
| AST binding | ast.expr.scalar_geom_collect_geometry |
| Engine entrypoint | geom_collect_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `geom_extent`

**Purpose:** Performs the `geom extent` spatial helper on bounded geometry input.

**Call Forms:**

- `geom_extent(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select geom_extent(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_extent |
| UUID | 019dffbb-f002-7021-8a00-000000000021 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_geom_extent.v3 |
| AST binding | ast.expr.scalar_geom_extent |
| Engine entrypoint | geom_extent |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-geom-extent;SBSFC036-st-x-invalid`.

### `geom_extent_geometry`

**Purpose:** Performs the `geom extent geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `geom_extent(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select geom_extent_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_extent_geometry |
| UUID | 019dffbb-f000-7597-a198-f3ea41f8b4c6 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_geom_extent_geometry.v3 |
| AST binding | ast.expr.scalar_geom_extent_geometry |
| Engine entrypoint | geom_extent_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-geom-extent;SBSFC036-st-x-invalid`.

### `geom_union`

**Purpose:** Performs the `geom union` spatial helper on bounded geometry input.

**Call Forms:**

- `geom_union(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select geom_union(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_union |
| UUID | 019dffbb-f002-7011-8a00-000000000011 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_geom_union.v3 |
| AST binding | ast.expr.scalar_geom_union |
| Engine entrypoint | geom_union |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-geom-union;SBSFC036-st-x-invalid`.

### `geom_union_geometry`

**Purpose:** Performs the `geom union geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `geom_union(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select geom_union_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_union_geometry |
| UUID | 019dffbb-f000-799f-9304-e40550ef7024 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_geom_union_geometry.v3 |
| AST binding | ast.expr.scalar_geom_union_geometry |
| Engine entrypoint | geom_union_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-geom-union;SBSFC036-st-x-invalid`.

### `st_area`

**Purpose:** Performs the `st area` spatial helper on bounded geometry input.

**Call Forms:**

- `st_area(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_area(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_area |
| UUID | 019dffbb-f002-7007-8a00-000000000007 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_area.v3 |
| AST binding | ast.expr.scalar_st_area |
| Engine entrypoint | st_area |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-area;SBSFC036-st-x-invalid`.

### `st_area_geometry`

**Purpose:** Performs the `st area geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_area(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_area_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_area_geometry |
| UUID | 019dffbb-f000-7341-91a0-334a4b21864e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_area_geometry.v3 |
| AST binding | ast.expr.scalar_st_area_geometry |
| Engine entrypoint | st_area_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-area;SBSFC036-st-x-invalid`.

### `st_asbinary`

**Purpose:** Performs the `st asbinary` spatial helper on bounded geometry input.

**Call Forms:**

- `st_asbinary(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_asbinary(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asbinary |
| UUID | 019dffbb-f000-7114-9a87-b2411f025c18 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_asbinary.v3 |
| AST binding | ast.expr.scalar_st_asbinary |
| Engine entrypoint | st_asbinary |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-asbinary;SBSFC036-st-x-invalid`.

### `st_asbinary_geometry`

**Purpose:** Performs the `st asbinary geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_asbinary(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_asbinary_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asbinary_geometry |
| UUID | 019dffbb-f000-77cb-b492-64a374f16a59 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_asbinary_geometry.v3 |
| AST binding | ast.expr.scalar_st_asbinary_geometry |
| Engine entrypoint | st_asbinary_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-asbinary-signature;SBSFC036-st-x-invalid`.

### `st_asgeojson`

**Purpose:** Performs the `st asgeojson` spatial helper on bounded geometry input.

**Call Forms:**

- `st_asgeojson(geometry[,maxdecimaldigits])`
- Syntax category: `function_call`

**Parameters:**

- `geometry[`: Bound using the declared descriptor rules for this overload.
- `maxdecimaldigits]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_asgeojson(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asgeojson |
| UUID | 019dffbb-f002-7023-8a00-000000000023 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_asgeojson.v3 |
| AST binding | ast.expr.scalar_st_asgeojson |
| Engine entrypoint | st_asgeojson |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-asgeojson;SBSFC036-st-x-invalid`.

### `st_asgeojson_geometry_maxdecimaldigits`

**Purpose:** Performs the `st asgeojson geometry maxdecimaldigits` spatial helper on bounded geometry input.

**Call Forms:**

- `st_asgeojson(geometry[,maxdecimaldigits])`
- Syntax category: `function_call`

**Parameters:**

- `geometry[`: Bound using the declared descriptor rules for this overload.
- `maxdecimaldigits]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_asgeojson_geometry_maxdecimaldigits(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asgeojson_geometry_maxdecimaldigits |
| UUID | 019dffbb-f000-7321-bd11-811d7b30c4ea |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_asgeojson_geometry_maxdecimaldigits.v3 |
| AST binding | ast.expr.scalar_st_asgeojson_geometry_maxdecimaldigits |
| Engine entrypoint | st_asgeojson_geometry_maxdecimaldigits |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-asgeojson;SBSFC036-st-x-invalid`.

### `st_asmvtgeom`

**Purpose:** Performs the `st asmvtgeom` spatial helper on bounded geometry input.

**Call Forms:**

- `st_asmvtgeom(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_asmvtgeom(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asmvtgeom |
| UUID | 019dffbb-f002-7008-8a00-000000000008 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_asmvtgeom.v3 |
| AST binding | ast.expr.scalar_st_asmvtgeom |
| Engine entrypoint | st_asmvtgeom |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-asmvtgeom;SBSFC038-st-geomfromtext-invalid`.

### `st_asmvtgeom_geometry_bbox_extent_buffer_clip`

**Purpose:** Performs the `st asmvtgeom geometry bbox extent buffer clip` spatial helper on bounded geometry input.

**Call Forms:**

- `st_asmvtgeom_geometry_bbox_extent_buffer_clip(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_asmvtgeom_geometry_bbox_extent_buffer_clip.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_asmvtgeom_geometry_bbox_extent_buffer_clip(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asmvtgeom_geometry_bbox_extent_buffer_clip |
| UUID | 019dffbb-f002-7043-8a00-000000000043 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_asmvtgeom_geometry_bbox_extent_buffer_clip.v3 |
| AST binding | ast.expr.scalar_st_asmvtgeom_geometry_bbox_extent_buffer_clip |
| Engine entrypoint | st_asmvtgeom_geometry_bbox_extent_buffer_clip |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_assvg`

**Purpose:** Performs the `st assvg` spatial helper on bounded geometry input.

**Call Forms:**

- `st_assvg(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_assvg(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_assvg |
| UUID | 019dffbb-f000-768d-bd20-e5f65f615cb7 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_assvg.v3 |
| AST binding | ast.expr.scalar_st_assvg |
| Engine entrypoint | st_assvg |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-assvg;SBSFC036-st-x-invalid`.

### `st_assvg_geometry`

**Purpose:** Performs the `st assvg geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_assvg(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_assvg_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_assvg_geometry |
| UUID | 019dffbb-f000-7fef-b05c-27c465af1bd4 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_assvg_geometry.v3 |
| AST binding | ast.expr.scalar_st_assvg_geometry |
| Engine entrypoint | st_assvg_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-assvg-signature;SBSFC036-st-x-invalid`.

### `st_astext`

**Purpose:** Performs the `st astext` spatial helper on bounded geometry input.

**Call Forms:**

- `st_astext(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_astext(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_astext |
| UUID | 019dffbb-f000-7eee-8972-19163693f63b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_astext.v3 |
| AST binding | ast.expr.scalar_st_astext |
| Engine entrypoint | st_astext |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-astext;SBSFC036-st-x-invalid`.

### `st_astext_geometry`

**Purpose:** Performs the `st astext geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_astext(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_astext_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_astext_geometry |
| UUID | 019dffbb-f000-7c52-9fd4-74b35cb69d96 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_astext_geometry.v3 |
| AST binding | ast.expr.scalar_st_astext_geometry |
| Engine entrypoint | st_astext_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-astext-signature;SBSFC036-st-x-invalid`.

### `st_buffer`

**Purpose:** Performs the `st buffer` spatial helper on bounded geometry input.

**Call Forms:**

- `st_buffer(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_buffer(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_buffer |
| UUID | 019dffbb-f000-799a-97a0-73b50e7f1dae |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_buffer.v3 |
| AST binding | ast.expr.scalar_st_buffer |
| Engine entrypoint | st_buffer |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-buffer;SBSFC036-st-x-invalid`.

### `st_buffer_geometry_distance`

**Purpose:** Performs the `st buffer geometry distance` spatial helper on bounded geometry input.

**Call Forms:**

- `st_buffer(geometry,distance)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- `distance`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_buffer_geometry_distance(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_buffer_geometry_distance |
| UUID | 019dffbb-f000-7569-9eb7-eee7184c8fab |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_buffer_geometry_distance.v3 |
| AST binding | ast.expr.scalar_st_buffer_geometry_distance |
| Engine entrypoint | st_buffer_geometry_distance |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-buffer-signature;SBSFC036-st-x-invalid`.

### `st_centroid`

**Purpose:** Performs the `st centroid` spatial helper on bounded geometry input.

**Call Forms:**

- `st_centroid(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_centroid(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_centroid |
| UUID | 019dffbb-f000-704c-8f3e-718258778236 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_centroid.v3 |
| AST binding | ast.expr.scalar_st_centroid |
| Engine entrypoint | st_centroid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-centroid;SBSFC036-st-x-invalid`.

### `st_centroid_geometry`

**Purpose:** Performs the `st centroid geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_centroid_geometry(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_centroid_geometry.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_centroid_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_centroid_geometry |
| UUID | 019dffbb-f002-7016-8a00-000000000016 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_centroid_geometry.v3 |
| AST binding | ast.expr.scalar_st_centroid_geometry |
| Engine entrypoint | st_centroid_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_contains`

**Purpose:** Performs the `st contains` spatial helper on bounded geometry input.

**Call Forms:**

- `st_contains(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_contains(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_contains |
| UUID | 019dffbb-f000-7b07-8acb-89483aca493f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_contains.v3 |
| AST binding | ast.expr.scalar_st_contains |
| Engine entrypoint | st_contains |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-contains;SBSFC036-st-x-invalid`.

### `st_contains_g1_g2`

**Purpose:** Performs the `st contains g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_contains(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_contains_g1_g2(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_contains_g1_g2 |
| UUID | 019dffbb-f000-7796-9fc8-bc433eb2f949 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_contains_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_contains_g1_g2 |
| Engine entrypoint | st_contains_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-contains-signature;SBSFC036-st-x-invalid`.

### `st_convexhull`

**Purpose:** Performs the `st convexhull` spatial helper on bounded geometry input.

**Call Forms:**

- `st_convexhull(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_convexhull(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_convexhull |
| UUID | 019dffbb-f002-7032-8a00-000000000032 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_convexhull.v3 |
| AST binding | ast.expr.scalar_st_convexhull |
| Engine entrypoint | st_convexhull |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-convexhull-geometry;SBSFC038-st-geomfromtext-invalid`.

### `st_convexhull_geometry`

**Purpose:** Performs the `st convexhull geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_convexhull_geometry(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_convexhull_geometry.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_convexhull_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_convexhull_geometry |
| UUID | 019dffbb-f002-7030-8a00-000000000030 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_convexhull_geometry.v3 |
| AST binding | ast.expr.scalar_st_convexhull_geometry |
| Engine entrypoint | st_convexhull_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_covers`

**Purpose:** Performs the `st covers` spatial helper on bounded geometry input.

**Call Forms:**

- `st_covers(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_covers(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_covers |
| UUID | 019dffbb-f002-7039-8a00-000000000039 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_covers.v3 |
| AST binding | ast.expr.scalar_st_covers |
| Engine entrypoint | st_covers |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-covers-signature;SBSFC038-st-geomfromtext-invalid`.

### `st_covers_g1_g2`

**Purpose:** Performs the `st covers g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_covers_g1_g2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_covers_g1_g2.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_covers_g1_g2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_covers_g1_g2 |
| UUID | 019dffbb-f002-7027-8a00-000000000027 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_covers_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_covers_g1_g2 |
| Engine entrypoint | st_covers_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_crosses`

**Purpose:** Performs the `st crosses` spatial helper on bounded geometry input.

**Call Forms:**

- `st_crosses(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_crosses(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_crosses |
| UUID | 019dffbb-f000-79fe-bda9-b3ff8ae8d7db |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_crosses.v3 |
| AST binding | ast.expr.scalar_st_crosses |
| Engine entrypoint | st_crosses |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-crosses;SBSFC036-st-x-invalid`.

### `st_crosses_g1_g2`

**Purpose:** Performs the `st crosses g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_crosses(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_crosses_g1_g2(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_crosses_g1_g2 |
| UUID | 019dffbb-f000-7ead-bdc4-ae2c3d42fbb8 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_crosses_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_crosses_g1_g2 |
| Engine entrypoint | st_crosses_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-crosses-signature;SBSFC036-st-x-invalid`.

### `st_difference`

**Purpose:** Performs the `st difference` spatial helper on bounded geometry input.

**Call Forms:**

- `st_difference(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_difference(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_difference |
| UUID | 019dffbb-f002-7009-8a00-000000000009 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_difference.v3 |
| AST binding | ast.expr.scalar_st_difference |
| Engine entrypoint | st_difference |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-difference-signature;SBSFC038-st-geomfromtext-invalid`.

### `st_difference_g1_g2`

**Purpose:** Performs the `st difference g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_difference_g1_g2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_difference_g1_g2.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_difference_g1_g2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_difference_g1_g2 |
| UUID | 019dffbb-f002-7005-8a00-000000000005 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_difference_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_difference_g1_g2 |
| Engine entrypoint | st_difference_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_disjoint`

**Purpose:** Performs the `st disjoint` spatial helper on bounded geometry input.

**Call Forms:**

- `st_disjoint(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_disjoint(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_disjoint |
| UUID | 019dffbb-f000-700c-8fbd-cea5012faf3f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_disjoint.v3 |
| AST binding | ast.expr.scalar_st_disjoint |
| Engine entrypoint | st_disjoint |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-disjoint;SBSFC036-st-x-invalid`.

### `st_disjoint_g1_g2`

**Purpose:** Performs the `st disjoint g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_disjoint_g1_g2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_disjoint_g1_g2.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_disjoint_g1_g2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_disjoint_g1_g2 |
| UUID | 019dffbb-f002-7029-8a00-000000000029 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_disjoint_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_disjoint_g1_g2 |
| Engine entrypoint | st_disjoint_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_distance`

**Purpose:** Performs the `st distance` spatial helper on bounded geometry input.

**Call Forms:**

- `st_distance(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_distance(st_makepoint(0.0, 0.0), st_makepoint(3.0, 4.0)) as distance;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_distance |
| UUID | 019dffbb-f000-70cc-960f-177f0b06f890 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_distance.v3 |
| AST binding | ast.expr.scalar_st_distance |
| Engine entrypoint | st_distance |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-distance;SBSFC036-st-x-invalid`.

### `st_distance_g1_g2`

**Purpose:** Performs the `st distance g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_distance(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_distance_g1_g2(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_distance_g1_g2 |
| UUID | 019dffbb-f000-7ae2-a9dd-4381816a119f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_distance_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_distance_g1_g2 |
| Engine entrypoint | st_distance_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-distance-signature;SBSFC036-st-x-invalid`.

### `st_dwithin`

**Purpose:** Performs the `st dwithin` spatial helper on bounded geometry input.

**Call Forms:**

- `st_dwithin(g1,g2,distance)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- `distance`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_dwithin(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_dwithin |
| UUID | 019dffbb-f002-7024-8a00-000000000024 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_dwithin.v3 |
| AST binding | ast.expr.scalar_st_dwithin |
| Engine entrypoint | st_dwithin |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-dwithin-signature;SBSFC038-st-geomfromtext-invalid`.

### `st_dwithin_g1_g2_distance`

**Purpose:** Performs the `st dwithin g1 g2 distance` spatial helper on bounded geometry input.

**Call Forms:**

- `st_dwithin_g1_g2_distance(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_dwithin_g1_g2_distance.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_dwithin_g1_g2_distance(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_dwithin_g1_g2_distance |
| UUID | 019dffbb-f002-7002-8a00-000000000002 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_dwithin_g1_g2_distance.v3 |
| AST binding | ast.expr.scalar_st_dwithin_g1_g2_distance |
| Engine entrypoint | st_dwithin_g1_g2_distance |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_envelope`

**Purpose:** Performs the `st envelope` spatial helper on bounded geometry input.

**Call Forms:**

- `st_envelope(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_envelope(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_envelope |
| UUID | 019dffbb-f000-7801-b48f-c90782cef66c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_envelope.v3 |
| AST binding | ast.expr.scalar_st_envelope |
| Engine entrypoint | st_envelope |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-envelope;SBSFC036-st-x-invalid`.

### `st_envelope_geometry`

**Purpose:** Performs the `st envelope geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_envelope(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_envelope_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_envelope_geometry |
| UUID | 019dffbb-f000-7db8-b038-25c899a67993 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_envelope_geometry.v3 |
| AST binding | ast.expr.scalar_st_envelope_geometry |
| Engine entrypoint | st_envelope_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-envelope-signature;SBSFC036-st-x-invalid`.

### `st_equals`

**Purpose:** Performs the `st equals` spatial helper on bounded geometry input.

**Call Forms:**

- `st_equals(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_equals(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_equals |
| UUID | 019dffbb-f002-7014-8a00-000000000014 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_equals.v3 |
| AST binding | ast.expr.scalar_st_equals |
| Engine entrypoint | st_equals |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-equals;SBSFC036-st-x-invalid`.

### `st_equals_g1_g2`

**Purpose:** Performs the `st equals g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_equals(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_equals_g1_g2(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_equals_g1_g2 |
| UUID | 019dffbb-f000-7bd7-939c-9670142ff560 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_equals_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_equals_g1_g2 |
| Engine entrypoint | st_equals_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-equals;SBSFC036-st-x-invalid`.

### `st_geogfromtext`

**Purpose:** Performs the `st geogfromtext` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geogfromtext(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geogfromtext(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geogfromtext |
| UUID | 019dffbb-f000-7868-ac24-4d0a09a7d7c0 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geogfromtext.v3 |
| AST binding | ast.expr.scalar_st_geogfromtext |
| Engine entrypoint | st_geogfromtext |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-geogfromtext;SBSFC036-st-x-invalid`.

### `st_geogfromtext_wkt`

**Purpose:** Performs the `st geogfromtext wkt` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geogfromtext_wkt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_geogfromtext_wkt.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_geogfromtext_wkt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geogfromtext_wkt |
| UUID | 019dffbb-f002-7041-8a00-000000000041 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geogfromtext_wkt.v3 |
| AST binding | ast.expr.scalar_st_geogfromtext_wkt |
| Engine entrypoint | st_geogfromtext_wkt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_geometrytype`

**Purpose:** Performs the `st geometrytype` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geometrytype(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geometrytype(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geometrytype |
| UUID | 019dffbb-f002-7017-8a00-000000000017 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geometrytype.v3 |
| AST binding | ast.expr.scalar_st_geometrytype |
| Engine entrypoint | st_geometrytype |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-geometrytype;SBSFC036-st-x-invalid`.

### `st_geometrytype_geometry`

**Purpose:** Performs the `st geometrytype geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geometrytype(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geometrytype_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geometrytype_geometry |
| UUID | 019dffbb-f000-77dd-ac54-85767abb9926 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geometrytype_geometry.v3 |
| AST binding | ast.expr.scalar_st_geometrytype_geometry |
| Engine entrypoint | st_geometrytype_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-geometrytype;SBSFC036-st-x-invalid`.

### `st_geomfromgeojson`

**Purpose:** Performs the `st geomfromgeojson` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geomfromgeojson(text)`
- Syntax category: `function_call`

**Parameters:**

- `text`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geomfromgeojson(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromgeojson |
| UUID | 019dffbb-f002-7018-8a00-000000000018 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geomfromgeojson.v3 |
| AST binding | ast.expr.scalar_st_geomfromgeojson |
| Engine entrypoint | st_geomfromgeojson |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-geomfromgeojson;SBSFC036-st-x-invalid`.

### `st_geomfromgeojson_text`

**Purpose:** Performs the `st geomfromgeojson text` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geomfromgeojson(text)`
- Syntax category: `function_call`

**Parameters:**

- `text`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geomfromgeojson_text(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromgeojson_text |
| UUID | 019dffbb-f000-72b5-b56a-b7993e3a9844 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geomfromgeojson_text.v3 |
| AST binding | ast.expr.scalar_st_geomfromgeojson_text |
| Engine entrypoint | st_geomfromgeojson_text |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-geomfromgeojson;SBSFC036-st-x-invalid`.

### `st_geomfromtext`

**Purpose:** Performs the `st geomfromtext` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geomfromtext(wkt[,srid])`
- Syntax category: `function_call`

**Parameters:**

- `wkt[`: Bound using the declared descriptor rules for this overload.
- `srid]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geomfromtext(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromtext |
| UUID | 019dffbb-f002-7036-8a00-000000000036 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geomfromtext.v3 |
| AST binding | ast.expr.scalar_st_geomfromtext |
| Engine entrypoint | st_geomfromtext |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-geomfromtext-signature;SBSFC038-st-geomfromtext-invalid`.

### `st_geomfromtext_wkt_srid`

**Purpose:** Performs the `st geomfromtext wkt srid` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geomfromtext_wkt_srid(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_geomfromtext_wkt_srid.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_geomfromtext_wkt_srid(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromtext_wkt_srid |
| UUID | 019dffbb-f002-7020-8a00-000000000020 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geomfromtext_wkt_srid.v3 |
| AST binding | ast.expr.scalar_st_geomfromtext_wkt_srid |
| Engine entrypoint | st_geomfromtext_wkt_srid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_geomfromwkb`

**Purpose:** Performs the `st geomfromwkb` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geomfromwkb(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geomfromwkb(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromwkb |
| UUID | 019dffbb-f000-76ee-a6f7-dcfc7057e7e9 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geomfromwkb.v3 |
| AST binding | ast.expr.scalar_st_geomfromwkb |
| Engine entrypoint | st_geomfromwkb |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-geomfromwkb;SBSFC036-st-x-invalid`.

### `st_geomfromwkb_wkb_srid`

**Purpose:** Performs the `st geomfromwkb wkb srid` spatial helper on bounded geometry input.

**Call Forms:**

- `st_geomfromwkb(wkb[,srid])`
- Syntax category: `function_call`

**Parameters:**

- `wkb[`: Bound using the declared descriptor rules for this overload.
- `srid]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_geomfromwkb_wkb_srid(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromwkb_wkb_srid |
| UUID | 019dffbb-f000-7b44-a0c1-6bcaca00d38a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_geomfromwkb_wkb_srid.v3 |
| AST binding | ast.expr.scalar_st_geomfromwkb_wkb_srid |
| Engine entrypoint | st_geomfromwkb_wkb_srid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-geomfromwkb-signature;SBSFC036-st-x-invalid`.

### `st_intersection`

**Purpose:** Performs the `st intersection` spatial helper on bounded geometry input.

**Call Forms:**

- `st_intersection(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_intersection(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersection |
| UUID | 019dffbb-f000-7c44-ae0e-b6225140db16 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_intersection.v3 |
| AST binding | ast.expr.scalar_st_intersection |
| Engine entrypoint | st_intersection |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-intersection;SBSFC036-st-x-invalid`.

### `st_intersection_g1_g2`

**Purpose:** Performs the `st intersection g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_intersection_g1_g2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_intersection_g1_g2.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_intersection_g1_g2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersection_g1_g2 |
| UUID | 019dffbb-f002-7015-8a00-000000000015 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_intersection_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_intersection_g1_g2 |
| Engine entrypoint | st_intersection_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_intersects`

**Purpose:** Performs the `st intersects` spatial helper on bounded geometry input.

**Call Forms:**

- `st_intersects(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_intersects(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersects |
| UUID | 019dffbb-f000-776b-a82f-edc543ad8574 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_intersects.v3 |
| AST binding | ast.expr.scalar_st_intersects |
| Engine entrypoint | st_intersects |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-intersects;SBSFC036-st-x-invalid`.

### `st_intersects_g1_g2`

**Purpose:** Performs the `st intersects g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_intersects(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_intersects_g1_g2(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersects_g1_g2 |
| UUID | 019dffbb-f000-70c2-86a8-116036bfee0a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_intersects_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_intersects_g1_g2 |
| Engine entrypoint | st_intersects_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-intersects-signature;SBSFC036-st-x-invalid`.

### `st_length`

**Purpose:** Performs the `st length` spatial helper on bounded geometry input.

**Call Forms:**

- `st_length(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_length(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_length |
| UUID | 019dffbb-f002-7010-8a00-000000000010 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_length.v3 |
| AST binding | ast.expr.scalar_st_length |
| Engine entrypoint | st_length |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-length;SBSFC038-st-geomfromtext-invalid`.

### `st_length_geometry`

**Purpose:** Performs the `st length geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_length_geometry(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_length_geometry.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_length_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_length_geometry |
| UUID | 019dffbb-f002-7031-8a00-000000000031 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_length_geometry.v3 |
| AST binding | ast.expr.scalar_st_length_geometry |
| Engine entrypoint | st_length_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_m`

**Purpose:** Performs the `st m` spatial helper on bounded geometry input.

**Call Forms:**

- `st_m(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_m(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_m |
| UUID | 019dffbb-f002-7003-8a00-000000000003 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_m.v3 |
| AST binding | ast.expr.scalar_st_m |
| Engine entrypoint | st_m |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-m;SBSFC038-st-geomfromtext-invalid`.

### `st_makeline`

**Purpose:** Performs the `st makeline` spatial helper on bounded geometry input.

**Call Forms:**

- `st_makeline(point,point\|array)`
- Syntax category: `function_call`

**Parameters:**

- `point`: Bound using the declared descriptor rules for this overload.
- `point\|array`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_makeline(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makeline |
| UUID | 019dffbb-f002-7034-8a00-000000000034 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_makeline.v3 |
| AST binding | ast.expr.scalar_st_makeline |
| Engine entrypoint | st_makeline |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-makeline-signature;SBSFC038-st-geomfromtext-invalid`.

### `st_makeline_point_point_array`

**Purpose:** Performs the `st makeline point point array` spatial helper on bounded geometry input.

**Call Forms:**

- `st_makeline_point_point_array(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_makeline_point_point_array.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_makeline_point_point_array(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makeline_point_point_array |
| UUID | 019dffbb-f002-7019-8a00-000000000019 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_makeline_point_point_array.v3 |
| AST binding | ast.expr.scalar_st_makeline_point_point_array |
| Engine entrypoint | st_makeline_point_point_array |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_makepoint`

**Purpose:** Performs the `st makepoint` spatial helper on bounded geometry input.

**Call Forms:**

- `st_makepoint(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_makepoint(10.0, 20.0) as point_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepoint |
| UUID | 019dffbb-f000-7dc2-b9e7-8a2bbe620fd2 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_makepoint.v3 |
| AST binding | ast.expr.scalar_st_makepoint |
| Engine entrypoint | st_makepoint |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-makepoint;SBSFC036-st-x-invalid`.

### `st_makepoint_x_y_z_m`

**Purpose:** Performs the `st makepoint x y z m` spatial helper on bounded geometry input.

**Call Forms:**

- `st_makepoint_x_y_z_m(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_makepoint_x_y_z_m.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_makepoint_x_y_z_m(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepoint_x_y_z_m |
| UUID | 019dffbb-f002-7013-8a00-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_makepoint_x_y_z_m.v3 |
| AST binding | ast.expr.scalar_st_makepoint_x_y_z_m |
| Engine entrypoint | st_makepoint_x_y_z_m |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_makepolygon`

**Purpose:** Performs the `st makepolygon` spatial helper on bounded geometry input.

**Call Forms:**

- `st_makepolygon(linestring[,holesarray])`
- Syntax category: `function_call`

**Parameters:**

- `linestring[`: Bound using the declared descriptor rules for this overload.
- `holesarray]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_makepolygon(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepolygon |
| UUID | 019dffbb-f002-7035-8a00-000000000035 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_makepolygon.v3 |
| AST binding | ast.expr.scalar_st_makepolygon |
| Engine entrypoint | st_makepolygon |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-makepolygon;SBSFC036-st-x-invalid`.

### `st_makepolygon_linestring_holesarray`

**Purpose:** Performs the `st makepolygon linestring holesarray` spatial helper on bounded geometry input.

**Call Forms:**

- `st_makepolygon(linestring[,holesarray])`
- Syntax category: `function_call`

**Parameters:**

- `linestring[`: Bound using the declared descriptor rules for this overload.
- `holesarray]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_makepolygon_linestring_holesarray(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepolygon_linestring_holesarray |
| UUID | 019dffbb-f000-7d31-8c78-4b381463878a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_makepolygon_linestring_holesarray.v3 |
| AST binding | ast.expr.scalar_st_makepolygon_linestring_holesarray |
| Engine entrypoint | st_makepolygon_linestring_holesarray |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-makepolygon;SBSFC036-st-x-invalid`.

### `st_npoints`

**Purpose:** Performs the `st npoints` spatial helper on bounded geometry input.

**Call Forms:**

- `st_npoints(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_npoints(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_npoints |
| UUID | 019dffbb-f000-7cf2-ad7c-7d46b446d664 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_npoints.v3 |
| AST binding | ast.expr.scalar_st_npoints |
| Engine entrypoint | st_npoints |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-npoints;SBSFC036-st-x-invalid`.

### `st_npoints_geometry`

**Purpose:** Performs the `st npoints geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_npoints_geometry(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_npoints_geometry.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_npoints_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_npoints_geometry |
| UUID | 019dffbb-f002-7033-8a00-000000000033 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_npoints_geometry.v3 |
| AST binding | ast.expr.scalar_st_npoints_geometry |
| Engine entrypoint | st_npoints_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_numpoints`

**Purpose:** Performs the `st numpoints` spatial helper on bounded geometry input.

**Call Forms:**

- `st_numpoints(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_numpoints(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_numpoints |
| UUID | 019dffbb-f000-7c6d-bf11-10d57aa799e0 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_numpoints.v3 |
| AST binding | ast.expr.scalar_st_numpoints |
| Engine entrypoint | st_numpoints |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-numpoints;SBSFC036-st-x-invalid`.

### `st_numpoints_geometry`

**Purpose:** Performs the `st numpoints geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_numpoints(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_numpoints_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_numpoints_geometry |
| UUID | 019dffbb-f000-7daf-9cac-8bc57ad35954 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_numpoints_geometry.v3 |
| AST binding | ast.expr.scalar_st_numpoints_geometry |
| Engine entrypoint | st_numpoints_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-numpoints-signature;SBSFC036-st-x-invalid`.

### `st_overlaps`

**Purpose:** Performs the `st overlaps` spatial helper on bounded geometry input.

**Call Forms:**

- `st_overlaps(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_overlaps(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_overlaps |
| UUID | 019dffbb-f000-78be-81ec-61a6d1cc9c74 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_overlaps.v3 |
| AST binding | ast.expr.scalar_st_overlaps |
| Engine entrypoint | st_overlaps |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-overlaps;SBSFC036-st-x-invalid`.

### `st_overlaps_g1_g2`

**Purpose:** Performs the `st overlaps g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_overlaps_g1_g2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_overlaps_g1_g2.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_overlaps_g1_g2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_overlaps_g1_g2 |
| UUID | 019dffbb-f002-7004-8a00-000000000004 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_overlaps_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_overlaps_g1_g2 |
| Engine entrypoint | st_overlaps_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_perimeter`

**Purpose:** Performs the `st perimeter` spatial helper on bounded geometry input.

**Call Forms:**

- `st_perimeter(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_perimeter(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_perimeter |
| UUID | 019dffbb-f000-7f31-a657-c857f289e81c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_perimeter.v3 |
| AST binding | ast.expr.scalar_st_perimeter |
| Engine entrypoint | st_perimeter |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-perimeter;SBSFC036-st-x-invalid`.

### `st_perimeter_geometry`

**Purpose:** Performs the `st perimeter geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_perimeter(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_perimeter_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_perimeter_geometry |
| UUID | 019dffbb-f000-7dcd-8c79-2345c86a1ff1 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_perimeter_geometry.v3 |
| AST binding | ast.expr.scalar_st_perimeter_geometry |
| Engine entrypoint | st_perimeter_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-perimeter-signature;SBSFC036-st-x-invalid`.

### `st_setsrid`

**Purpose:** Performs the `st setsrid` spatial helper on bounded geometry input.

**Call Forms:**

- `st_setsrid(geometry,srid)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- `srid`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_setsrid(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_setsrid |
| UUID | 019dffbb-f002-7001-8a00-000000000001 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_setsrid.v3 |
| AST binding | ast.expr.scalar_st_setsrid |
| Engine entrypoint | st_setsrid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-setsrid;SBSFC036-st-x-invalid`.

### `st_setsrid_geometry_srid`

**Purpose:** Performs the `st setsrid geometry srid` spatial helper on bounded geometry input.

**Call Forms:**

- `st_setsrid(geometry,srid)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- `srid`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_setsrid_geometry_srid(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_setsrid_geometry_srid |
| UUID | 019dffbb-f000-7b35-b88b-e0324f8946ee |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_setsrid_geometry_srid.v3 |
| AST binding | ast.expr.scalar_st_setsrid_geometry_srid |
| Engine entrypoint | st_setsrid_geometry_srid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-setsrid;SBSFC036-st-x-invalid`.

### `st_simplify`

**Purpose:** Performs the `st simplify` spatial helper on bounded geometry input.

**Call Forms:**

- `st_simplify(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_simplify(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_simplify |
| UUID | 019dffbb-f000-7bd7-97f4-f66f545606cd |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_simplify.v3 |
| AST binding | ast.expr.scalar_st_simplify |
| Engine entrypoint | st_simplify |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-simplify;SBSFC036-st-x-invalid`.

### `st_simplify_geometry_tolerance`

**Purpose:** Performs the `st simplify geometry tolerance` spatial helper on bounded geometry input.

**Call Forms:**

- `st_simplify(geometry,tolerance)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- `tolerance`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_simplify_geometry_tolerance(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_simplify_geometry_tolerance |
| UUID | 019dffbb-f000-774b-a0c7-2289630c180c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_simplify_geometry_tolerance.v3 |
| AST binding | ast.expr.scalar_st_simplify_geometry_tolerance |
| Engine entrypoint | st_simplify_geometry_tolerance |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-simplify-signature;SBSFC036-st-x-invalid`.

### `st_srid`

**Purpose:** Performs the `st srid` spatial helper on bounded geometry input.

**Call Forms:**

- `st_srid(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_srid(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_srid |
| UUID | 019dffbb-f002-7028-8a00-000000000028 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_srid.v3 |
| AST binding | ast.expr.scalar_st_srid |
| Engine entrypoint | st_srid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-srid;SBSFC036-st-x-invalid`.

### `st_srid_geometry`

**Purpose:** Performs the `st srid geometry` spatial helper on bounded geometry input.

**Call Forms:**

- `st_srid(geometry)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_srid_geometry(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_srid_geometry |
| UUID | 019dffbb-f000-7043-ad9b-036862729b51 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_srid_geometry.v3 |
| AST binding | ast.expr.scalar_st_srid_geometry |
| Engine entrypoint | st_srid_geometry |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-srid;SBSFC036-st-x-invalid`.

### `st_symdifference`

**Purpose:** Performs the `st symdifference` spatial helper on bounded geometry input.

**Call Forms:**

- `st_symdifference(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_symdifference(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_symdifference |
| UUID | 019dffbb-f002-7038-8a00-000000000038 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_symdifference.v3 |
| AST binding | ast.expr.scalar_st_symdifference |
| Engine entrypoint | st_symdifference |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-symdifference-signature;SBSFC038-st-geomfromtext-invalid`.

### `st_symdifference_g1_g2`

**Purpose:** Performs the `st symdifference g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_symdifference_g1_g2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_symdifference_g1_g2.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_symdifference_g1_g2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_symdifference_g1_g2 |
| UUID | 019dffbb-f002-7022-8a00-000000000022 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_symdifference_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_symdifference_g1_g2 |
| Engine entrypoint | st_symdifference_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_touches`

**Purpose:** Performs the `st touches` spatial helper on bounded geometry input.

**Call Forms:**

- `st_touches(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_touches(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_touches |
| UUID | 019dffbb-f002-7025-8a00-000000000025 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_touches.v3 |
| AST binding | ast.expr.scalar_st_touches |
| Engine entrypoint | st_touches |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-touches;SBSFC036-st-x-invalid`.

### `st_touches_g1_g2`

**Purpose:** Performs the `st touches g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_touches(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_touches_g1_g2(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_touches_g1_g2 |
| UUID | 019dffbb-f000-7293-a7b2-2c424b503190 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_touches_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_touches_g1_g2 |
| Engine entrypoint | st_touches_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-touches;SBSFC036-st-x-invalid`.

### `st_transform`

**Purpose:** Performs the `st transform` spatial helper on bounded geometry input.

**Call Forms:**

- `st_transform(geometry,target_srid)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- `target_srid`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_transform(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_transform |
| UUID | 019dffbb-f002-7026-8a00-000000000026 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_transform.v3 |
| AST binding | ast.expr.scalar_st_transform |
| Engine entrypoint | st_transform |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-transform;SBSFC036-st-x-invalid`.

### `st_transform_geometry_target_srid`

**Purpose:** Performs the `st transform geometry target srid` spatial helper on bounded geometry input.

**Call Forms:**

- `st_transform(geometry,target_srid)`
- Syntax category: `function_call`

**Parameters:**

- `geometry`: Bound using the declared descriptor rules for this overload.
- `target_srid`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_transform_geometry_target_srid(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_transform_geometry_target_srid |
| UUID | 019dffbb-f000-717b-b421-4f72a9d57a45 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_transform_geometry_target_srid.v3 |
| AST binding | ast.expr.scalar_st_transform_geometry_target_srid |
| Engine entrypoint | st_transform_geometry_target_srid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-transform;SBSFC036-st-x-invalid`.

### `st_union`

**Purpose:** Performs the `st union` spatial helper on bounded geometry input.

**Call Forms:**

- `st_union(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_union(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_union |
| UUID | 019dffbb-f002-7040-8a00-000000000040 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_union.v3 |
| AST binding | ast.expr.scalar_st_union |
| Engine entrypoint | st_union |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-union;SBSFC038-st-geomfromtext-invalid`.

### `st_union_g1_g2`

**Purpose:** Performs the `st union g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_union_g1_g2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: the descriptor implicit cast matrix is applied; invalid or ambiguous casts are refused by the function guard.
- NULL handling: the selected overload handles NULL and descriptor edge cases according to its documented function rule.

**Returns:**

the descriptor-selected result produced by the st_union_g1_g2.

**Behavior:**

- Volatility: runtime-selected.
- Determinism: follows the selected overload; stable inputs produce stable results when the function volatility permits it.
- Side effects: none unless the function-specific behavior says otherwise.
- Collation/charset: text descriptors use their bound character set and collation where text comparison or rendering is involved.
- Timezone: temporal descriptors use session timezone rules where temporal conversion or rendering is involved.
- Security and authority: uses the registered SBsql authority rules for spatial.

**Errors:**

invalid arity, type, domain, policy, or descriptor inputs are reported through SBsql message vectors.

**Example:**

```sql
select st_union_g1_g2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_union_g1_g2 |
| UUID | 019dffbb-f002-7042-8a00-000000000042 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_union_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_union_g1_g2 |
| Engine entrypoint | st_union_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

### `st_within`

**Purpose:** Performs the `st within` spatial helper on bounded geometry input.

**Call Forms:**

- `st_within(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_within(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_within |
| UUID | 019dffbb-f002-7037-8a00-000000000037 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_within.v3 |
| AST binding | ast.expr.scalar_st_within |
| Engine entrypoint | st_within |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-within;SBSFC036-st-x-invalid`.

### `st_within_g1_g2`

**Purpose:** Performs the `st within g1 g2` spatial helper on bounded geometry input.

**Call Forms:**

- `st_within(g1,g2)`
- Syntax category: `function_call`

**Parameters:**

- `g1`: Bound using the declared descriptor rules for this overload.
- `g2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_within_g1_g2(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_within_g1_g2 |
| UUID | 019dffbb-f000-7684-bd0e-dfd71b44d2f7 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_within_g1_g2.v3 |
| AST binding | ast.expr.scalar_st_within_g1_g2 |
| Engine entrypoint | st_within_g1_g2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-within;SBSFC036-st-x-invalid`.

### `st_x`

**Purpose:** Performs the `st x` spatial helper on bounded geometry input.

**Call Forms:**

- `st_x(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_x(st_makepoint(10.0, 20.0)) as x_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_x |
| UUID | 019dffbb-f000-7273-aa82-e684d9b32184 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_x.v3 |
| AST binding | ast.expr.scalar_st_x |
| Engine entrypoint | st_x |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-x;SBSFC036-st-x-invalid`.

### `st_x_point`

**Purpose:** Performs the `st x point` spatial helper on bounded geometry input.

**Call Forms:**

- `st_x(point)`
- Syntax category: `function_call`

**Parameters:**

- `point`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_x_point(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_x_point |
| UUID | 019dffbb-f000-749e-a8e4-ecd9f14ac67f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_x_point.v3 |
| AST binding | ast.expr.scalar_st_x_point |
| Engine entrypoint | st_x_point |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-x-point;SBSFC036-st-x-invalid`.

### `st_y`

**Purpose:** Performs the `st y` spatial helper on bounded geometry input.

**Call Forms:**

- `st_y(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_y(st_makepoint(10.0, 20.0)) as y_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_y |
| UUID | 019dffbb-f000-7dfa-b709-97ed7fc33a6c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_y.v3 |
| AST binding | ast.expr.scalar_st_y |
| Engine entrypoint | st_y |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC036-st-y;SBSFC036-st-x-invalid`.

### `st_z`

**Purpose:** Performs the `st z` spatial helper on bounded geometry input.

**Call Forms:**

- `st_z(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core spatial scalar helper arguments: WKT geometry descriptors, POINT/LINESTRING/POLYGON/GEOMETRYCOLLECTION text, narrow GeoJSON text, point WKB hex text, numeric SRID/tolerance/distance, or point coordinates.
- Coercion: accepts only deterministic bounded textual geometry descriptors, narrow GeoJSON, point WKB hex, and scalar numeric helper arguments; no external dialect or catalog coercion.
- NULL handling: SQL null input returns SQL null using the target descriptor where applicable.

**Returns:**

spatial geometry scalar helper returns deterministic bounded geometry, character, boolean, int64, real64, bytea, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual geometry and GeoJSON output; boolean/numeric helpers use native scalar descriptors.
- Timezone: not applicable.
- Security and authority: pure bounded spatial geometry scalar helper; deterministic in-core WKT, POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, narrow GeoJSON, and point WKB hex route; no parser SQL execution, external plugin call, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, malformed geometry, or unsupported WKB refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select st_z(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_z |
| UUID | 019dffbb-f002-7006-8a00-000000000006 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_st_z.v3 |
| AST binding | ast.expr.scalar_st_z |
| Engine entrypoint | st_z |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC038-st-z;SBSFC038-st-geomfromtext-invalid`.

