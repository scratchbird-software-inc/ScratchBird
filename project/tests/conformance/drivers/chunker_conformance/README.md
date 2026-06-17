# Statement-Chunker Conformance (cross-driver)

Every ScratchBird driver tool that executes a `.sbsql` script splits it into
statements before sending each to the server. Those splitters **must behave
identically across all drivers**, or the same conformance chain will split
differently per driver. This fixture (`cases.json`) is the shared oracle: each
driver's splitter MUST reproduce `expected` exactly for every `input`.

## The chunker contract (canonical `SET TERM` semantics)

Ported from the standalone CLI `project/drivers/tool/cli/sb_isql.cpp` (`SET TERM`).

1. **Active terminator** starts as `;`.
2. Scan the input; cut a statement at the active terminator when it occurs
   **outside single (`'`) and double (`"`) quotes**. The terminator may be
   multiple characters (e.g. `^`, `$$`).
3. For each cut chunk, ignore leading full-line `--` comments and blank lines;
   if the remaining content matches `SET TERM <terminator>` (case-insensitive),
   it is a **client directive**: set the active terminator to `<terminator>` and
   **consume it** — it is NOT emitted as a statement and NOT counted in statement
   indexing.
4. Otherwise emit the chunk (trimmed) as a statement. A chunk that is non-empty
   after trimming (including a comment-only chunk) is emitted, exactly as the
   pre-`SET TERM` splitter did.
5. With no `SET TERM` directive present, behavior is identical to a plain
   quote-aware top-level `;` split (backward compatible — existing scripts and
   `statement_id` indices do not shift).

Usage idiom (Firebird / `sb_isql`):

```
SET TERM ^ ;
CREATE PROCEDURE p ... AS BEGIN ... ; ... ; END^
SET TERM ; ^
```

**Author caveat:** the chosen terminator must not appear (outside quotes) inside
the body it wraps.

## Statement indexing

`statement_id = <script>:<index>` counts only **emitted** statements (1-based),
in order; `SET TERM` directives and empty chunks do not count. This keeps the
existing refusal indices (e.g. `080_...:6`, `011_...:13`, `099_...:2-5`) stable.

## Reference implementation and how each driver is verified

- Reference: `drivers/driver/python/src/scratchbird/sql.py::split_top_level_statements`.
- Each driver's splitter must pass `cases.json`. A driver test loads `cases.json`,
  runs its splitter on each `input`, and asserts the result equals `expected`.
- Adding a case here raises the bar for every driver at once.
