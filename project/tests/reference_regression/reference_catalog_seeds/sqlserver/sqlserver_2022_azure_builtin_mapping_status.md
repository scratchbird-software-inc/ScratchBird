# SQL Server 2022/Azure Built-in Mapping Status

Search key: SB_REFERENCE_REFERENCE_SQLSERVER_2022_AZURE_BUILTIN_MAPPING_STATUS

Scope correction: SQL Server is not emulated by ScratchBird. This file is retained only as capability-reference evidence. It must not drive SQL Server parser emulation, SQL Server catalog emulation, or inbound TDS behavior.

This file records the implementation status for SQL Server built-in behavior copied into the private reference tree. It is not a completed function implementation table. It exists so implementation work can distinguish authoritative reference behavior groups from generated inventory defects.

## Mapping authority

The SQL Server parser must lower supported Transact-SQL built-ins to native v3 AST nodes and SBLR opcodes when the behavior is common to ScratchBird. When SQL Server behavior differs from ScratchBird native behavior, the SQL Server C++ UDR bridge must provide the compatibility surface and must call only documented SBLR or trusted C++ UDR entry points.

The engine must not execute Transact-SQL text. The engine receives SBLR, UUIDs, data packets, and diagnostic-vector requests only.

## Behavior groups

| Group | Examples | Required ScratchBird mapping | Current status |
|---|---|---|---|
| Logical scalar | CHOOSE, IIF, GREATEST, LEAST, COALESCE, NULLIF | Native expression AST and SBLR branch/comparison/null operators | Inventory copied; exact edge semantics still required |
| String scalar | ASCII, CHARINDEX, CONCAT, FORMAT, LEN, PATINDEX, STRING_SPLIT, TRANSLATE | Native text, collation, locale, and table-returning function support; SQL Server length and null behavior must be preserved | Inventory copied; exact collation and return-shape mapping still required |
| Date/time scalar | SYSDATETIME, GETDATE, DATE_BUCKET, DATETRUNC, DATEDIFF_BIG, AT TIME ZONE | Native temporal domain plus session timezone/language settings; cluster time rules apply where UUID v7 or cluster ordering is involved | Inventory copied; exact precision and language behavior still required |
| Math scalar | ABS, ROUND, POWER, LOG, RAND | Native numeric SBLR operators, including decimal and float128 where applicable | Inventory copied; exact overflow and rounding diagnostics still required |
| Aggregate | APPROX_COUNT_DISTINCT, AVG, COUNT_BIG, STRING_AGG, VARP | Native aggregate AST/SBLR; approximate aggregate requires explicit profile and metrics | Inventory copied; exact null and overflow behavior still required |
| Analytic/window | CUME_DIST, LAG, LEAD, PERCENTILE_CONT, ROW_NUMBER | Native window AST/SBLR and executor frame semantics | Inventory copied; exact frame defaults and ordering behavior still required |
| JSON | ISJSON, JSON_VALUE, JSON_QUERY, JSON_MODIFY, OPENJSON, JSON_ARRAY, JSON_OBJECT | Native structured-value domain support plus table-returning SBLR stream for OPENJSON | Inventory copied; exact lax/strict path behavior still required |
| System/session | @@ERROR, @@ROWCOUNT, @@TRANCOUNT, XACT_STATE, SESSION_CONTEXT | Reference session-state object backed by ScratchBird transaction/session diagnostics | Inventory copied; exact state-transition rules still required |
| Metadata | OBJECT_ID, DB_NAME, SERVERPROPERTY, TYPEPROPERTY, STATS_DATE | Reference catalog projection over UUID-backed ScratchBird catalog and metrics | Inventory copied; exact visibility rules still required |
| Security | HAS_PERMS_BY_NAME, IS_MEMBER, SUSER_SNAME, ORIGINAL_LOGIN | Reference security projection over ScratchBird security tree and external-auth cache | Inventory copied; exact principal visibility and denial behavior still required |
| Procedure compatibility | sp_helplanguage | C++ UDR compatibility procedure returning reference-shaped rowsets | Inventory copied; exact default rowset still required |

## Generated inventory defects

The generated built-in inventory contains tokens that are not standalone built-in functions:

- `SET`
- `AT`
- `SQL`
- `Azure`
- `NEXT`
- `VERSION`

These tokens must not be implemented from the generated CSV as functions. The final exact mapping table must replace them with the correct parsed constructs:

- `SET DATEFIRST`
- `SET DATEFORMAT`
- `SET LANGUAGE`
- `AT TIME ZONE`
- `NEXT VALUE FOR`
- `@@VERSION`
- SQL Server/Azure availability notes as metadata, not functions

## Required exact mapping table before implementation-ready status

Each built-in behavior row must be expanded into a normative table with these fields:

| Field | Required content |
|---|---|
| reference_name | Exact reference-visible token or grammar production. |
| reference_kind | scalar_function, aggregate_function, window_function, table_function, procedure, session_variable, grammar_construct, or metadata_construct. |
| argument_signature | Argument count, optional arguments, named arguments, accepted datatypes, default values, collation participation, and null behavior. |
| return_signature | Return datatype, nullability, precision/scale/length, rowset shape if table-returning, and reference-specific type aliases. |
| deterministic_class | deterministic, stable, volatile, transaction_state, session_state, metadata_state, or external_state. |
| native_v3_ast | Required AST node or parser production. |
| sblr_mapping | Required SBLR opcode sequence or named C++ UDR entry point. |
| executor_requirements | Required executor state, memory behavior, transaction interaction, and error behavior. |
| scratchbird_authority | Native engine, SQL Server parser, SQL Server C++ UDR, catalog projection, metrics projection, or security projection. |
| unsupported_behavior | Exact forbidden/deferred behavior and diagnostic vector. |
| conformance_tests | Positive, negative, null, collation, overflow, security, transaction, and reference-visibility tests. |

## Diagnostics

| Condition | Diagnostic vector |
|---|---|
| Generated token treated as a function | REFERENCE.SQLSERVER.BUILTIN.GENERATED_TOKEN_FORBIDDEN |
| Missing exact signature | REFERENCE.SQLSERVER.BUILTIN.SIGNATURE_MISSING |
| Missing return shape | REFERENCE.SQLSERVER.BUILTIN.RETURN_SHAPE_MISSING |
| Missing session-state binding | REFERENCE.SQLSERVER.BUILTIN.SESSION_STATE_UNBOUND |
| Missing metadata visibility rule | REFERENCE.SQLSERVER.BUILTIN.VISIBILITY_RULE_MISSING |
| Unsupported SQL Server behavior called | REFERENCE.SQLSERVER.BUILTIN.UNSUPPORTED_BEHAVIOR |
| Raw secret or credential exposure attempted | REFERENCE.SQLSERVER.BUILTIN.RAW_SECRET_FORBIDDEN |
