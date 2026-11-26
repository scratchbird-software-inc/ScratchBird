#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
#
# SEARCH_KEY: SB_NATIVE_NAME_FOLD_FREE (Part B step S3, 2026-12-01).
# Owner rulings 2026-11-20..26: the engine stores names as given and uses them ONLY in the exact UUID<->path.name procedures;
# folding, quoting, text splitting, search-path walks and language fallback are PARSER rules. This test is the grep half of S3:
#
#   1. The fold machinery (ProfileFold, NameRegistryLookupKey, NameRegistryEntryOwnLaneKey, NameRegistryEntryMatchesLookupKey,
#      NameRegistryLookupKeysCollide), the native profile projection (NativeProfileLabel, NativeNameProfileValue) and the retired
#      SBLR_NAME_RESOLVE descriptor fields (canonical_name_utf8, case_folding_profile) are referenced, in CODE (comments and string
#      literals are stripped), by NO production file except the ALLOWED_LEGACY list below. That list IS the remaining legacy-cut work:
#      every file on it holds a fold that only a LEGACY (side-file) context reaches; the legacy cut deletes it family by family.
#   2. Each allowed file that MIXES native and legacy code is checked structurally: the native functions contain none of the tokens,
#      and each legacy entry point is GUARDED by a native refusal that precedes its first fold.
#
# Usage: native_name_fold_free_test.py <project-root>      (exit 0 = pass)
import os
import re
import sys

TOKENS = [
    "ProfileFold", "NameRegistryLookupKey", "NameRegistryEntryOwnLaneKey", "NameRegistryEntryMatchesLookupKey",
    "NameRegistryLookupKeysCollide", "NativeProfileLabel", "NativeNameProfileValue", "canonical_name_utf8",
    "case_folding_profile",
]

# file (relative to src/) -> why it still holds a fold (all of them legacy-only)
ALLOWED_LEGACY = {
    "engine/internal_api/catalog/name_registry.cpp":
        "the LEGACY resolver, key derivation and collision check; every native entry point branches off before them",
    "engine/internal_api/catalog/name_registry.hpp":
        "declarations of the legacy key functions",
    "engine/internal_api/catalog/catalog_object_lifecycle.cpp":
        "the legacy lifecycle store's keys; a native context is refused at its entry points (NativeServedContext)",
    "engine/internal_api/catalog/relation_descriptor_projection.cpp":
        "folded name compare after the native byte-equality branch in NameEntryMatches",
    "engine/internal_api/ddl/create_api_00_common_helpers.inc":
        "folded segment compare after the native byte-equality branch in NameMatchesIdentifierSegmentLocal",
    "engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp":
        "side-file journal; a native context is refused in ValidateInputs and Recover before the fold",
    "engine/sblr/sblr_dispatch.cpp":
        "ResolveSchemaParentPathToUuid: legacy text parent path, guarded by NativeServedContext",
    "server/ipc_server.cpp":
        "PsNameResolveUniqueRegistryLeaf and the dotted-text frame: a native session is refused before them (S5 replaces the wire)",
    "server/sblr_dispatch_server.cpp":
        "dotted-text dispatch resolvers: a native session is refused (DispatchNativeTextPathRefused) before the split and the fold",
}


