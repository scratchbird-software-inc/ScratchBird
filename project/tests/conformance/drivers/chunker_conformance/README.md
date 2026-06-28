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

Usage idiom:

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

## Chain-level harness rule (terminator reset + indexing across scripts)

The compiler emits two run forms (see `full_surface_scripts/`): one **per-script
compiled file** per script, and one concatenated **chain**
(`full_surface_chain.sbsql`) in which each script's body is wrapped by
`-- begin_script: <name>` / `-- end_script: <name>` line-comment markers. A
runner may execute either form, and BOTH MUST produce identical `statement_id`s.

The rule a chain runner must follow:

1. A `-- begin_script: <name>` marker starts a new script segment: set the current
   script name to `<name>`, **reset the active terminator to `;`**, and **restart
   the emitted-statement index at 1**.
2. Chunk the segment body (the lines up to the matching `-- end_script: <name>`,
   excluding both marker lines) exactly as a standalone script — so a segment is
   byte-for-byte the per-script file body.
3. Content outside any begin/end pair (the chain header) is ignored.

Because each segment is chunked with a fresh terminator, **terminator state never
leaks across scripts**: a script that leaves `SET TERM ^` active cannot change how
the next script splits. A runner that executes the per-script files one at a time
satisfies this automatically (each file is a fresh chunk starting at `;`), so only
chain-mode runners need to implement the reset explicitly.

Reference: `drivers/driver/python/src/scratchbird/sql.py::iter_chain_statements`,
which yields `(script_name, index, statement)`. The fixture `chain_cases.json`
encodes the rule as `input -> [[script, index, statement], ...]`.

## Reference implementation and how each driver is verified

- Single-script splitter reference:
  `drivers/driver/python/src/scratchbird/sql.py::split_top_level_statements`
  (mirrored by the shared C++ header `cpp/tools/sb_statement_chunker.hpp` and the
  per-language driver splitters).
- Chain-rule reference: `…/sql.py::iter_chain_statements`.
- Each driver's splitter must pass `cases.json`. A driver test loads `cases.json`,
  runs its splitter on each `input`, and asserts the result equals `expected`.
- Adding a case here raises the bar for every driver at once.

### Verifiers in this directory

- `verify_python_reference.py` — single-script splitter vs `cases.json` (Python ref).
- `verify_cpp_chunker.cpp` — single-script splitter vs `cases.json` (shared C++ header).
- `verify_pascal_chunker.pas` — single-script splitter vs `cases.json` (Free Pascal).
- `verify_python_chain.py` — chain rule vs `chain_cases.json`, plus a real-suite
  parity check asserting chain-mode and per-script-mode emit identical
  `(script, index, statement)` sequences over the full compiled suite.
