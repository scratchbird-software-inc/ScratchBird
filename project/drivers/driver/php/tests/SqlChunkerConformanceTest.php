<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use PHPUnit\Framework\TestCase;
use ScratchBird\PDO\Sql;

/**
 * Cross-driver statement-chunker conformance. The PHP splitter MUST reproduce
 * `expected` exactly for every `input` in the shared fixture, mirroring
 * tests/conformance/drivers/chunker_conformance/verify_python_reference.py.
 */
final class SqlChunkerConformanceTest extends TestCase
{
    private const FIXTURES = [
        __DIR__ . '/../../../../tests/conformance/drivers/chunker_conformance/cases.json',
        __DIR__ . '/../../../../../project/tests/conformance/drivers/chunker_conformance/cases.json',
        __DIR__ . '/../../../../../../project/tests/conformance/drivers/chunker_conformance/cases.json',
    ];

    /**
     * @return array<int, mixed>
     */
    private function cases(): array
    {
        $path = false;
        foreach (self::FIXTURES as $fixture) {
            $path = realpath($fixture);
            if ($path !== false) {
                break;
            }
        }
        $this->assertNotFalse($path, 'conformance fixture not found');
        $data = json_decode((string) file_get_contents($path), true, 512, JSON_THROW_ON_ERROR);
        return $data['cases'];
    }

    /**
     * @param string $input
     * @return array<int, string>
     */
    private function split(string $input): array
    {
        $method = new ReflectionMethod(Sql::class, 'splitTopLevelStatements');
        $method->setAccessible(true);
        /** @var array<int, string> $result */
        $result = $method->invoke(null, $input);
        return $result;
    }

    public function testEveryConformanceCaseSplitsExactly(): void
    {
        foreach ($this->cases() as $case) {
            $this->assertSame(
                $case['expected'],
                $this->split($case['input']),
                'chunker conformance case failed: ' . $case['name']
            );
        }
    }
}
