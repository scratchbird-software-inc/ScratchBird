#!/usr/bin/env python3
"""
Assemble the ScratchBird documentation into per-volume "books".

For each volume it produces, under book_assembly/output/:
  <id>.md   - a single assembled markdown (front matter -> parts/chapters -> glossary -> about)
  <id>.html - a print-ready standalone HTML (via pandoc, if available) for PDF rendering elsewhere

Chapter order within a manual follows file_listing_2.txt; files not listed are appended sorted.
This script does NOT render PDF (no engine assumed); it produces print-ready inputs.
"""
import os, subprocess, sys, re

IMG_RE = re.compile(r'(!\[[^\]]*\]\()([^)]+?)(\))')

def rewrite_images(text, srcdir):
    """Make relative image paths absolute (relative to the source file's directory)
    so they still resolve after the file is concatenated into the output directory."""
    def repl(m):
        pre, path, post = m.group(1), m.group(2).strip(), m.group(3)
        if path.startswith(('http://','https://','data:','/')): return m.group(0)
        ab=os.path.normpath(os.path.join(srcdir, path))
        return f"{pre}{ab}{post}"
    return IMG_RE.sub(repl, text)

DRAFT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # .../draft
OUT   = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'output')
CSS   = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'style.css')
GLOSSARY = os.path.join(DRAFT, 'GLOSSARY.md')

# Files never pulled in as a chapter (superseded by shared back matter, or build artifacts)
EXCLUDE_BASENAMES = {'combined.md', 'combined_old.md'}
EXCLUDE_RELPATHS  = {'Getting_Started/reference/glossary.md'}  # replaced by canonical GLOSSARY back matter

# Visible, searchable chapter-boundary marker placed before each concatenated source file.
# Search for "FILE SEPARATION" in an editor to jump between chapters / apply a page break.
def chapter_separator(rel):
    return f"\n\n===== FILE SEPARATION =====\n\n<!-- chapter source: {rel} -->\n\n"

def first_heading(text, fallback):
    for line in text.splitlines():
        s=line.strip()
        if s.startswith('# '): return s[2:].strip()
    return fallback

def slug(rel):
    return 'ch-'+re.sub(r'[^a-z0-9]+','-', rel.lower()).strip('-')

def anchor(rel):
    return f'<a id="{slug(rel)}"></a>\n\n'

# Markdown link (not image): [text](target)
LINK_RE = re.compile(r'(?<!\!)\[([^\]]+)\]\(([^)]+)\)')

# Populated by main() before assembly: draft-relative path -> (doc_basename, doc_title)
FILE2DOC = {}

def rewrite_links(text, srcdir, current_doc, gloss_here=True):
    """Rewrite cross-file links so the output never points at an external file:
       - target inside the SAME output document -> internal anchor (#ch-...)
       - target inside ANOTHER book/document   -> "text (Doc Title, page XXX)"
       - unresolved relative target            -> plain text (link removed)
       Web (http/https/mailto) and existing #anchors are left untouched."""
    def repl(m):
        label, target = m.group(1), m.group(2).strip()
        if target.startswith(('http://','https://','mailto:','#')):
            return m.group(0)
        path = target.split('#',1)[0].split()[0] if target.split('#',1)[0].strip() else target
        path = target.split('#',1)[0].strip()
        if not path:  # pure fragment already handled above
            return m.group(0)
        # glossary (canonical or the excluded per-chapter copy) -> shared glossary anchor
        bn = os.path.basename(path).lower()
        if bn in ('glossary.md',) or path.endswith('GLOSSARY.md'):
            return f'[{label}](#ch-glossary)' if gloss_here else f'{label} (see the Glossary)'
        if not path.endswith('.md'):
            # any other relative, non-web reference: drop the link, keep the text
            return label
        tgt = os.path.normpath(os.path.join(srcdir, path))
        rel = os.path.relpath(tgt, DRAFT)
        info = FILE2DOC.get(rel)
        if info is None:
            return label  # not in any book -> plain text (no external link)
        doc, title = info
        if doc == current_doc:
            return f'[{label}](#{slug(rel)})'
        return f'{label} ({title}, page XXX)'
    return LINK_RE.sub(repl, text)

