// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
//
// Canonical SET TERM- and comment-aware statement chunker for the Dart driver
// tools (sb_isql_dart). Keeping ONE implementation here (instead of a
// hand-maintained copy per tool) prevents the splitter divergence the chunker
// conformance fixture guards against.
//
// Behavior mirrors the Python reference
// (drivers/driver/python/src/scratchbird/sql.py::split_top_level_statements)
// and the C++ reference (drivers/driver/cpp/tools/sb_statement_chunker.hpp),
// and is verified against
// tests/conformance/drivers/chunker_conformance/cases.json.

/// Return the new terminator if [chunk] is a `SET TERM <terminator>` client
/// directive, else `null`. Leading full-line `--` comments and blank lines are
/// ignored when matching, so a directive may be preceded by comment lines in
/// the same chunk.
String? _setTermDirective(String chunk) {
  final meaningful = <String>[];
  for (final line in chunk.split('\n')) {
    final stripped = line.trim();
    if (stripped.isEmpty || stripped.startsWith('--')) {
      continue;
    }
    meaningful.add(stripped);
  }
  if (meaningful.isEmpty) {
    return null;
  }
  final joined = meaningful.join(' ');
  final match = RegExp(
    r'^set\s+term\s+(\S.*?)\s*$',
    caseSensitive: false,
  ).firstMatch(joined);
  if (match == null) {
    return null;
  }
  return match.group(1)!.trim();
}

final _beginScriptRegex = RegExp(r'^--\s*begin_script:\s*(\S.*?)\s*$');
final _endScriptRegex = RegExp(r'^--\s*end_script:\s*(\S.*?)\s*$');

class ChainStatement {
  final String scriptName;
  final int statementIndex;
  final String sql;

  const ChainStatement(this.scriptName, this.statementIndex, this.sql);
}

/// Split SQL into top-level statements on the active terminator.
///
/// Quote-aware (single/double quotes) and `--` comment-aware. Honors the
/// `SET TERM <terminator>` client directive: the
/// directive changes the active terminator and is consumed — it is not emitted
/// as a statement and is not counted in statement indexing. This lets
/// procedural bodies (functions, procedures, triggers) contain inner `;`
/// between `SET TERM ^` and the restoring `SET TERM ;^`.
///
/// With no `SET TERM` directive present, the behavior is identical to a plain
/// quote-aware top-level `;` split, so existing scripts and statement indices
/// are unchanged. (The chosen terminator must not appear in the bodies it
/// wraps.)
List<String> splitStatements(String script) {
  final statements = <String>[];
  var term = ';';
  final buf = StringBuffer();
  var inSingle = false;
  var inDouble = false;

  void flush() {
    final chunk = buf.toString().trim();
    if (chunk.isEmpty) {
      return;
    }
    final newTerm = _setTermDirective(chunk);
    if (newTerm != null && newTerm.isNotEmpty) {
      term = newTerm;
      return;
    }
    statements.add(chunk);
  }

  var i = 0;
  final n = script.length;
  while (i < n) {
    final ch = script[i];
    if (!inSingle &&
        !inDouble &&
        ch == '-' &&
        i + 1 < n &&
        script[i + 1] == '-') {
      // `--` line comment: copy to end of line verbatim, without scanning for
      // the terminator or quotes inside it. ';'/terminator chars inside a
      // comment never split.
      var eol = script.indexOf('\n', i);
      if (eol == -1) {
        eol = n;
      }
      buf.write(script.substring(i, eol));
      i = eol;
      continue;
    }
    if (ch == "'" && !inDouble) {
      inSingle = !inSingle;
      buf.write(ch);
      i += 1;
      continue;
    }
    if (ch == '"' && !inSingle) {
      inDouble = !inDouble;
      buf.write(ch);
      i += 1;
      continue;
    }
    if (!inSingle &&
        !inDouble &&
        term.isNotEmpty &&
        script.startsWith(term, i)) {
      final matchedLen = term.length; // capture before flush() may change term
      flush();
      buf.clear();
      i += matchedLen;
      continue;
    }
    buf.write(ch);
    i += 1;
  }
  flush();
  return statements;
}

/// Split a compiled full-surface chain into original per-script statements.
///
/// Each `-- begin_script:` marker resets the active terminator to `;` and the
/// emitted statement index to 1. Content outside marker pairs is ignored.
List<ChainStatement> splitChainStatements(String chain) {
  final statements = <ChainStatement>[];
  String? currentName;
  var buffer = <String>[];
  var capturing = false;

  void flush() {
    final name = currentName;
    if (name == null) {
      return;
    }
    final split = splitStatements(buffer.join('\n'));
    for (var i = 0; i < split.length; i++) {
      statements.add(ChainStatement(name, i + 1, split[i]));
    }
  }

  for (final line in chain.replaceAll('\r\n', '\n').split('\n')) {
    final begin = _beginScriptRegex.firstMatch(line);
    if (begin != null) {
      currentName = begin.group(1);
      buffer = <String>[];
      capturing = true;
      continue;
    }
    if (capturing && _endScriptRegex.hasMatch(line)) {
      flush();
      currentName = null;
      buffer = <String>[];
      capturing = false;
      continue;
    }
    if (capturing) {
      buffer.add(line);
    }
  }
  if (capturing) {
    flush();
  }
  return statements;
}
