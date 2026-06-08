// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data.Common;

namespace ScratchBird.Data;

public class ScratchBirdException : DbException
{
    public string? SqlState { get; }
    public string? Detail { get; }
    public string? Hint { get; }

    public ScratchBirdException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message)
    {
        SqlState = sqlState;
        Detail = detail;
        Hint = hint;
    }
}

public class ScratchBirdWarning : ScratchBirdException
{
    public ScratchBirdWarning(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdNoDataException : ScratchBirdException
{
    public ScratchBirdNoDataException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdConnectionException : ScratchBirdException
{
    public ScratchBirdConnectionException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdNotSupportedException : ScratchBirdException
{
    public ScratchBirdNotSupportedException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdDataException : ScratchBirdException
{
    public ScratchBirdDataException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdIntegrityException : ScratchBirdException
{
    public ScratchBirdIntegrityException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdAuthException : ScratchBirdException
{
    public ScratchBirdAuthException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdTransactionException : ScratchBirdException
{
    public ScratchBirdTransactionException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdSyntaxException : ScratchBirdException
{
    public ScratchBirdSyntaxException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdResourceException : ScratchBirdException
{
    public ScratchBirdResourceException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdLimitException : ScratchBirdException
{
    public ScratchBirdLimitException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdOperatorInterventionException : ScratchBirdException
{
    public ScratchBirdOperatorInterventionException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdSystemException : ScratchBirdException
{
    public ScratchBirdSystemException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public class ScratchBirdInternalException : ScratchBirdException
{
    public ScratchBirdInternalException(string message, string? sqlState = null, string? detail = null, string? hint = null)
        : base(message, sqlState, detail, hint) { }
}

public enum ScratchBirdRetryScope
{
    None,
    Reconnect,
    Statement,
    Transaction
}

public static class ScratchBirdSqlStateMapper
{
    public static ScratchBirdException Create(string message, string? sqlState, string? detail, string? hint)
    {
        if (string.IsNullOrEmpty(sqlState) || sqlState.Length < 2)
        {
            return new ScratchBirdException(message, sqlState, detail, hint);
        }

        var mappedExact = MapByExactState(message, sqlState, detail, hint);
        if (mappedExact is not null)
        {
            return mappedExact;
        }

        var mappedClass = MapByStateClass(message, sqlState, detail, hint);
        return mappedClass ?? new ScratchBirdException(message, sqlState, detail, hint);
    }

    public static ScratchBirdRetryScope RetryScopeForSqlState(string? sqlState)
    {
        // Drivers are fail-closed: fresh statement restart for 40xxx,
        // reconnect only for 08xxx, and no automatic whole-transaction replay.
        if (string.IsNullOrEmpty(sqlState) || sqlState.Length != 5)
        {
            return ScratchBirdRetryScope.None;
        }

        return sqlState switch
        {
            "40001" or "40P01" => ScratchBirdRetryScope.Statement,
            _ when sqlState.StartsWith("08", StringComparison.Ordinal) => ScratchBirdRetryScope.Reconnect,
            _ => ScratchBirdRetryScope.None
        };
    }

    public static bool IsRetryableSqlState(string? sqlState)
    {
        return RetryScopeForSqlState(sqlState) != ScratchBirdRetryScope.None;
    }

    private static ScratchBirdException? MapByExactState(string message, string sqlState, string? detail, string? hint)
    {
        return sqlState switch
        {
            "01000" => new ScratchBirdWarning(message, sqlState, detail, hint),
            "02000" => new ScratchBirdNoDataException(message, sqlState, detail, hint),
            "08001" or "08003" or "08004" or "08006" or "08P01" => new ScratchBirdConnectionException(message, sqlState, detail, hint),
            "0A000" => new ScratchBirdNotSupportedException(message, sqlState, detail, hint),
            "22001" or "22003" or "22007" or "22012" or "22023" or "22P02" or "22P03" => new ScratchBirdDataException(message, sqlState, detail, hint),
            "23000" or "23502" or "23503" or "23505" or "23514" => new ScratchBirdIntegrityException(message, sqlState, detail, hint),
            "28000" or "28P01" => new ScratchBirdAuthException(message, sqlState, detail, hint),
            "40001" or "40P01" => new ScratchBirdTransactionException(message, sqlState, detail, hint),
            "42501" or "42601" or "42703" or "42704" or "42710" or "42883" or "42P01" or "42P07" => new ScratchBirdSyntaxException(message, sqlState, detail, hint),
            "53P00" or "53100" or "53200" or "53300" => new ScratchBirdResourceException(message, sqlState, detail, hint),
            "54000" => new ScratchBirdLimitException(message, sqlState, detail, hint),
            "57014" or "57P01" or "57P03" => new ScratchBirdOperatorInterventionException(message, sqlState, detail, hint),
            "58000" => new ScratchBirdSystemException(message, sqlState, detail, hint),
            "XX000" => new ScratchBirdInternalException(message, sqlState, detail, hint),
            _ => null
        };
    }

    private static ScratchBirdException? MapByStateClass(string message, string sqlState, string? detail, string? hint)
    {
        var stateClass = sqlState[..2];
        return stateClass switch
        {
            "01" => new ScratchBirdWarning(message, sqlState, detail, hint),
            "02" => new ScratchBirdNoDataException(message, sqlState, detail, hint),
            "08" => new ScratchBirdConnectionException(message, sqlState, detail, hint),
            "0A" => new ScratchBirdNotSupportedException(message, sqlState, detail, hint),
            "22" => new ScratchBirdDataException(message, sqlState, detail, hint),
            "23" => new ScratchBirdIntegrityException(message, sqlState, detail, hint),
            "28" => new ScratchBirdAuthException(message, sqlState, detail, hint),
            "40" => new ScratchBirdTransactionException(message, sqlState, detail, hint),
            "42" => new ScratchBirdSyntaxException(message, sqlState, detail, hint),
            "53" => new ScratchBirdResourceException(message, sqlState, detail, hint),
            "54" => new ScratchBirdLimitException(message, sqlState, detail, hint),
            "57" => new ScratchBirdOperatorInterventionException(message, sqlState, detail, hint),
            "58" => new ScratchBirdSystemException(message, sqlState, detail, hint),
            "XX" => new ScratchBirdInternalException(message, sqlState, detail, hint),
            _ => null
        };
    }
}