def strip_code(text, keep_literals=False):
    """Remove comments and (unless keep_literals) string/char literals; keeps line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text.startswith('R"', i):  # raw string R"delim( ... )delim"
            m = re.match(r'R"([^()\\ ]*)\(', text[i:])
            if m:
                end = text.find(")" + m.group(1) + '"', i)
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append("\n" * text.count("\n", i, end))
                i = end
            else:
                out.append(c)
                i += 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1] if keep_literals else '""')
            i = j + 1
        elif c == "'" and not (i > 0 and text[i - 1].isalnum()):
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1] if keep_literals else "''")
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def function_body(code, signature_regex):
    """The brace-balanced body of the first definition whose header matches signature_regex (None if absent)."""
    m = re.search(signature_regex + r"[^;{]*\{", code)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(code) and depth:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.start():i]


failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def main(root):
    src = os.path.join(root, "src")
    codes = {}
    for base, _dirs, files in os.walk(src):
        for name in files:
            if not name.endswith((".cpp", ".hpp", ".inc", ".h")):
                continue
            path = os.path.join(base, name)
            rel = os.path.relpath(path, src)
            with open(path, encoding="utf-8", errors="replace") as handle:
                raw = handle.read()
            # Speed (suite timeout at 60 s under load: this scan is pure-Python and char-by-char): stripping comments and
            # literals only REMOVES text, so a token that is absent from the raw text cannot be in the stripped code.
            # Skip the expensive strip for files whose raw text names no token at all (same result, about 50x faster).
            if not any(t in raw for t in TOKENS):
                continue
            code = strip_code(raw)
            hits = sorted(t for t in TOKENS if re.search(r"\b" + t + r"\b", code))
            if hits:
                codes[rel] = (code, hits)
    # 1. no token outside the allowed legacy list
    for rel, (_code, hits) in sorted(codes.items()):
        check(rel in ALLOWED_LEGACY, "fold token(s) %s in %s, which is not on the allowed-legacy list" % (hits, rel))
    for rel in ALLOWED_LEGACY:
        check(rel in codes, "%s is on the allowed-legacy list but no longer holds a token: remove it from the list" % rel)
    # the SBLR descriptor, the native projection and the facts carry none, anywhere
    for rel in ("engine/sblr/sblr_name_resolve_runtime.hpp", "engine/internal_api/native_served_database_facts.hpp",
                "engine/public_abi.cpp", "engine/internal_api/sblr_name_resolve_journal.cpp", "engine/internal_api/catalog/name_list_api.cpp"):
        check(rel not in codes, "%s must hold no fold token" % rel)

    # 2. structure of the mixed files
    def load(rel):
        return codes[rel][0] if rel in codes else None

    registry = load("engine/internal_api/catalog/name_registry.cpp")
    if registry is not None:
        for fn in ("LoadNativeNameRegistryState", "NativeExactEntry", "ResolveNativeExactName", "NativeMapUuidToName",
                   "NativeLanguageUuidOfTag", "NameRegistryLanguageUuidOfTag"):
            body = function_body(registry, r"\b" + fn + r"\s*\(")
            check(body is not None, "name_registry.cpp: native function %s not found" % fn)
            if body is not None:
                for t in TOKENS:
                    check(not re.search(r"\b" + t + r"\b", body), "name_registry.cpp: native function %s uses %s" % (fn, t))
        for fn, guard in (("ResolveNameRegistryPrivate", "NativeServedContext(request.context)) return ResolveNativeExactName"),
                          ("MapNameRegistryUuidToNamePrivate", "NativeServedContext(request.context)) return NativeMapUuidToName"),
                          ("MapNameRegistryUuidToNamePublic", "NativeServedContext(request.context)) return NativeMapUuidToName")):
            body = function_body(registry, r"\b" + fn + r"\s*\(")
            check(body is not None and guard in body and (
                  min([body.find(t) for t in TOKENS if body.find(t) >= 0] or [10 ** 9]) > body.find(guard)),
                  "name_registry.cpp: %s is not guarded by a native branch before its first fold" % fn)
        body = function_body(registry, r"\bNameRegistryWouldConflict\s*\(")
        check(body is not None and "NativeServedContext(context)" in body and (
              min([body.find(t) for t in TOKENS if body.find(t) >= 0] or [10 ** 9]) > body.find("NativeServedContext(context)")),
              "name_registry.cpp: NameRegistryWouldConflict has no native byte-equality branch before its fold")

    dispatch = load("engine/sblr/sblr_dispatch.cpp")
    if dispatch is not None:
        body = function_body(dispatch, r"\bResolveSchemaParentPathToUuid\s*\(")
        check(body is not None and "NativeServedContext(request.context)" in body and
              body.find("NativeServedContext(request.context)") < body.find("NameRegistryLookupKey"),
              "sblr_dispatch.cpp: ResolveSchemaParentPathToUuid has no native refusal before its fold")

    for rel, fn, guard in (
            ("engine/internal_api/catalog/relation_descriptor_projection.cpp", "NameEntryMatches", "NativeServedContext(context)"),
            ("engine/internal_api/ddl/create_api_00_common_helpers.inc", "NameMatchesIdentifierSegmentLocal", "NativeServedContext(context)"),
            ("engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp", "ValidateInputs", "NativeServedContext(context)"),
            ("engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp", "RecoverSblrDdlCreateSchemaExecutionJournalV1",
             "NativeServedContext(authenticated_context)")):
        code = load(rel)
        body = function_body(code, r"\b" + fn + r"\s*\(") if code else None
        check(body is not None and guard in body, "%s: %s has no native guard" % (rel, fn))
        if body is not None and guard in body:
            firsts = [body.find(t) for t in TOKENS if body.find(t) >= 0]
            check(not firsts or min(firsts) > body.find(guard), "%s: %s folds before its native guard" % (rel, fn))
    # the journal's fold lives in ExactRecoveryPostcondition, reached only after ValidateInputs (guarded) or Recover (guarded):
    journal = load("engine/internal_api/sblr_ddl_create_schema_execution_journal.cpp")
    if journal is not None:
        check("ExactRecoveryPostcondition(" in journal, "create-schema journal: the postcondition function moved; re-check its guards")

    server = None
    if "server/ipc_server.cpp" in codes:
        with open(os.path.join(src, "server/ipc_server.cpp"), encoding="utf-8", errors="replace") as handle:
            server = strip_code(handle.read(), keep_literals=True)
    if server is not None:
        body = function_body(server, r"\bResolveNamePublicFrame\s*\(")
        guard = "ENGINE.NATIVE.WIRE_NAME_RESOLVE_S5"
        check(body is not None and guard in body, "ipc_server.cpp: the dotted-text frame handler has no native refusal")
        if body is not None and guard in body:
            for needle in ("PsNameLower(decoded->presented_name", "PsNameSplitPresentedName(", "PsNameResolveUniqueRegistryLeaf(",
                           "EngineResolveName(", "AdoptAndFindExactActiveDefaultTransaction(", "PsNameCanonicalIdentifierProfile("):
                check(body.find(needle) > body.find(guard), "ipc_server.cpp: %s precedes the native refusal" % needle)

    dispatch_server = load("server/sblr_dispatch_server.cpp")
    if dispatch_server is not None:
        for fn in ("ResolveObjectPathForDispatch", "ResolveSchemaParentPathForDispatch", "ResolveDefaultSchemaForDispatch",
                   "ResolveDomainTypePathForDispatch", "ResolveSequencePathForDispatch", "ResolveRelationPathForDispatch"):
            matches = [m.start() for m in re.finditer(r"\b" + fn + r"\s*\([^;{]*\)\s*\{", dispatch_server)]
            body = function_body(dispatch_server, r"\b" + fn + r"\s*\([^;{]*\)")
            check(bool(matches) and body is not None and "DispatchNativeTextPathRefused(session)" in body and
                  (body.find("SplitDispatchIdentifierPath(") < 0 or
                   body.find("DispatchNativeTextPathRefused(session)") < body.find("SplitDispatchIdentifierPath(")),
                  "sblr_dispatch_server.cpp: %s is not guarded by DispatchNativeTextPathRefused before its split" % fn)

    # 3. the descriptor never joins or splits: the codec holds no '.' join of atoms
    with open(os.path.join(src, "engine/sblr/sblr_name_resolve_runtime.hpp"), encoding="utf-8") as handle:
        raw_codec = strip_code(handle.read(), keep_literals=True)
    check("'.'" not in raw_codec and '"."' not in raw_codec,
          "sblr_name_resolve_runtime.hpp: a '.' separator literal appears in the descriptor codec")

    if failures:
        for message in failures:
            print("FAIL: " + message)
        return 1
    print("NATIVE_NAME_FOLD_FREE_OK files_with_tokens=%d allowed_legacy=%d" % (len(codes), len(ALLOWED_LEGACY)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
