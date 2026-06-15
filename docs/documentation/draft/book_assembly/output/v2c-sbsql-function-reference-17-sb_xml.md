

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_xml.md -->

<a id="ch-language-reference-functional-reference-sb-xml-md"></a>

# SB XML Functional Reference

Generation task: `sb_xml`

Package namespace: `sb.xml`

Bounded XML construction, query, serialization, validation, and XML-to-row helpers.

## How To Read This Page

Creates, parses, queries, and serializes bounded XML values.

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
| scalar | 20 |

## Operation Reference

### `xml_attrs`

**Purpose:** Performs the `xml attrs` XML helper on bounded XML input.

**Call Forms:**

- `xml.attrs(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xml.attrs(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.attrs |
| UUID | 019dffbb-f003-7008-8a00-000000000008 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_attrs.v3 |
| AST binding | ast.expr.xml_attrs |
| Engine entrypoint | xml_attrs |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xml-attrs;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xml_ns`

**Purpose:** Performs the `xml ns` XML helper on bounded XML input.

**Call Forms:**

- `xml.ns(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xml.ns(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.ns |
| UUID | 019dffbb-f003-7009-8a00-000000000009 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_ns.v3 |
| AST binding | ast.expr.xml_ns |
| Engine entrypoint | xml_ns |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xml-ns;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xmlagg`

**Purpose:** Performs the `xmlagg` XML helper on bounded XML input.

**Call Forms:**

- `xmlagg(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlagg(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.agg |
| UUID | 019dffbb-f000-7b98-9ff9-18050f0ac8ac |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_agg.v3 |
| AST binding | ast.expr.xml_agg |
| Engine entrypoint | xmlagg |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlagg;SBSFC037-xmlcomment-invalid`.

### `xmlattributes`

**Purpose:** Performs the `xmlattributes` XML helper on bounded XML input.

**Call Forms:**

- `xmlattributes(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlattributes(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.attributes |
| UUID | 019dffbb-f000-7403-9b7d-be5431e418ed |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_attributes.v3 |
| AST binding | ast.expr.xml_attributes |
| Engine entrypoint | xmlattributes |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlattributes;SBSFC037-xmlcomment-invalid`.

### `xmlcast`

**Purpose:** Performs the `xmlcast` XML helper on bounded XML input.

**Call Forms:**

- `XMLCAST(exprAStype)`
- Syntax category: `function_call`

**Parameters:**