ABOUT = """# About This Documentation

This book is part of the ScratchBird documentation set. ScratchBird is a
Convergent Data Engine (CDE).

**Draft status.** This is draft documentation. It describes the architecture and
intended behavior of the source tree. A topic appearing here does not by itself
guarantee that a feature is complete, enabled, performant, certified, or
available in any particular build. Always confirm against the current build,
configuration, tests, and release notes.

**License.** The ScratchBird engine is distributed under the Mozilla Public
License 2.0 (MPL-2.0). This documentation describes that open-source engine.

**No certification claim.** Nothing in this documentation constitutes a security
certification, performance benchmark, or compatibility guarantee.
"""

# Volume definitions, in library order.
# parts: list of (Part title, source path relative to DRAFT — a directory or a single .md file)
VOLUMES = [
 dict(id='v1-concepts-and-getting-started',
   title='ScratchBird — Concepts and Getting Started',
   audience='New users, evaluators, and anyone forming a first understanding of ScratchBird.',
   preface=("This volume introduces ScratchBird as a Convergent Data Engine (CDE): what that "
            "product class is, the architecture and ideas behind it, and the first practical "
            "steps for creating a database and running a session. Read it before the reference "
            "and operations volumes."),
   parts=[('Getting Started','Getting_Started'),
          ('Concepts of a Convergent Data Engine','CDE_Concepts'),
          ('Appendix: Functionality Support Matrix','functionality_matrix.md')]),
 dict(id='v2a-sbsql-foundations-types-catalog',
   title='SBsql Language Reference — Foundations, Data Types, and Catalog',
   audience='Developers writing SBsql; readers needing the type system and catalog model.',
   preface=("This volume covers the foundations of the SBsql language: the core paradigms that "
            "govern how SBsql relates to the engine, the data type system, the system catalog "
            "surfaces, and localized language support. It is the first of three SBsql reference "
            "volumes."),
   parts=[('Language Reference Overview','Language_Reference/README.md'),
          ('Core Paradigms','Language_Reference/core_paradigms'),
          ('Data Types','Language_Reference/data_types'),
          ('Catalog Reference','Language_Reference/catalog_reference'),
          ('Language Support','Language_Support')]),
 dict(id='v2b-sbsql-syntax-reference',
   title='SBsql Language Reference — Syntax',
   audience='Developers writing SBsql statements and scripts.',
   preface=("This volume is the SBsql statement and syntax reference: statement lifecycles, DML "
            "and queries, procedural SQL, operational statements, and the per-production EBNF "
            "grammar. It is the second of three SBsql reference volumes."),
   parts=[('Syntax Reference','Language_Reference/syntax_reference')]),
 dict(id='v2c-sbsql-function-reference',
   title='SBsql Language Reference — Functions',
   audience='Developers using built-in functions, operators, aggregates, and windows.',
   short='SBsql Functions',
   preface=("This volume is the SBsql functional reference: the built-in function packages, "
            "operators, aggregates, window functions, and special forms, grouped by namespace. "
            "It is split into one file per namespace group; this index lists them. "
            "It is the third of three SBsql reference volumes."),
   split_by_file=True,
   parts=[('Functional Reference','Language_Reference/functional_reference')]),
 dict(id='v3-operations-security-autonomy',
   title='ScratchBird — Operations, Security, and Autonomy',
   audience='Operators, administrators, and security reviewers.',
   preface=("This volume is for running ScratchBird in practice: installation and configuration, "
            "service and database lifecycle, backup and recovery, the security model, the "
            "autonomous agent runtime, and execution acceleration."),
   parts=[('Operations and Administration','Operations_Administration'),
          ('Security Guide','Security_Guide'),
          ('Agent Runtime Guide','Agent_Runtime_Guide'),
          ('Acceleration Guide','Acceleration_Guide')]),
 dict(id='v4-application-development-and-integration',
   title='ScratchBird — Application Development and Integration',
   audience='Application developers, driver users, and integration engineers.',
   preface=("This volume is for building on ScratchBird: the embedding API and frozen ABI, the "
            "client drivers and integration adaptors, and the AI / MCP integration layer."),
   parts=[('Embedding and API Reference','Embedding_API_Reference'),
          ('Client and Driver Guide','Client_Driver_Guide'),
          ('AI Integration Guide','AI_Integration_Guide')]),
 dict(id='v5-compatibility-and-reference-parsers',
   title='ScratchBird — Compatibility and Reference Parsers',
   audience='Teams migrating from, or interoperating with, other databases.',
   preface=("This volume explains, in general terms, how ScratchBird's reference parsers provide "
            "compatibility with other database dialects and wire protocols, how behavior is "
            "emulated, and where the boundaries are."),
   parts=[('Reference Parsers','Reference_Parsers')]),
]

