# Oracle Built-in, Package, and PL/SQL Mapping Status

Search key: SB_REFERENCE_DONOR_ORACLE_BUILTIN_MAPPING_STATUS

Scope correction: Oracle is not emulated by ScratchBird. This file is retained only as capability-reference evidence. It must not drive Oracle parser emulation, Oracle catalog emulation, or inbound Oracle Net/TTC behavior.

The Oracle behavior inventory is a first-pass compatibility inventory. It is not a complete Oracle function/package/PLSQL implementation table.

## Required exact mapping classes

| Class | Authority |
|---|---|
| SQL scalar and aggregate functions | Native v3 AST/SBLR when exact; otherwise trusted C++ UDR. |
| Oracle format models | Native v3 format model support or C++ helper routines where ScratchBird chooses to match the capability; no Oracle parser emulation. |
| Hierarchical query constructs | Native AST/SBLR executor profile or explicit unsupported diagnostic. |
| PL/SQL routines and packages | SBLR routine frames, internal procedures, or trusted C++ UDR bridges. |
| Ref cursors/result sets | ScratchBird result-set descriptors with Oracle lifetime and transaction visibility. |
| Autonomous transactions | ScratchBird external transaction mechanism with exact diagnostic and audit behavior. |
| Supplied packages | Native, report-only, migration-only, C++ UDR, native-v3-only, or forbidden classification. |
| Object methods and collections | Compound/locator/opaque domain methods through SBLR/native/C++ UDR only. |
| Java/external procedures | Forbidden as trusted ScratchBird runtime. |

## Required mapping row fields

Each final mapping row must define:

- Oracle version profile
- SQL versus PL/SQL visibility
- package/schema/object qualification
- exact signature and overload resolution
- argument defaults and named-argument behavior
- return descriptor or cursor shape
- exception behavior
- transaction effect
- security effect
- parser AST production
- SBLR mapping or C++ UDR entry point
- donor-rendered diagnostic vector
- conformance tests

## Diagnostics

| Condition | Diagnostic vector |
|---|---|
| Exact signature missing | DONOR.ORACLE.BUILTIN.SIGNATURE_MISSING |
| Package surface missing | DONOR.ORACLE.PACKAGE.SURFACE_MISSING |
| PL/SQL construct unmapped | DONOR.ORACLE.PLSQL.MAPPING_MISSING |
| Ref cursor lifetime unspecified | DONOR.ORACLE.REF_CURSOR.LIFETIME_UNSPECIFIED |
| Oracle format model unspecified | DONOR.ORACLE.FORMAT_MODEL.MISSING |
| Non-C++ runtime requested | DONOR.ORACLE.NON_CPP_RUNTIME_FORBIDDEN |
| Source-backed pending behavior invoked as native | DONOR.ORACLE.SOURCE_BACKED_PENDING |
