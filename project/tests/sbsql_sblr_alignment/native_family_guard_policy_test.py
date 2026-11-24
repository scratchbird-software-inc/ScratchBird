#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SB_NATIVE_FAMILY_GUARD_POLICY (2026-12-04; owner: the database is ONE file, no other files; legacy ".sb." side files are drift).

SOURCE POLICY. A native context (EngineRequestContext::native_served_facts) must never reach a legacy side-file family. Every family
is guarded at its ENTRY with `if (NativeServedContext(ctx)) return NativeFamilyRefusalAs<R>("<stem>", ctx);` (or the older
`NativeLegacyRegistryRefusal(` form) as the FIRST statement of the function, before any lock, validation, path or file touch.

THE PARSE (deterministic, no compiler): for every project/src file under engine/, server_engine_bridge/, server/, storage/ and manager/
  1. comments are blanked (// and /* */), raw strings blanked; ordinary string literals are KEPT (the code's own literals are the evidence);
  2. a FAMILY FILE is one whose code holds a string literal naming ".sb." or one of the extra side-file suffixes below;
  3. functions are found by brace matching at namespace level (namespace and anonymous-namespace braces are transparent; a '{' whose
     header ends in ')' [with const/noexcept/->T/ctor-init list] opens a function); each function records its name, whether it sits in
     an anonymous namespace (PRIVATE), its body and the identifiers it calls;
  4. a BUILDER is a function whose body holds the side-file literal; a function REACHES a builder when it is one or calls (by name,
     within the same file, transitively) one. A PUBLIC REACHER is a reacher outside an anonymous namespace.
RULES (each failure names the file and function):
  R1  a family file is either in FILE_ALLOW (one-line reason) or has at least one guard token and every PUBLIC REACHER that is not in
      FN_ALLOW (one-line reason) starts with the guard as its first statement;
  R2  the guard's family token is a side-file stem that the file's own literals name (or is in STEM_OVERRIDE for a dynamic literal);
  R3  nothing in FILE_ALLOW / FN_ALLOW is stale (the file or function must still exist and still be a family file / public reacher);
  R4  FILE_ALLOW holds no file that has a guard (an allowed file must stay allowed for its reason, not by accident).
A NEW family file, or a NEW public function that reaches a builder in a guarded file, FAILS this test until it is guarded or allowed with a
reason. Server control files (Q257) and the storage layer are allowed with that reason; they are listed for the owner, not guarded.
"""
import re, sys, os, json, collections

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
SRC = os.path.join(ROOT, 'src')
SCAN_DIRS = ['engine', 'server_engine_bridge', 'server', 'storage', 'manager']
EXTRA_SUFFIXES = ['.metrics.history']          # side files next to the database whose name does not start with ".sb."
GUARD_TOKENS = ('NativeFamilyRefusalAs<', 'NativeLegacyRegistryRefusal(')
EXTS = ('.cpp', '.hpp', '.inc', '.h')

# ---- the allow lists (one line of reason each) -------------------------------------------------------------------------------------
FILE_ALLOW = {}
FN_ALLOW = {}
STEM_OVERRIDE = {}
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'native_family_guard_policy_allowlist.py')).read())

def strip_comments(src):
    out = []; i = 0; n = len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i); j = n if j < 0 else j; out.append(' ' * (j - i)); i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j])); i = j
        elif src.startswith('R"', i):
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if m:
                end = ')' + m.group(1) + '"'; j = src.find(end, i); j = n if j < 0 else j + len(end)
                out.append(re.sub(r'[^\n]', ' ', src[i:j])); i = j
            else: out.append(c); i += 1
        elif c == '"' or c == "'":
            q = c; j = i + 1
            while j < n and src[j] != q:
                if src[j] == '\\': j += 1
                j += 1
            out.append(src[i:j + 1]); i = j + 1
        else: out.append(c); i += 1
    return ''.join(out)

def mask_strings(code):
    out = []; i = 0; n = len(code)
    while i < n:
        c = code[i]
        if c == '"' or c == "'":
            q = c; j = i + 1
            while j < n and code[j] != q:
                if code[j] == '\\': j += 1
                j += 1
            out.append(q + ' ' * max(0, j - i - 1) + q if j < n else code[i:]); i = j + 1
        else: out.append(c); i += 1
    return ''.join(out)

def functions(code):
    """-> list of dict(name, hdr, body, line, anon, start). Namespace braces are transparent; anon tracks `namespace {`."""
    m = mask_strings(code)
    res = []; stack = []; i = 0; n = len(m)
    while i < n:
        c = m[i]
        if c == '{':
            j = i - 1
            while j >= 0 and m[j].isspace(): j -= 1
            hs = max(m.rfind(';', 0, j + 1), m.rfind('}', 0, j + 1), m.rfind('{', 0, j + 1)) + 1
            hd = m[hs:i].strip()
            kind = 'other'
            if re.search(r'\bnamespace\b[^()]*$', hd) or hd.startswith('extern "C"'):
                kind = 'anon' if re.search(r'\bnamespace\s*$', hd) else 'ns'
            elif re.search(r'\b(class|struct|union|enum)\b[^()]*$', hd) and not re.search(r'\)\s*(const|noexcept|override|->[^{]*)?\s*$', hd):
                kind = 'type'
            elif re.search(r'\)\s*(const|noexcept|override|final|\s)*(->[^{;]*)?\s*(:[^{;]*)?$', hd) or re.search(r'\)\s*:', hd):
                kind = 'fn'
            stack.append((kind, i, hs)); i += 1; continue
        if c == '}':
            if stack:
                kind, s, hs = stack.pop()
                if kind == 'fn' and all(k in ('ns', 'anon') for k, _, _ in stack):
                    hdr = re.sub(r'\s+', ' ', code[hs:s].strip())
                    res.append(dict(name=fname(hdr), hdr=hdr, body=code[s:i + 1], line=code.count('\n', 0, s) + 1,
                                    anon=any(k == 'anon' for k, _, _ in stack) or bool(re.match(r'(static|inline static|static inline)\b', hdr)), start=s))
            i += 1; continue
        i += 1
    return res

def fname(hdr):
    depth = 0; idx = None
    for k, ch in enumerate(hdr):
        if ch == '(' and depth == 0: idx = k; break
    if idx is None: return None
    mm = re.search(r'([A-Za-z_~][A-Za-z0-9_:~]*)\s*$', hdr[:idx])
    return mm.group(1) if mm else None

def first_statement_is_guard(body):
    inner = body[1:].lstrip()
    return bool(re.match(r'if\s*\(\s*NativeServedContext\s*\(', inner)) and any(t in inner[:700] for t in GUARD_TOKENS)

def literal_stems(code):
    stems = set()
    for m in re.finditer(r'"[^"\n]*"', code):
        for s in re.finditer(r'\.sb\.([A-Za-z0-9_]+)', m.group(0)):
            stems.add(re.sub(r'_?v\d+$', '', s.group(1)))
        for suffix in EXTRA_SUFFIXES:
            if suffix in m.group(0): stems.add(suffix.strip('.').replace('.', '_'))
    return stems

def is_family_literal(code):
    for m in re.finditer(r'"[^"\n]*"', code):
        if '.sb.' in m.group(0) or any(sfx in m.group(0) for sfx in EXTRA_SUFFIXES): return True
    return False

GLOBAL_BUILDERS = set()   # names of PATH BUILDERS (a function that returns a string/path and holds the side-file literal) in ANY family file

def literal_in(body):
    return any(('.sb.' in m.group(0) or any(s in m.group(0) for s in EXTRA_SUFFIXES)) for m in re.finditer(r'"[^"\n]*"', body))

def analyse(rel, code):
    fns = functions(code)
    byname = collections.defaultdict(list)
    for f in fns:
        if f['name']: byname[f['name'].split('::')[-1]].append(f)
    for f in fns:
        f['calls'] = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', f['body']))
        f['builder'] = literal_in(f['body']) or bool(f['calls'] & GLOBAL_BUILDERS)
    reach = {}
    def reaches(n, seen):
        if n in reach: return reach[n]
        if n in seen: return False
        seen = seen | {n}; r = False
        for f in byname.get(n, []):
            if f['builder']: r = True
            else:
                for c in f['calls']:
                    if c != n and c in byname and reaches(c, seen): r = True; break
            if r: break
        reach[n] = r; return r
    out = []
    for f in fns:
        if f['name'] and not f['anon'] and reaches(f['name'].split('::')[-1], frozenset()): out.append(f)
    return out

def main():
    failures = []; stats = collections.Counter(); report = []
    files = []
    for d in SCAN_DIRS:
        for dp, dn, fnm in os.walk(os.path.join(SRC, d)):
            for x in fnm:
                if x.endswith(EXTS): files.append(os.path.join(dp, x))
    family_files = {}
    for p in sorted(files):
        rel = os.path.relpath(p, SRC)
        code = strip_comments(open(p, errors='replace').read())
        if is_family_literal(code): family_files[rel] = code
    stats['family_files'] = len(family_files)
    for rel, code in family_files.items():
        for f in functions(code):
            if f['name'] and literal_in(f['body']) and re.search(r'\b(string|path)\b[^()]*\b' + re.escape(f['name'].split('::')[-1]) + r'\s*\(', f['hdr']) and f['name'].split('::')[-1] not in GENERIC_NAMES:
                GLOBAL_BUILDERS.add(f['name'].split('::')[-1])
    stats['global_path_builders'] = len(GLOBAL_BUILDERS)
    seen_fn_allow = set(); seen_file_allow = set()
    for rel, code in sorted(family_files.items()):
        if rel in FILE_ALLOW:
            seen_file_allow.add(rel); stats['allowed_files'] += 1
            if any(t in code for t in GUARD_TOKENS) and rel not in FILE_ALLOW_MAY_HAVE_GUARD:
                failures.append(f'R4 {rel}: allowed ({FILE_ALLOW[rel]}) but it holds a guard token')
            continue
        if not any(t in code for t in GUARD_TOKENS):
            failures.append(f'R1 {rel}: a family file with no guard token and no FILE_ALLOW reason'); continue
        stats['guarded_files'] += 1
        stems = literal_stems(code) | set(STEM_OVERRIDE.get(rel, []))
        for m in re.finditer(r'(?:NativeFamilyRefusalAs<[^>]*(?:<[^>]*>)?[^>]*>|NativeLegacyRegistryRefusal)\s*\(\s*"([^"]+)"', code):
            stats['guard_tokens'] += 1
            if m.group(1) not in stems: failures.append(f'R2 {rel}: guard family "{m.group(1)}" is not a stem the file names ({sorted(stems)})')
        for f in analyse(rel, code):
            key = f'{rel}::{f["name"]}'
            if key in FN_ALLOW:
                seen_fn_allow.add(key); stats['allowed_functions'] += 1; continue
            if first_statement_is_guard(f['body']) or any(first_statement_is_guard(g['body']) for g in functions_by(f, code)):
                stats['guarded_functions'] += 1
            else:
                failures.append(f'R1 {rel}:{f["line"]} {f["name"]}: a public function that reaches a side-file path does not start with the native guard (and is not in FN_ALLOW)')
    for rel in FILE_ALLOW:
        if rel not in seen_file_allow: failures.append(f'R3 FILE_ALLOW stale: {rel} is not a family file any more')
    for key in FN_ALLOW:
        if key not in seen_fn_allow: failures.append(f'R3 FN_ALLOW stale: {key} is not a public reacher any more')
    print('native_family_guard_policy:', dict(stats))
    if '--report' in sys.argv:
        print('\n'.join(failures))
    if failures:
        print('\n'.join(failures[:80])); print(f'FAIL: {len(failures)} policy violation(s)'); return 1
    print('PASS'); return 0

def functions_by(f, code):
    return []   # overloads are distinct entries in `analyse`; kept for symmetry

FILE_ALLOW_MAY_HAVE_GUARD = set()
GENERIC_NAMES = {'Path', 'Load', 'Open', 'Replay', 'Publish', 'publish', 'journal', 'Journal', 'Record', 'Append', 'Run'}

if __name__ == '__main__':
    sys.exit(main())
