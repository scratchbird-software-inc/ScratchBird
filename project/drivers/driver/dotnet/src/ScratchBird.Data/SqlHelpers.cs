// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Linq;
using System.Text;

namespace ScratchBird.Data;

internal sealed record NormalizedQuery(string Sql, List<ScratchBirdParameter> Parameters);

internal static class SqlHelpers
{
    public static NormalizedQuery Normalize(string sql, IReadOnlyList<ScratchBirdParameter> parameters)
    {
        if (parameters.Count == 0)
        {
            return new NormalizedQuery(sql, new List<ScratchBirdParameter>());
        }

        if (HasNamedParameters(sql))
        {
            return RewriteNamed(sql, parameters);
        }

        if (sql.Contains('?'))
        {
            return RewritePositional(sql, parameters);
        }

        return new NormalizedQuery(sql, parameters.ToList());
    }

    public static NormalizedQuery NormalizeCallable(string sql, IReadOnlyList<ScratchBirdParameter> parameters)
    {
        var callableSql = NormalizeCallableSql(sql);
        return Normalize(callableSql, parameters);
    }

    public static string NormalizeCallableSql(string sql)
    {
        var trimmed = sql.Trim();
        if (!(trimmed.StartsWith('{') && trimmed.EndsWith('}')))
        {
            return sql;
        }

        var inner = trimmed[1..^1].Trim();
        if (inner.Length == 0)
        {
            return sql;
        }

        if (inner.StartsWith('?'))
        {
            var afterQuestion = inner[1..].TrimStart();
            if (afterQuestion.StartsWith('='))
            {
                var afterEquals = afterQuestion[1..].TrimStart();
                if (StartsWithCall(afterEquals))
                {
                    var invocation = ParseCallableInvocation(afterEquals[4..].TrimStart());
                    var args = invocation.HasParens ? invocation.Args : string.Empty;
                    return $"select {invocation.Routine}({args}) as return_value";
                }
            }
        }

        if (StartsWithCall(inner))
        {
            var invocation = ParseCallableInvocation(inner[4..].TrimStart());
            return invocation.HasParens
                ? $"call {invocation.Routine}({invocation.Args})"
                : $"call {invocation.Routine}";
        }

        return sql;
    }

    private static bool HasNamedParameters(string sql)
    {
        var inString = false;
        for (var i = 0; i + 1 < sql.Length; i++)
        {
            var ch = sql[i];
            if (ch == '\'')
            {
                inString = !inString;
                continue;
            }
            if (inString)
            {
                continue;
            }
            if (ch == ':' && ((i + 1 < sql.Length && sql[i + 1] == ':') || (i > 0 && sql[i - 1] == ':')))
            {
                continue;
            }
            if ((ch == ':' || ch == '@') && IsIdentStart(sql[i + 1]))
            {
                return true;
            }
        }
        return false;
    }

    private static NormalizedQuery RewriteNamed(string sql, IReadOnlyList<ScratchBirdParameter> parameters)
    {
        var lookup = new Dictionary<string, ScratchBirdParameter>(StringComparer.OrdinalIgnoreCase);
        foreach (var param in parameters)
        {
            if (!string.IsNullOrEmpty(param.ParameterName))
            {
                lookup[param.ParameterName.TrimStart('@', ':')] = param;
            }
        }

        var ordered = new List<ScratchBirdParameter>();
        var sb = new StringBuilder();
        var inString = false;

        for (var i = 0; i < sql.Length;)
        {
            var ch = sql[i];
            if (ch == '\'')
            {
                inString = !inString;
                sb.Append(ch);
                i++;
                continue;
            }
            if (!inString && ch == ':' && ((i + 1 < sql.Length && sql[i + 1] == ':') || (i > 0 && sql[i - 1] == ':')))
            {
                sb.Append(ch);
                i++;
                continue;
            }
            if (!inString && (ch == ':' || ch == '@') && i + 1 < sql.Length && IsIdentStart(sql[i + 1]))
            {
                var j = i + 1;
                while (j < sql.Length && IsIdentPart(sql[j]))
                {
                    j++;
                }
                var key = sql.Substring(i + 1, j - i - 1);
                if (!lookup.TryGetValue(key, out var param))
                {
                    throw new InvalidOperationException($"missing named parameter: {key}");
                }
                ordered.Add(param);
                sb.Append('$').Append(ordered.Count);
                i = j;
                continue;
            }
            sb.Append(ch);
            i++;
        }

        return new NormalizedQuery(sb.ToString(), ordered);
    }

    private static NormalizedQuery RewritePositional(string sql, IReadOnlyList<ScratchBirdParameter> parameters)
    {
        var ordered = new List<ScratchBirdParameter>();
        var sb = new StringBuilder();
        var inString = false;
        var index = 0;

        for (var i = 0; i < sql.Length;)
        {
            var ch = sql[i];
            if (ch == '\'')
            {
                inString = !inString;
                sb.Append(ch);
                i++;
                continue;
            }
            if (!inString && ch == '?')
            {
                if (index >= parameters.Count)
                {
                    throw new InvalidOperationException("not enough parameters");
                }
                ordered.Add(parameters[index]);
                index++;
                sb.Append('$').Append(ordered.Count);
                i++;
                continue;
            }
            sb.Append(ch);
            i++;
        }

        if (index < parameters.Count)
        {
            throw new InvalidOperationException("too many parameters");
        }

        return new NormalizedQuery(sb.ToString(), ordered);
    }

    private static bool IsIdentStart(char ch)
    {
        return char.IsLetter(ch) || ch == '_';
    }

    private static bool IsIdentPart(char ch)
    {
        return char.IsLetterOrDigit(ch) || ch == '_';
    }

    private static bool StartsWithCall(string value)
    {
        return value.StartsWith("call", StringComparison.OrdinalIgnoreCase);
    }

    private static CallableInvocation ParseCallableInvocation(string value)
    {
        var openParen = value.IndexOf('(');
        if (openParen < 0)
        {
            var routineOnly = value.Trim();
            if (routineOnly.Length == 0)
            {
                throw new InvalidOperationException("invalid JDBC escape call syntax");
            }
            return new CallableInvocation(routineOnly, string.Empty, false);
        }

        var inSingle = false;
        var inDouble = false;
        var depth = 0;
        var closeParen = -1;
        for (var i = openParen; i < value.Length; i++)
        {
            var ch = value[i];
            if (ch == '\'' && !inDouble)
            {
                inSingle = !inSingle;
                continue;
            }
            if (ch == '"' && !inSingle)
            {
                inDouble = !inDouble;
                continue;
            }
            if (inSingle || inDouble)
            {
                continue;
            }
            if (ch == '(')
            {
                depth++;
                continue;
            }
            if (ch == ')')
            {
                depth--;
                if (depth == 0)
                {
                    closeParen = i;
                    break;
                }
            }
        }

        if (closeParen < 0)
        {
            throw new InvalidOperationException("invalid JDBC escape call syntax");
        }

        var routine = value[..openParen].Trim();
        if (routine.Length == 0)
        {
            throw new InvalidOperationException("invalid JDBC escape call syntax");
        }
        var trailing = value[(closeParen + 1)..].Trim();
        if (trailing.Length > 0)
        {
            throw new InvalidOperationException("invalid JDBC escape call syntax");
        }

        var args = value[(openParen + 1)..closeParen].Trim();
        return new CallableInvocation(routine, args, true);
    }

    private readonly record struct CallableInvocation(string Routine, string Args, bool HasParens);
}
