# Book Assembly

This directory turns the source manuals under `docs/documentation/draft/` into
per-volume "books" for PDF publication. It replaces the earlier single
`combined.md` approach, which no longer scales to the full documentation set.

## What it produces

Running the build writes, under `book_assembly/output/`, for each volume:

- `<id>.md` — one assembled markdown file: generated front matter (title,
  audience, preface, parts map) → the volume's parts in reading order → the
  shared glossary → the "About This Documentation" notice.
- `<id>.html` — a print-ready standalone HTML (Pandoc), with a table of contents
  and the print stylesheet `style.css`, intended for PDF rendering.

## Chapter markers (for editing in OpenOffice etc.)

Inside each assembled file, every concatenated source file is preceded by a
visible marker line:

```text
===== FILE SEPARATION =====
```

Search for **`FILE SEPARATION`** to jump between chapters, or use Find & Replace
to turn each marker into a page break when formatting.

## Split volumes

The SBsql Function Reference (`v2c-...`) is **split into one file per namespace
group** rather than one large file. The build emits:

- `v2c-sbsql-function-reference-00-index` — the volume front matter, the list of
  namespace files, the glossary, and the about notice.
- `v2c-sbsql-function-reference-01-overview` — the function-reference overview.
- `v2c-sbsql-function-reference-NN-sb_<namespace>` — one file per namespace
  package (`sb_core`, `sb_crypto`, `sb_json`, ...).

Mark any volume split this way by adding `split_by_file=True` to its entry in
`VOLUMES`.

This repository has Pandoc but no PDF engine, so the build stops at print-ready
HTML. Produce the final PDFs in your own environment, for example:

```text
# headless Chromium (no extra install):
chromium --headless --print-to-pdf=v1.pdf book_assembly/output/v1-...html
# or WeasyPrint:
weasyprint book_assembly/output/v1-...html v1.pdf
# or Pandoc with a TeX engine:
pandoc book_assembly/output/<id>.md -o <id>.pdf --toc --pdf-engine=xelatex
```

## Volumes

Volume definitions live in `build.py` (`VOLUMES`). The current set:

| id | Title |
| --- | --- |
| v1-concepts-and-getting-started | Concepts and Getting Started |
| v2a-sbsql-foundations-types-catalog | SBsql — Foundations, Data Types, and Catalog |
| v2b-sbsql-syntax-reference | SBsql — Syntax |
| v2c-sbsql-function-reference | SBsql — Functions |
| v3-operations-security-autonomy | Operations, Security, and Autonomy |
| v4-application-development-and-integration | Application Development and Integration |
| v5-compatibility-and-reference-parsers | Compatibility and Reference Parsers |

## How to build

```text
python3 book_assembly/build.py
```

## Cross-references and links

An assembled document never links to an external file. During assembly every
markdown link is rewritten:

- **Target in the same document** → an internal anchor link (`[text](#ch-...)`).
  Each chapter is preceded by `<a id="ch-...">`, so these resolve for in-PDF
  navigation. The shared glossary anchor is `#ch-glossary`.
- **Target in another book/document** → plain text of the form
  `link text (Document Title, page XXX)`. Replace `XXX` with the real page once
  page numbers are finalized. (For the split Function Reference, the "Document
  Title" is the specific namespace file, since each is its own PDF.)
- **Unresolvable relative target** → the link is dropped, keeping just its text.
- **Web links** (`http(s)://`, `mailto:`) are left unchanged.

Images are rewritten to absolute paths and embedded into the HTML, so they are
not external references either.

To find every cross-book reference still needing a page number, search for
`page XXX`.

## Ordering

Chapter order within each manual follows `file_listing_2.txt`; any files not
listed there are appended in sorted order. Adjust a volume's parts or order by
editing `VOLUMES` in `build.py`.

## Notes

- The shared glossary is `../GLOSSARY.md` (canonical); the per-chapter Getting
  Started glossary is excluded from assembly so it is not duplicated.
- Cross-file relative links inside a volume are not rewritten; they are harmless
  in the print output. In-volume navigation comes from the generated table of
  contents.