def listing_order():
    order={}
    p=os.path.join(DRAFT,'file_listing_2.txt')
    if os.path.exists(p):
        for i,line in enumerate(open(p)):
            s=line.strip()
            if s.startswith(DRAFT):
                order[os.path.relpath(s,DRAFT)]=i
    return order

ORDER=listing_order()

def gather(path):
    """Return ordered list of absolute md files for a part source (dir or single file)."""
    full=os.path.join(DRAFT,path)
    files=[]
    if os.path.isfile(full) and full.endswith('.md'):
        files=[os.path.relpath(full,DRAFT)]
    elif os.path.isdir(full):
        for dp,_,fs in os.walk(full):
            for fn in fs:
                if not fn.endswith('.md'): continue
                rel=os.path.relpath(os.path.join(dp,fn),DRAFT)
                if fn in EXCLUDE_BASENAMES or rel in EXCLUDE_RELPATHS: continue
                files.append(rel)
    files=[f for f in files if f not in EXCLUDE_RELPATHS]
    files.sort(key=lambda r:(ORDER.get(r, 10**9), r))
    return files

def front_matter(v):
    parts_list='\n'.join(f"- **{t}**" for t,_ in v['parts'])
    return f"""---
title: "{v['title']}"
---

# {v['title']}

*ScratchBird documentation — draft*

## Who this book is for

{v['audience']}

## About this book

{v['preface']}

## Parts in this volume

{parts_list}

> This is a **draft**. See *About This Documentation* at the end of this book for
> status and license. Confirm any specific behavior against the current build.

\\newpage
"""

def read_raw(rel):
    return open(os.path.join(DRAFT,rel),encoding='utf-8',errors='replace').read()

def read_rewritten(rel):
    return rewrite_images(read_raw(rel), os.path.dirname(os.path.join(DRAFT,rel)))

def glossary_text():
    return open(GLOSSARY,encoding='utf-8',errors='replace').read() if os.path.exists(GLOSSARY) else ''

GLOSSARY_ANCHOR='<a id="ch-glossary"></a>\n\n'

def split_outputs(v):
    """Shared ordering+naming for a split volume: list of (rel, output_basename, chapter_title)."""
    files=[]
    for _,psrc in v['parts']:
        files += gather(psrc)
    files.sort(key=lambda r:(0 if os.path.basename(r).lower() in ('index.md','readme.md') else 1,
                             ORDER.get(r,10**9), r))
    res=[]
    for i,rel in enumerate(files, start=1):
        base=os.path.splitext(os.path.basename(rel))[0]
        if base.lower() in ('index','readme'): base='overview'
        title=first_heading(read_raw(rel), base)
        res.append((rel, f"{v['id']}-{i:02d}-{base}", title))
    return res

