// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// chunkerFixture mirrors the cross-driver conformance fixture at
// tests/conformance/drivers/chunker_conformance/cases.json.
type chunkerFixture struct {
	SchemaVersion int `json:"schema_version"`
	Cases         []struct {
		Name     string   `json:"name"`
		Input    string   `json:"input"`
		Expected []string `json:"expected"`
	} `json:"cases"`
}

// TestSplitTopLevelStatementsConformance loads the shared cross-driver chunker
// fixture and asserts the Go splitter reproduces every expected statement list.
func TestSplitTopLevelStatementsConformance(t *testing.T) {
	// The Go driver package lives at project/drivers/driver/go; the fixture lives
	// at project/tests/conformance/drivers/chunker_conformance/cases.json.
	fixturePath := filepath.Join(
		"..", "..", "..", "tests", "conformance", "drivers",
		"chunker_conformance", "cases.json",
	)

	data, err := os.ReadFile(fixturePath)
	if err != nil {
		t.Fatalf("read fixture %s: %v", fixturePath, err)
	}

	var fixture chunkerFixture
	if err := json.Unmarshal(data, &fixture); err != nil {
		t.Fatalf("parse fixture: %v", err)
	}
	if len(fixture.Cases) == 0 {
		t.Fatalf("fixture contained no cases")
	}

	for _, tc := range fixture.Cases {
		tc := tc
		t.Run(tc.Name, func(t *testing.T) {
			got := SplitTopLevelStatements(tc.Input)
			// Normalize an empty (nil) result to an empty slice for comparison.
			if got == nil {
				got = []string{}
			}
			want := tc.Expected
			if want == nil {
				want = []string{}
			}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("case %q mismatch\n input:    %q\n expected: %#v\n got:      %#v",
					tc.Name, tc.Input, want, got)
			}
		})
	}

	t.Logf("chunker conformance: %d/%d cases passed", len(fixture.Cases), len(fixture.Cases))
}
