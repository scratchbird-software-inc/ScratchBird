// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package main

import (
	"encoding/binary"
	"testing"
)

func TestCopyTextRowsToNativeFrameCanonicalRows(t *testing.T) {
	frame, err := copyTextRowsToNativeFrame([]byte(
		"id=591;note=copy-alpha;payload_text=payload-a;marker=10\n" +
			"id=592;note=copy-beta;payload_text=payload-b;marker=20\n" +
			"id=593;note=copy-null;payload_text=NULL;marker=30\n",
	))
	if err != nil {
		t.Fatalf("copyTextRowsToNativeFrame failed: %v", err)
	}
	if string(frame[:4]) != "SBNR" {
		t.Fatalf("native frame magic mismatch: %q", frame[:4])
	}
	if version := binary.LittleEndian.Uint16(frame[4:6]); version != 2 {
		t.Fatalf("native frame version = %d, want 2", version)
	}
	if rowCount := binary.LittleEndian.Uint64(frame[8:16]); rowCount != 3 {
		t.Fatalf("row count = %d, want 3", rowCount)
	}
	if columnCount := binary.LittleEndian.Uint32(frame[16:20]); columnCount != 4 {
		t.Fatalf("column count = %d, want 4", columnCount)
	}
	gotTypes := []byte{frame[20], frame[21], frame[22], frame[23]}
	wantTypes := []byte{
		nativeRowsetTypeInt32,
		nativeRowsetTypeText,
		nativeRowsetTypeText,
		nativeRowsetTypeInt32,
	}
	for index := range wantTypes {
		if gotTypes[index] != wantTypes[index] {
			t.Fatalf("type[%d] = %d, want %d", index, gotTypes[index], wantTypes[index])
		}
	}
}

func TestCopyTextRowsToNativeFrameRejectsShapeChange(t *testing.T) {
	_, err := copyTextRowsToNativeFrame([]byte(
		"id=1;note=a\n" +
			"id=2;payload_text=b\n",
	))
	if err == nil {
		t.Fatal("expected shape-change error")
	}
}
