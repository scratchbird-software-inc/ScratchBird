# Engine Internal API Runtime Foundation

This package implements `RUNTIME-019`: the engine-owned API that parsers eventually call.

## Scope

The package owns:

- SBLR envelope metadata;
- engine context identity;
- bound operation descriptors;
- operation authority classes;
- canonical result-shape descriptors;
- dispatch request validation;
- deterministic diagnostics for API boundary violations.

## Authority rules

- The engine accepts SBLR/internal envelopes only.
- SQL text is not accepted at the engine boundary.
- Names must be resolved to UUIDs before engine work begins.
- The parser is untrusted and cannot mark itself trusted.
- Security checks are engine-owned and required for every bound operation.
- Engine diagnostics are canonical; parsers perform client-facing package shaping.

## Identity rules

- Database, session, and principal UUIDs in engine context are engine identity and must be UUIDv7.
- UUIDv1 through UUIDv6 are donor/client compatibility values only and cannot be engine context identity.

## Non-scope

This slice does not execute operations, parse SBLR payload bytes, connect parser processes, perform catalog access, or implement security policy. It reserves the contract used by later engine and parser slices.

## RUNTIME-020 minimal built-in operations

This package also implements `RUNTIME-020`: minimal internal operations for `SHOW VERSION` and `SHOW DATABASE` equivalents.

The built-in operation layer owns:

- operation descriptors for `show_version` and `show_database`;
- canonical engine result shapes for those operations;
- canonical engine result rows for those operations;
- validation that database identity returned by `show_database` is UUIDv7 engine identity.

## Built-in operation rules

- Built-in operations are engine-owned internal operations, not SQL commands.
- Parser surfaces may render them as `SHOW` commands, but the engine operation remains SBLR/internal.
- Result rows are canonical engine structures. Parser package shaping is still required before donor/client output.
- `SHOW DATABASE` returns database UUID identity, not name authority.
- `SHOW VERSION` returns engine component/version metadata only.

## Non-scope

This slice does not execute arbitrary SBLR, check full security policy, expose management views, attach databases, or integrate parser calls. Those are later slices.

## DBOPEN-005 database runtime binding

This package now includes the first runtime binding between an open database lifecycle state and engine-owned built-in operations.

The database runtime layer owns:

- accepting a created/opened database lifecycle state;
- preserving database UUIDv7 identity as the authority returned by `SHOW DATABASE`;
- executing `SHOW VERSION` and `SHOW DATABASE` through canonical engine result shapes;
- keeping parser SQL text outside the engine boundary.

This API still does not execute arbitrary SBLR payloads, perform full security policy, attach sessions, or expose management/cluster surfaces.
