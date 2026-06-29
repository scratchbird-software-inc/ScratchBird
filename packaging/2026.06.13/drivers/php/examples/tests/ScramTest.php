<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

require_once __DIR__ . '/bootstrap.php';

use PHPUnit\Framework\TestCase;
use ScratchBird\PDO\Scram;

final class ScramTest extends TestCase
{
    public function testScramSha512ClientFinalContainsProof(): void
    {
        $scram = new Scram('alice', Scram::ALGORITHM_SHA512);
        $clientFirst = $scram->clientFirstMessage();
        $this->assertMatchesRegularExpression('/^n,,n=alice,r=.+$/', $clientFirst);

        preg_match('/r=([^,]+)/', $clientFirst, $matches);
        $this->assertNotEmpty($matches[1] ?? '');
        $serverFirst = 'r=' . $matches[1] . 'server,s=' . base64_encode('salt') . ',i=4096';

        $clientFinal = $scram->handleServerFirst('secret', $serverFirst);
        $this->assertStringStartsWith('c=biws,r=' . $matches[1] . 'server,p=', $clientFinal);
        $this->assertGreaterThan(0, strlen(substr($clientFinal, strrpos($clientFinal, 'p=') + 2)));
    }
}
