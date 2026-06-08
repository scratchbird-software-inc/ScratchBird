<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

class ScratchBirdException extends \RuntimeException
{
    public string $sqlState = '';
    public string $detail = '';
    public string $hint = '';

    public function __construct(string $message, string $sqlState = '', string $detail = '', string $hint = '')
    {
        parent::__construct($message);
        $this->sqlState = $sqlState;
        $this->detail = $detail;
        $this->hint = $hint;
    }
}

class ScratchBirdWarning extends ScratchBirdException {}
class ScratchBirdNoDataException extends ScratchBirdException {}
class ScratchBirdConnectionException extends ScratchBirdException {}
class ScratchBirdNotSupportedException extends ScratchBirdException {}
class ScratchBirdDataException extends ScratchBirdException {}
class ScratchBirdIntegrityException extends ScratchBirdException {}
class ScratchBirdAuthException extends ScratchBirdException {}
class ScratchBirdTransactionException extends ScratchBirdException {}
class ScratchBirdSyntaxException extends ScratchBirdException {}
class ScratchBirdResourceException extends ScratchBirdException {}
class ScratchBirdLimitException extends ScratchBirdException {}
class ScratchBirdOperatorInterventionException extends ScratchBirdException {}
class ScratchBirdSystemException extends ScratchBirdException {}
class ScratchBirdInternalException extends ScratchBirdException {}

final class RetryScope
{
    public const NONE = 'none';
    public const RECONNECT = 'reconnect';
    public const STATEMENT = 'statement';
    public const TRANSACTION = 'transaction';
}

final class ErrorMapper
{
    public static function map(string $sqlState, string $message, string $detail = '', string $hint = ''): ScratchBirdException
    {
        $text = strtolower(trim(implode("\n", array_filter([$message, $detail, $hint], fn (mixed $value): bool => $value !== null && $value !== ''))));
        if (str_starts_with($sqlState, '42') && self::integrityMessage($text)) {
            $sqlState = '23000';
        }
        if (strlen($sqlState) === 5) {
            $mapped = match ($sqlState) {
                '01000' => new ScratchBirdWarning($message, $sqlState, $detail, $hint),
                '02000' => new ScratchBirdNoDataException($message, $sqlState, $detail, $hint),
                '08001', '08003', '08004', '08006', '08P01' => new ScratchBirdConnectionException($message, $sqlState, $detail, $hint),
                '0A000' => new ScratchBirdNotSupportedException($message, $sqlState, $detail, $hint),
                '22001', '22003', '22007', '22012', '22023', '22P02', '22P03' => new ScratchBirdDataException($message, $sqlState, $detail, $hint),
                '23000', '23502', '23503', '23505', '23514' => new ScratchBirdIntegrityException($message, $sqlState, $detail, $hint),
                '28000', '28P01' => new ScratchBirdAuthException($message, $sqlState, $detail, $hint),
                '40001', '40P01' => new ScratchBirdTransactionException($message, $sqlState, $detail, $hint),
                '42501', '42601', '42703', '42704', '42710', '42883', '42P01', '42P07' => new ScratchBirdSyntaxException($message, $sqlState, $detail, $hint),
                '53P00', '53100', '53200', '53300' => new ScratchBirdResourceException($message, $sqlState, $detail, $hint),
                '54000' => new ScratchBirdLimitException($message, $sqlState, $detail, $hint),
                '57014', '57P01', '57P03' => new ScratchBirdOperatorInterventionException($message, $sqlState, $detail, $hint),
                '58000' => new ScratchBirdSystemException($message, $sqlState, $detail, $hint),
                'XX000' => new ScratchBirdInternalException($message, $sqlState, $detail, $hint),
                default => null,
            };
            if ($mapped !== null) {
                return $mapped;
            }

            return match (substr($sqlState, 0, 2)) {
                '01' => new ScratchBirdWarning($message, $sqlState, $detail, $hint),
                '02' => new ScratchBirdNoDataException($message, $sqlState, $detail, $hint),
                '08' => new ScratchBirdConnectionException($message, $sqlState, $detail, $hint),
                '0A' => new ScratchBirdNotSupportedException($message, $sqlState, $detail, $hint),
                '22' => new ScratchBirdDataException($message, $sqlState, $detail, $hint),
                '23' => new ScratchBirdIntegrityException($message, $sqlState, $detail, $hint),
                '28' => new ScratchBirdAuthException($message, $sqlState, $detail, $hint),
                '40' => new ScratchBirdTransactionException($message, $sqlState, $detail, $hint),
                '42' => new ScratchBirdSyntaxException($message, $sqlState, $detail, $hint),
                '53' => new ScratchBirdResourceException($message, $sqlState, $detail, $hint),
                '54' => new ScratchBirdLimitException($message, $sqlState, $detail, $hint),
                '57' => new ScratchBirdOperatorInterventionException($message, $sqlState, $detail, $hint),
                '58' => new ScratchBirdSystemException($message, $sqlState, $detail, $hint),
                'XX' => new ScratchBirdInternalException($message, $sqlState, $detail, $hint),
                default => new ScratchBirdException($message, $sqlState, $detail, $hint),
            };
        }
        return new ScratchBirdException($message, $sqlState, $detail, $hint);
    }

    private static function integrityMessage(string $text): bool
    {
        if ($text === '') {
            return false;
        }
        return str_contains($text, 'constraint violation')
            || str_contains($text, 'duplicate value')
            || str_contains($text, 'duplicate key')
            || str_contains($text, 'primary key')
            || str_contains($text, 'foreign key')
            || str_contains($text, 'not null')
            || str_contains($text, 'unique index violation');
    }

    public static function retryScopeForSqlState(?string $sqlState): string
    {
        // Drivers are fail-closed: fresh statement restart for 40xxx,
        // reconnect only for 08xxx, and no automatic whole-transaction replay.
        if ($sqlState === null || strlen($sqlState) !== 5) {
            return RetryScope::NONE;
        }
        if ($sqlState === '40001' || $sqlState === '40P01') {
            return RetryScope::STATEMENT;
        }
        if (str_starts_with($sqlState, '08')) {
            return RetryScope::RECONNECT;
        }
        return RetryScope::NONE;
    }

    public static function isRetryableSqlState(?string $sqlState): bool
    {
        return self::retryScopeForSqlState($sqlState) !== RetryScope::NONE;
    }
}
