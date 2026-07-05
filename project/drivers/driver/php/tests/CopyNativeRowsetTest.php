<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use PHPUnit\Framework\TestCase;

require_once dirname(__DIR__) . '/tools/sb_isql_php.php';

final class CopyNativeRowsetTest extends TestCase
{
    public function testCanonicalCopyRowsConvertToNativeRowsetFrame(): void
    {
        $frame = copy_text_rows_to_native_frame(
            "id=591;note=copy-alpha;payload_text=payload-a;marker=10\n"
            . "id=592;note=copy-beta;payload_text=payload-b;marker=20\n"
            . "id=593;note=copy-null;payload_text=NULL;marker=30\n"
        );

        $this->assertSame('SBNR', substr($frame, 0, 4));
        $this->assertSame(2, unpack('v', substr($frame, 4, 2))[1]);
        $this->assertSame(3, unpack('P', substr($frame, 8, 8))[1]);
        $this->assertSame(4, unpack('V', substr($frame, 16, 4))[1]);
        $this->assertSame(
            [NATIVE_ROWSET_TYPE_INT32, NATIVE_ROWSET_TYPE_TEXT, NATIVE_ROWSET_TYPE_TEXT, NATIVE_ROWSET_TYPE_INT32],
            array_values(unpack('C4', substr($frame, 20, 4)))
        );
    }

    public function testCanonicalCopyRowsRejectShapeChanges(): void
    {
        $this->expectException(RuntimeException::class);
        $this->expectExceptionMessage('row shape');

        copy_text_rows_to_native_frame("id=1;note=a\nid=2;payload_text=b\n");
    }
}
