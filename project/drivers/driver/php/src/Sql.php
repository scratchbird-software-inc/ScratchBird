<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class Sql
{
    public static function normalize(string $sql, array $params): array
    {
        if (empty($params)) {
            return ['sql' => $sql, 'params' => []];
        }

        if (self::hasNamedParameters($sql)) {
            return self::rewriteNamed($sql, $params);
        }

        if (str_contains($sql, '?')) {
            return self::rewritePositional($sql, $params);
        }

        return ['sql' => $sql, 'params' => array_values($params)];
    }

    public static function normalizeCallable(string $sql, array $params): array
    {
        return self::normalize(self::normalizeCallableSql($sql), $params);
    }

    /**
     * @param array<int, mixed> $params
     * @return array<int, array{sql: string, params: array<int, mixed>}>|null
     */
    public static function splitExecutableStatements(string $sql, array $params): ?array
    {
        if (self::isSingleProceduralDefinition($sql)) {
            return null;
        }
        $statements = self::splitTopLevelStatements($sql);
        if (count($statements) <= 1) {
            return null;
        }
        $out = [];
        foreach ($statements as $statement) {
            $out[] = self::remapStatementParams($statement, $params);
        }
        return $out;
    }

    public static function normalizeCallableSql(string $sql): string
    {
        $trimmed = trim($sql);
        if (!str_starts_with($trimmed, '{') || !str_ends_with($trimmed, '}')) {
            return $sql;
        }
        $inner = trim(substr($trimmed, 1, -1));
        if ($inner === '') {
            return $sql;
        }

        if (preg_match('/^\?\s*=\s*call\s+([\s\S]+)$/i', $inner, $matches) === 1) {
            $parsed = self::parseCallableInvocation(trim($matches[1]));
            if ($parsed === null) {
                throw new \InvalidArgumentException('invalid JDBC escape call syntax');
            }
            [$routine, $args, $hasParens] = $parsed;
            $callArgs = $hasParens ? $args : '';
            return sprintf('select %s(%s) as return_value', $routine, $callArgs);
        }

        if (preg_match('/^call\s+([\s\S]+)$/i', $inner, $matches) === 1) {
            $parsed = self::parseCallableInvocation(trim($matches[1]));
            if ($parsed === null) {
                throw new \InvalidArgumentException('invalid JDBC escape call syntax');
            }
            [$routine, $args, $hasParens] = $parsed;
            if ($hasParens) {
                return sprintf('call %s(%s)', $routine, $args);
            }
            return sprintf('call %s', $routine);
        }

        return $sql;
    }

    private static function hasNamedParameters(string $sql): bool
    {
        $len = strlen($sql);
        $inString = false;
        for ($i = 0; $i + 1 < $len; $i++) {
            $ch = $sql[$i];
            if ($ch === "'") {
                $inString = !$inString;
                continue;
            }
            if ($inString) {
                continue;
            }
            if (self::isNamedParameterStart($sql, $i)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Split SQL into top-level statements on the active terminator.
     *
     * Quote-aware (single/double quotes) and `--` line-comment aware. Honors the
     * `SET TERM <terminator>` client directive:
     * the directive changes the active terminator and is consumed -- it is not
     * emitted as a statement and is not counted in statement indexing. This lets
     * procedural bodies contain inner `;` between `SET TERM ^` and the restoring
     * `SET TERM ;^`.
     *
     * With no `SET TERM` directive present, the behavior reduces to a plain
     * quote-aware top-level `;` split, so existing scripts and statement indices
     * are unchanged.
     *
     * @return array<int, string>
     */
    private static function splitTopLevelStatements(string $sql): array
    {
        $statements = [];
        $term = ';';
        $current = '';
        $len = strlen($sql);
        $inString = false;
        $inDouble = false;

        $flush = static function () use (&$current, &$term, &$statements): void {
            $chunk = trim($current);
            $current = '';
            if ($chunk === '') {
                return;
            }
            $newTerm = self::chunkSetTerm($chunk);
            if ($newTerm !== null) {
                $term = $newTerm;
                return;
            }
            $statements[] = $chunk;
        };

        for ($i = 0; $i < $len;) {
            $ch = $sql[$i];
            if (!$inString && !$inDouble && $ch === '-' && $i + 1 < $len && $sql[$i + 1] === '-') {
                // `--` line comment: copy verbatim to end of line without scanning
                // for the terminator or quotes inside it.
                $eol = strpos($sql, "\n", $i);
                if ($eol === false) {
                    $eol = $len;
                }
                $current .= substr($sql, $i, $eol - $i);
                $i = $eol;
                continue;
            }
            if ($ch === "'" && !$inDouble) {
                $inString = !$inString;
                $current .= $ch;
                $i++;
                continue;
            }
            if ($ch === '"' && !$inString) {
                $inDouble = !$inDouble;
                $current .= $ch;
                $i++;
                continue;
            }
            if (!$inString && !$inDouble && $term !== '' && substr_compare($sql, $term, $i, strlen($term)) === 0) {
                $matchedLen = strlen($term); // capture before flush(), which may change $term
                $flush();
                $i += $matchedLen;
                continue;
            }
            $current .= $ch;
            $i++;
        }
        $flush();
        return $statements;
    }

    /**
     * Return the new terminator if `$chunk` is a `SET TERM <terminator>` client
     * directive, else `null`. Leading full-line `--` comments and blank lines are
     * ignored when matching, so a directive may be preceded by comment lines.
     */
    private static function chunkSetTerm(string $chunk): ?string
    {
        $meaningful = [];
        foreach (preg_split('/\r\n|\r|\n/', $chunk) as $line) {
            $stripped = trim($line);
            if ($stripped === '' || str_starts_with($stripped, '--')) {
                continue;
            }
            $meaningful[] = $stripped;
        }
        if ($meaningful === []) {
            return null;
        }
        if (preg_match('/^set\s+term\s+(\S.*?)\s*$/i', implode(' ', $meaningful), $matches) === 1) {
            return trim($matches[1]);
        }
        return null;
    }

    private static function isSingleProceduralDefinition(string $sql): bool
    {
        $trimmed = self::trimLeadingLineComments($sql);
        return preg_match(
            '/^create\s+(?:or\s+replace\s+)?(?:trigger|function|procedure)\b/i',
            $trimmed
        ) === 1;
    }

    private static function trimLeadingLineComments(string $sql): string
    {
        $lines = preg_split('/\r\n|\r|\n/', $sql);
        if ($lines === false) {
            return ltrim($sql);
        }
        $offset = 0;
        $count = count($lines);
        while ($offset < $count) {
            $line = ltrim($lines[$offset]);
            if ($line === '' || str_starts_with($line, '--')) {
                $offset++;
                continue;
            }
            break;
        }
        return ltrim(implode("\n", array_slice($lines, $offset)));
    }

    private static function rewriteNamed(string $sql, array $params): array
    {
        $lookup = [];
        foreach ($params as $key => $value) {
            if (is_string($key)) {
                $lookup[ltrim($key, '@:')] = $value;
            }
        }
        $ordered = [];
        $out = '';
        $len = strlen($sql);
        $inString = false;
        for ($i = 0; $i < $len;) {
            $ch = $sql[$i];
            if ($ch === "'") {
                $inString = !$inString;
                $out .= $ch;
                $i++;
                continue;
            }
            if (!$inString && self::isNamedParameterStart($sql, $i)) {
                $j = $i + 1;
                while ($j < $len && (ctype_alnum($sql[$j]) || $sql[$j] === '_')) {
                    $j++;
                }
                $name = substr($sql, $i + 1, $j - $i - 1);
                if (!array_key_exists($name, $lookup)) {
                    throw new \InvalidArgumentException("missing named parameter: {$name}");
                }
                $ordered[] = $lookup[$name];
                $out .= '$' . count($ordered);
                $i = $j;
                continue;
            }
            $out .= $ch;
            $i++;
        }
        return ['sql' => $out, 'params' => $ordered];
    }

    private static function rewritePositional(string $sql, array $params): array
    {
        $ordered = [];
        $out = '';
        $len = strlen($sql);
        $inString = false;
        $index = 0;
        for ($i = 0; $i < $len;) {
            $ch = $sql[$i];
            if ($ch === "'") {
                $inString = !$inString;
                $out .= $ch;
                $i++;
                continue;
            }
            if (!$inString && $ch === '?') {
                if (!array_key_exists($index, $params)) {
                    throw new \InvalidArgumentException('not enough parameters');
                }
                $ordered[] = $params[$index];
                $index++;
                $out .= '$' . count($ordered);
                $i++;
                continue;
            }
            $out .= $ch;
            $i++;
        }
        if ($index < count($params)) {
            throw new \InvalidArgumentException('too many parameters');
        }
        return ['sql' => $out, 'params' => $ordered];
    }

    /**
     * @param array<int, mixed> $params
     * @return array{sql: string, params: array<int, mixed>}
     */
    private static function remapStatementParams(string $sql, array $params): array
    {
        if ($params === []) {
            return ['sql' => $sql, 'params' => []];
        }

        $len = strlen($sql);
        $inString = false;
        $inDouble = false;
        $out = '';
        $remap = [];
        $orderedIndexes = [];
        for ($i = 0; $i < $len;) {
            $ch = $sql[$i];
            if ($ch === "'" && !$inDouble) {
                $inString = !$inString;
                $out .= $ch;
                $i++;
                continue;
            }
            if ($ch === '"' && !$inString) {
                $inDouble = !$inDouble;
                $out .= $ch;
                $i++;
                continue;
            }
            if (!$inString && !$inDouble && $ch === '$' && $i + 1 < $len && ctype_digit($sql[$i + 1])) {
                $j = $i + 1;
                while ($j < $len && ctype_digit($sql[$j])) {
                    $j++;
                }
                $originalIndex = (int) substr($sql, $i + 1, $j - $i - 1);
                if (!isset($remap[$originalIndex])) {
                    $remap[$originalIndex] = count($orderedIndexes) + 1;
                    $orderedIndexes[] = $originalIndex;
                }
                $out .= '$' . $remap[$originalIndex];
                $i = $j;
                continue;
            }
            $out .= $ch;
            $i++;
        }

        $ordered = [];
        foreach ($orderedIndexes as $originalIndex) {
            if ($originalIndex < 1 || !array_key_exists($originalIndex - 1, $params)) {
                throw new \InvalidArgumentException('parameter count mismatch');
            }
            $ordered[] = $params[$originalIndex - 1];
        }
        return ['sql' => $out, 'params' => $ordered];
    }

    private static function isNamedParameterStart(string $sql, int $index): bool
    {
        $len = strlen($sql);
        if ($index < 0 || $index + 1 >= $len) {
            return false;
        }

        $marker = $sql[$index];
        if ($marker !== ':' && $marker !== '@') {
            return false;
        }
        if (!ctype_alpha($sql[$index + 1])) {
            return false;
        }
        if ($marker === ':' && $index > 0 && $sql[$index - 1] === ':') {
            return false;
        }
        return true;
    }

    /**
     * @return array{0: string, 1: string, 2: bool}|null
     */
    private static function parseCallableInvocation(string $text): ?array
    {
        $openParen = strpos($text, '(');
        if ($openParen === false) {
            $routine = trim($text);
            if ($routine === '') {
                return null;
            }
            return [$routine, '', false];
        }

        $inSingle = false;
        $inDouble = false;
        $depth = 0;
        $closeParen = -1;
        $len = strlen($text);
        for ($i = $openParen; $i < $len; $i++) {
            $ch = $text[$i];
            if ($ch === "'" && !$inDouble) {
                $inSingle = !$inSingle;
                continue;
            }
            if ($ch === '"' && !$inSingle) {
                $inDouble = !$inDouble;
                continue;
            }
            if ($inSingle || $inDouble) {
                continue;
            }
            if ($ch === '(') {
                $depth++;
                continue;
            }
            if ($ch === ')') {
                $depth--;
                if ($depth === 0) {
                    $closeParen = $i;
                    break;
                }
            }
        }

        if ($closeParen < 0) {
            return null;
        }
        $routine = trim(substr($text, 0, $openParen));
        if ($routine === '') {
            return null;
        }
        $trailing = trim(substr($text, $closeParen + 1));
        if ($trailing !== '') {
            return null;
        }
        $args = trim(substr($text, $openParen + 1, $closeParen - $openParen - 1));
        return [$routine, $args, true];
    }
}
