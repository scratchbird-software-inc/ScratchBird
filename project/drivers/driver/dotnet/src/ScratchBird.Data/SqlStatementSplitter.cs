// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Collections.Generic;
using System.Text.RegularExpressions;
using System.Text;

namespace ScratchBird.Data;

/// <summary>
/// Canonical SET TERM- and comment-aware SQL statement chunker shared by the
/// .NET driver library (<see cref="ScratchBirdCommand"/>) and the sb_isql_dotnet
/// tool. Keeping ONE implementation here (instead of a hand-maintained copy per
/// call site) is what prevents the splitter divergence the chunker conformance
/// fixture guards against.
///
/// Behavior mirrors the Python reference
/// (drivers/driver/python/src/scratchbird/sql.py::split_top_level_statements)
/// and the C++ reference (drivers/driver/cpp/tools/sb_statement_chunker.hpp),
/// and is verified against
/// tests/conformance/drivers/chunker_conformance/cases.json.
/// </summary>
public static class SqlStatementSplitter
{
    private static readonly Regex BeginScriptRegex = new(
        @"^--\s*begin_script:\s*(\S.*?)\s*$",
        RegexOptions.CultureInvariant);
    private static readonly Regex EndScriptRegex = new(
        @"^--\s*end_script:\s*(\S.*?)\s*$",
        RegexOptions.CultureInvariant);

    /// <summary>
    /// Split SQL into top-level statements on the active terminator.
    ///
    /// Quote-aware (single/double quotes) and <c>--</c> comment-aware. Honors the
    /// <c>SET TERM &lt;terminator&gt;</c> client directive: the directive changes
    /// the active terminator and is consumed
    /// (not emitted, not counted in statement indexing). With no SET TERM present,
    /// behavior is a plain quote-aware top-level <c>;</c> split, so existing
    /// scripts and statement indices are stable.
    /// </summary>
    public static IReadOnlyList<string> Split(string sql)
    {
        var statements = new List<string>();
        if (string.IsNullOrEmpty(sql))
        {
            return statements;
        }
        if (IsSingleProceduralCreatePayload(sql))
        {
            var trimmed = sql.Trim();
            return trimmed.Length == 0 ? statements : [trimmed];
        }

        var term = ";";
        var buffer = new StringBuilder();
        var inSingle = false;
        var inDouble = false;

        void Flush()
        {
            var chunk = buffer.ToString().Trim();
            if (chunk.Length == 0)
            {
                return;
            }
            var newTerm = SetTermDirective(chunk);
            if (newTerm != null)
            {
                term = newTerm;
                return;
            }
            statements.Add(chunk);
        }

        var i = 0;
        var n = sql.Length;
        while (i < n)
        {
            var ch = sql[i];
            if (!inSingle && !inDouble && ch == '-' && i + 1 < n && sql[i + 1] == '-')
            {
                // `--` line comment: copy to end of line verbatim, without scanning
                // for the terminator or quotes inside it. ';'/terminator chars in a
                // comment never split.
                var eol = sql.IndexOf('\n', i);
                if (eol < 0)
                {
                    eol = n;
                }
                buffer.Append(sql, i, eol - i);
                i = eol;
                continue;
            }
            if (ch == '\'' && !inDouble)
            {
                inSingle = !inSingle;
                buffer.Append(ch);
                i++;
                continue;
            }
            if (ch == '"' && !inSingle)
            {
                inDouble = !inDouble;
                buffer.Append(ch);
                i++;
                continue;
            }
            if (!inSingle && !inDouble && term.Length > 0 && MatchesAt(sql, i, term))
            {
                var matchedLen = term.Length; // capture before Flush() may change term
                Flush();
                buffer.Clear();
                i += matchedLen;
                continue;
            }
            buffer.Append(ch);
            i++;
        }
        Flush();
        return statements;
    }