- `exprAStype`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlcast(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.cast |
| UUID | 019dffbb-f000-7b48-afa6-134b032a0e11 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_cast.v3 |
| AST binding | ast.expr.xml_cast |
| Engine entrypoint | xmlcast |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlcast-as;SBSFC037-xmlcomment-invalid`.

### `xmlcomment`

**Purpose:** Creates an XML comment from safe text content.

**Call Forms:**

- `xmlcomment(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlcomment('generated by SBsql') as comment_node;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.comment |
| UUID | 019dffbb-f000-7830-a706-102315c7a34a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_comment.v3 |
| AST binding | ast.expr.xml_comment |
| Engine entrypoint | xmlcomment |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlcomment;SBSFC037-xmlcomment-invalid`.

### `xmlconcat`

**Purpose:** Concatenates XML fragments.

**Call Forms:**

- `XMLCONCAT(expr_list)`
- Syntax category: `function_call`

**Parameters:**

- `expr_list`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlconcat(xmlelement('a', '1'), xmlelement('b', '2')) as xml_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.concat |
| UUID | 019dffbb-f000-7970-825c-8ace8ab8b77e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_concat.v3 |
| AST binding | ast.expr.xml_concat |
| Engine entrypoint | xmlconcat |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlconcat-list;SBSFC037-xmlcomment-invalid`.

### `xmldocument`

**Purpose:** Builds or validates a bounded XML document value.

**Call Forms:**

- `xmldocument(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmldocument(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.document |
| UUID | 019dffbb-f003-7001-8a00-000000000001 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_document.v3 |
| AST binding | ast.expr.xml_document |
| Engine entrypoint | xmldocument |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xmldocument-bare;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xmlelement`

**Purpose:** Builds an XML element from a safe name and content values.

**Call Forms:**

- `XMLELEMENT(NAMEname[,namespaces][,attrs][,content_list])`
- Syntax category: `function_call`

**Parameters:**

- `NAMEname[`: Bound using the declared descriptor rules for this overload.
- `namespaces][`: Bound using the declared descriptor rules for this overload.
- `attrs][`: Bound using the declared descriptor rules for this overload.
- `content_list]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlelement('person', 'Ada') as xml_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.element |
| UUID | 019dffbb-f000-7ecd-ac64-30f2e5814c94 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_element.v3 |
| AST binding | ast.expr.xml_element |
| Engine entrypoint | xmlelement |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlelement-name;SBSFC037-xmlcomment-invalid`.

### `xmlexists`

**Purpose:** Returns whether an XML query finds a value.

**Call Forms:**

- `xmlexists(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlexists(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.exists |
| UUID | 019dffbb-f000-7f8e-8d18-5a968f021f07 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_exists.v3 |
| AST binding | ast.expr.xml_exists |
| Engine entrypoint | xmlexists |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlexists;SBSFC037-xmlcomment-invalid`.

### `xmlforest`

**Purpose:** Builds XML elements from named scalar values.

**Call Forms:**

- `xmlforest(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlforest(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.forest |
| UUID | 019dffbb-f000-770d-ad5a-9652416b8eb8 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_forest.v3 |
| AST binding | ast.expr.xml_forest |
| Engine entrypoint | xmlforest |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlforest;SBSFC037-xmlcomment-invalid`.

### `xmlnamespaces`

**Purpose:** Performs the `xmlnamespaces` XML helper on bounded XML input.

**Call Forms:**

- `XMLNAMESPACES(decl_list)`
- Syntax category: `function_call`

**Parameters:**

- `decl_list`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlnamespaces(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.namespaces |
| UUID | 019dffbb-f003-7002-8a00-000000000002 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_namespaces.v3 |
| AST binding | ast.expr.xml_namespaces |
| Engine entrypoint | xmlnamespaces |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xmlnamespaces-decl;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xmlparse`

**Purpose:** Parses bounded text into an XML value.

**Call Forms:**

- `xmlparse(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlparse('<person>Ada</person>') as xml_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.parse |
| UUID | 019dffbb-f003-7003-8a00-000000000003 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_parse.v3 |
| AST binding | ast.expr.xml_parse |
| Engine entrypoint | xmlparse |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xmlparse-bare;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xmlpi`

**Purpose:** Performs the `xmlpi` XML helper on bounded XML input.

**Call Forms:**

- `XMLPI(NAMEtarget[,content])`
- Syntax category: `function_call`

**Parameters:**

- `NAMEtarget[`: Bound using the declared descriptor rules for this overload.
- `content]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlpi(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.pi |
| UUID | 019dffbb-f000-7c5d-9d3d-92c0b78fb021 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_pi.v3 |
| AST binding | ast.expr.xml_pi |
| Engine entrypoint | xmlpi |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlpi-name;SBSFC037-xmlcomment-invalid`.

### `xmlquery`

**Purpose:** Evaluates a bounded XML query expression.

**Call Forms:**

- `XMLQUERY(xq[namespaces][PASSING...][RETURNING...][ONEMPTY])`
- Syntax category: `function_call`

**Parameters:**

- `xq[namespaces][PASSING...][RETURNING...][ONEMPTY]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlquery(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.query |
| UUID | 019dffbb-f003-7004-8a00-000000000004 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_query.v3 |
| AST binding | ast.expr.xml_query |
| Engine entrypoint | xmlquery |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xmlquery-path;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xmlroot`

**Purpose:** Returns XML with root-level declaration controls applied.

**Call Forms:**

- `xmlroot(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlroot(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.root |
| UUID | 019dffbb-f000-771b-9c03-aea00ea3719e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_root.v3 |
| AST binding | ast.expr.xml_root |
| Engine entrypoint | xmlroot |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmlroot;SBSFC037-xmlcomment-invalid`.

### `xmlserialize`

**Purpose:** Serializes XML to a text or binary descriptor.

**Call Forms:**

- `xmlserialize(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlserialize(xmlelement('person', 'Ada')) as xml_text;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.serialize |
| UUID | 019dffbb-f003-7005-8a00-000000000005 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_serialize.v3 |
| AST binding | ast.expr.xml_serialize |
| Engine entrypoint | xmlserialize |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xmlserialize-bare;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xmltable`

**Purpose:** Maps XML content into a tabular row shape.

**Call Forms:**

- `XMLTABLE([namespaces,]xq[PASSING...]COLUMNS(...))`
- Syntax category: `function_call`

**Parameters:**

- `[namespaces`: Bound using the declared descriptor rules for this overload.
- `]xq[PASSING...]COLUMNS(...)`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded in-core XML scalar helper arguments: XML/text fragments, name-like aliases, path tokens, declaration controls, and scalar content values only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes non-XML descriptors and rejects unsafe XML names, comments, PI content, declaration values, arity, or byte limits.
- NULL handling: SQL null input propagates to SQL null for document/boolean helpers where applicable.

**Returns:**

XML/multimodel scalar helper returns bounded xml_document, xml fragment, boolean, or json_document descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML/JSON descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML/multimodel scalar helper; deterministic in-core XML text construction/inspection only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content, malformed XML control value, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmltable(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.table |
| UUID | 019dffbb-f000-7f47-8a74-4acf46ce946c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_table.v3 |
| AST binding | ast.expr.xml_table |
| Engine entrypoint | xmltable |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC037-xmltable-columns;SBSFC037-xmlcomment-invalid`.

### `xmltext`

**Purpose:** Performs the `xmltext` XML helper on bounded XML input.

**Call Forms:**

- `xmltext(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmltext(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.text |
| UUID | 019dffbb-f003-7006-8a00-000000000006 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_text.v3 |
| AST binding | ast.expr.xml_text |
| Engine entrypoint | xmltext |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xmltext-bare;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

### `xmlvalidate`

**Purpose:** Validates bounded XML input according to the selected XML helper route.

**Call Forms:**

- `xmlvalidate(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: bounded in-core XML document/query helper arguments: XML/text fragments, document/content/sequence mode tokens, namespace or attribute name/value pairs, path tokens, and target type metadata only.
- Coercion: accepts deterministic bounded textual XML fragments and scalar values; escapes text/attribute/namespace output and rejects unsafe names, malformed XML document text, arity, entity, DTD, or byte-limit violations.
- NULL handling: SQL null input propagates to SQL null for document/query/serialize helpers where applicable.

**Returns:**

XML document/query scalar helper returns bounded xml_document, xml fragment, or character descriptor according to function id.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: UTF-8 text semantics for XML descriptor output; no collation-sensitive comparison.
- Timezone: not applicable.
- Security and authority: pure bounded XML document/query scalar helper; deterministic in-core XML normalization, escaping, namespace/attribute descriptor, serialize, validate, and narrow tag-path query behavior only; no parser SQL execution, external plugin XML engine, storage/catalog lookup, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity, unsafe XML name/content/entity, malformed XML document text, DTD/entity declaration, or bounded-size violation refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xmlvalidate(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.xml.validate |
| UUID | 019dffbb-f003-7007-8a00-000000000007 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.xml_validate.v3 |
| AST binding | ast.expr.xml_validate |
| Engine entrypoint | xmlvalidate |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC039-xmlvalidate-bare;SBSFC039-xmldocument-invalid;SBSFC039-xml-attrs-invalid`.