def build_file2doc():
    """Map each draft-relative source file to the output document (basename, title) that contains it."""
    m={}
    for v in VOLUMES:
        title=v['title']; short=v.get('short', title)
        if v.get('split_by_file'):
            for rel,outname,ctitle in split_outputs(v):
                m[rel]=(outname, f"{short} — {ctitle}")
        else:
            for _,psrc in v['parts']:
                for rel in gather(psrc):
                    m[rel]=(v['id'], title)
    return m

def chapter_block(rel, current_doc, gloss_here=True, with_separator=True):
    srcdir=os.path.dirname(os.path.join(DRAFT,rel))
    body=rewrite_links(read_rewritten(rel), srcdir, current_doc, gloss_here)
    head=chapter_separator(rel) if with_separator else ''
    return f"{head}{anchor(rel)}{body}\n"

def assemble(v):
    """Single-file assembly: front matter -> chapters (FILE SEPARATION + anchors) -> glossary -> about."""
    chunks=[front_matter(v)]
    for ptitle,psrc in v['parts']:
        chunks.append(f"\n\n# {ptitle}\n\n")
        for rel in gather(psrc):
            chunks.append(chapter_block(rel, v['id']))
    chunks.append("\n\n"+GLOSSARY_ANCHOR+glossary_text())
    chunks.append("\n\n"+ABOUT)
    return "\n".join(chunks)

def assemble_split(v):
    """Split assembly: returns list of (output_basename, markdown):
    one index file plus one file per source chapter (each its own document)."""
    outs=split_outputs(v)
    chapter_files=[(outname, chapter_block(rel, outname, gloss_here=False)) for rel,outname,_ in outs]
    idx=[front_matter(v)]
    idx.append("\n\n# Function Reference — Namespace Files\n\n")
    idx.append("This volume is split into one file per namespace group. The files, in order:\n\n")
    for rel,outname,title in outs:
        idx.append(f"- **{title}** — `{outname}.md`\n")
    idx.append("\n\n"+GLOSSARY_ANCHOR+glossary_text())
    idx.append("\n\n"+ABOUT)
    return [(f"{v['id']}-00-index", "".join(idx))] + chapter_files

def main():
    os.makedirs(OUT,exist_ok=True)
    FILE2DOC.update(build_file2doc())   # must precede assembly so link rewriting can resolve targets
    have_pandoc=subprocess.run(['bash','-lc','command -v pandoc'],capture_output=True).returncode==0
    def render(basename, md, title):
        mdpath=os.path.join(OUT,basename+'.md')
        open(mdpath,'w',encoding='utf-8').write(md)
        html='-'
        if have_pandoc:
            cmd=['pandoc',mdpath,'-o',os.path.join(OUT,basename+'.html'),
                 '--standalone','--embed-resources','--toc','--toc-depth=3',
                 '--metadata',f"title={title}"]
            if os.path.exists(CSS): cmd+= ['--css',os.path.basename(CSS)]
            r=subprocess.run(cmd,capture_output=True,text=True)
            html='ok' if r.returncode==0 else f'pandoc-fail: {r.stderr.strip()[:80]}'
        return md.count('\n'), html

    summary=[]
    for v in VOLUMES:
        if v.get('split_by_file'):
            for basename, md in assemble_split(v):
                lines,html=render(basename, md, v['title'])
                summary.append((basename,lines,html))
        else:
            lines,html=render(v['id'], assemble(v), v['title'])
            summary.append((v['id'],lines,html))
    # copy css next to html for relative reference
    if os.path.exists(CSS):
        import shutil; shutil.copy(CSS,os.path.join(OUT,os.path.basename(CSS)))
    w=max(len(s[0]) for s in summary)
    print(f"{'volume'.ljust(w)}  lines   html")
    for i,l,h in summary: print(f"{i.ljust(w)}  {l:6}  {h}")

if __name__=='__main__': main()