    /// <summary>
    /// Split a compiled full-surface chain into per-script statements.
    /// </summary>
    public static IReadOnlyList<ChainStatement> SplitChain(string chain)
    {
        var statements = new List<ChainStatement>();
        if (string.IsNullOrEmpty(chain))
        {
            return statements;
        }

        string? currentName = null;
        var buffer = new List<string>();
        var capturing = false;

        void Flush()
        {
            if (currentName == null)
            {
                return;
            }
            var index = 1;
            foreach (var statement in Split(string.Join("\n", buffer)))
            {
                statements.Add(new ChainStatement(currentName, index, statement));
                index++;
            }
        }

        foreach (var line in chain.Replace("\r\n", "\n").Split('\n'))
        {
            var begin = BeginScriptRegex.Match(line);
            if (begin.Success)
            {
                currentName = begin.Groups[1].Value;
                buffer.Clear();
                capturing = true;
                continue;
            }
            if (capturing && EndScriptRegex.IsMatch(line))
            {
                Flush();
                currentName = null;
                buffer.Clear();
                capturing = false;
                continue;
            }
            if (capturing)
            {
                buffer.Add(line);
            }
        }

        return statements;
    }

    private static bool MatchesAt(string sql, int index, string term)
    {
        if (index + term.Length > sql.Length)
        {
            return false;
        }
        return string.CompareOrdinal(sql, index, term, 0, term.Length) == 0;
    }

    private static bool IsSingleProceduralCreatePayload(string sql)
    {
        var trimmed = StripLeadingLineComments(sql).Trim();
        if (!StartsWithWord(trimmed, "CREATE FUNCTION") &&
            !StartsWithWord(trimmed, "CREATE PROCEDURE") &&
            !StartsWithWord(trimmed, "CREATE TRIGGER"))
        {
            return false;
        }
        var upper = trimmed.ToUpperInvariant();
        return upper.Contains("\nAS\nBEGIN") ||
               upper.Contains("\nAS\r\nBEGIN") ||
               upper.Contains("\nAS\nDECLARE") ||
               upper.Contains("\nAS\r\nDECLARE") ||
               upper.Contains(" AS BEGIN") ||
               upper.Contains(" AS DECLARE") ||
               upper.Contains("\nBEGIN") ||
               upper.Contains(" BEGIN");
    }

    private static string StripLeadingLineComments(string sql)
    {
        var cursor = 0;
        while (cursor < sql.Length)
        {
            while (cursor < sql.Length && char.IsWhiteSpace(sql[cursor]))
            {
                cursor++;
            }
            if (cursor + 1 >= sql.Length || sql[cursor] != '-' || sql[cursor + 1] != '-')
            {
                break;
            }
            var eol = sql.IndexOf('\n', cursor);
            if (eol < 0)
            {
                return string.Empty;
            }
            cursor = eol + 1;
        }
        return sql[cursor..];
    }

    private static bool StartsWithWord(string text, string word)
    {
        if (!text.StartsWith(word, System.StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        return text.Length == word.Length || char.IsWhiteSpace(text[word.Length]);
    }

    /// <summary>
    /// Return the new terminator if <paramref name="chunk"/> is a
    /// <c>SET TERM &lt;terminator&gt;</c> client directive, else <c>null</c>.
    /// Leading full-line <c>--</c> comments and blank lines are ignored when
    /// matching, so a directive may be preceded by comment lines.
    /// </summary>
    private static string? SetTermDirective(string chunk)
    {
        var meaningful = new StringBuilder();
        foreach (var rawLine in chunk.Split('\n'))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line.StartsWith("--", System.StringComparison.Ordinal))
            {
                continue;
            }
            if (meaningful.Length > 0)
            {
                meaningful.Append(' ');
            }
            meaningful.Append(line);
        }
        var text = meaningful.ToString();
        if (!text.StartsWith("set term", System.StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }
        var rest = text.Substring("set term".Length).Trim();
        return rest.Length > 0 ? rest : null;
    }
}

public sealed record ChainStatement(string ScriptName, int StatementIndex, string Sql);
